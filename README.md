# About

This project provides documentation about the Supplier Bootloader (SBOOT) provided in Simos18 ECUs, with a focus on the OTP/Customer Protection module supplied by VW AG in VW vehicles.

For more background about Simos18 in general and the later-stage (Customer Bootloader/CBOOT) exploit used for unsigned code execution, please see [my other documentation](https://github.com/bri3d/VW_Flash/blob/master/docs.md).

# High Level Summary

A bit of background: All ECUs based on Infineon Tricore MCUs are protected, fundamentally, by a simple mechanism: the Flash memory is inside of the processor module itself, and the processor's Flash controller protects the memory from Read and Write using passwords stored inside of User Configuration Blocks in the CPU.
Tricore CPUs do allow the user to run arbitrary code natively, using a set of hardware pins which configure the CPU to load a Bootstrap Loader (BSL) over the CAN bus. However, an attacker or tuner cannot read or write the Flash memory without either an exploit for the factory flashing process, or access to these passwords, because when the Bootstrap Loader starts, Flash is locked using these passwords. In older Tricore based ECUs, these passwords were fixed or generated algorithmically, but in modern ECUs like Simos18, manufacturers wised up and the passwords are generated randomly and burned into a One Time Programmed area during end-of-line manufacturing. So, there's a chicken-and-egg problem: an attacker needs to read Flash to get the passwords back out, but they can't read Flash without the passwords...

So, the attack that needs to be found is a read primitive for an arbitrary part of Flash memory.

On Simos18 for VW AG, there is a simple exploit for the factory flashing process which allows arbitrary write access to Flash memory and therefore arbitrary code execution, so this password access is not as important for tuning - but, there are still some reasons to want it: the factory flashing process won't work if the ECU's immobilizer is active, including if you bought a junkyard ECU to play with or to repair your own car with. The factory flashing exploit also won't work if you bricked the car using a bad software patch, or shot yourself in the foot by installing software for the wrong vehicle. So, having the passwords is still useful.

Most modern ECUs have what's called SBOOT: a Supplier Bootloader which is provided by the ECU manufacturer (i.e. Continental). This Supplier Bootloader is used to write ECUs during the manufacturing process, and to inspect and un-brick ECUs sent into a manufacturer for repair.
This kind of low-level access differs almost entirely between manufacturers - for example, from what I know, Bosch use standard protocols like UDS and CCP, while Continental have a custom command-set on top of ISO-TP.

Anyway, here is how the exploit process works.

First, the Simos18 boot process needs to be interrupted so it arrives in SBOOT, rather than loading up CBOOT (Customer Bootloader) and then the normal ASW (Application Software) to run the car. This is done the same way it is on ECUs from other manufacturers: by sending a custom PWM waveform to two pins on the ECU harness, which the ECU measures at boot time using the GPTA logic analyzer built into the Tricore procesor. In Simos18, this is accomplished by expecting a 235.2uS Pin1 -> Pin 2 timing measurement and a 312uS Pin 2 -> Pin 2 timing measurement, meaning two 3.2Khz signals phase-shifted 1/4 from each other.

If these two signals are present, the ECU knows it's time to go into the Supplier Bootloader. Next, the ECU expects some CAN messages on the normal Powertrain CAN pins to tell it it REALLY is time to go into SBOOT, and then it loads up the SBOOT command processor and an ISO-TP stack.

Next, the SBOOT command processor expects some custom commands sent over ISO-TP, which ideally are used to upload a manufacturer-provided custom bootloader. Where things really get interesting is that there's a Seed/Key process before any extended access is allowed to the command processor.
The Seed/Key process looks secure on the surface. The ECU sends 256 bytes of psuedo-random (Mersenne Twister) data encrypted using an RSA public key (M ^ E mod N) to the client and expects the client to send the decrypted data back. Given how RSA works, this SHOULD require the private key, but there is a fatal mistake:
The ECU does not seed the Seed/Key process with real OR enough entropy (randomness). There are two mistakes: the random number generator is seeded with the system timer, which is not a source of entropy because it behaves predictably, and the random number generator is only seeded with 31 bits (231 possible values) worth of data, which can easily be calculated on a modern processor in seconds to minutes.

This process could be exploited by carefully controlling the timing of the Seed message to always cause the same message to be sent (there is a countermeasure against this, where Seed isn't accepted until the ECU has been running for a few hundred ms, but it's a weak countermeasure because it's still a fixed time value). Or it could be exploited by generating a huge rainbow table of all possible values. But the easiest way to exploit it is a combination: to send the Seed message at a predictable time, and then brute-force over a small keyspace until we find a Key message which encrypts to match the Seed. https://github.com/bri3d/Simos18_SBOOT/blob/main/twister.c

Now that we are past the Seed/Key process and the PWM access process, we need an arbitrary read exploit. The SBOOT command shell is designed to allow a manufacturer provided code block to be uploaded into RAM and executed, but, that code block is signed with the same RSA validation process used by all Simos18 code blocks (the block data is mixed with address headers to prevent relocation attacks, and then SHA-256 checksummed, and then the SHA-256 is encoded into a standard PKCS#1 RSA signature which is attached to the block). This process is secure as far as I can tell and nobody has exploited it. So we can't gain arbitrary code execution with a valid block. But, we just need a read primitive, and it turns out there is one:

In addition to the RSA signature, all Simos18 code blocks are also checked against a simple CRC checksum. And, in this case, SBOOT tries to validate the CRC checksum before it validates the RSA signature. It turns out the CRC validation is performed using an address header, and the address header has weak bounds checking. So, we can ask the ECU to checksum the boot passwords, and send us the checksum back. Since CRC is not a cryptographic hash (it is reversible), we can use the CRC values to back-calculate the boot passwords. Unfortunately, we can't do this cleanly, because the high side of the address range IS correctly bounds-checked. So instead, we have to start the CRC checksum process, then rapidly reboot into the Tricore Bootstrap Loader and dump the contents of RAM, to see the intermediate/temporary CRC values that have been calculated for only the boot passwords. https://github.com/bri3d/TC1791_CAN_BSL/blob/main/bootloader.py#L113

I know this write-up was long, but I hope this helps readers gain a deeper knowledge of the kinds of exploit chains which are present in semi-modern ECUs. At a base level, this isn't too different from the kinds of protection and exploits used in mobile devices and game consoles. I hope that by sharing this exploit chain (PWM timer -> Seed/Key timing and bruteforce -> CRC bounds-checking Reset/RGH exploit), I can help those getting into ECUs from another kind of device to recognize the similarities and gain interest in ECU reverse engineering.

As for my process: I loaded up SBOOT in Ghidra using a register map I built using available TriCore toolchain information: https://github.com/bri3d/ghidra_tc1791_registers . Then I literally just, well, read the code. The GPT timing comparisons were easy to find from access to the GPTA registers, but it took a long time to realize the base frequency the PLLs were locked at (8.75Mhz) to translate the cycle-timing back into real timing.

Then, the Seed/Key process was trivial to figure out (Mersenne Twister is full of constants which are easy to Google, and the cryptography routines are in a separate library inside the same binary so it is easy to tell what's going on), but looked so secure in concept that it took some time to recognize the weak seed data.

Next, I had to very carefully replicate the process used to encrypt the Seed data, which had a few weird nuances (one byte is just set to 0 for seemingly no reason). I ran the binary through Tricore emulated in QEMU, which helped a lot since I could dump RAM and inspect the state of various buffers quickly, although I could have done the same using a Bootstrap Loader.

After the Seed/Key process was figured out, the CRC exploit was very obvious as the bounds checking is blatantly broken. https://github.com/resilar/crchack came in handy to take the CRC + unknown data and back-generate the correct data used to produce the CRC.

I wish I could share my annotated disassembly for the greater good, but sharing manufacturer binaries is unfortunately not a great idea, so I will have to live with this description.

# Detailed Documentation

The SBOOT is the first part of the ECU's trust chain after the CPU mask ROM. It starts at 0x80000000 (the beginning of Program Memory) and has a few basic responsibilities:

* Early-stage startup of the Tricore CPU's peripherals and clock initialization and re-sync.
* Validation of the "Validity flags" for CBOOT, the next bootloader stage.
* Promotion of a customer-supplied CBOOT update (CBOOT_temp) into the CBOOT area, after validating the "validity flags" and CRC checksum.
* Access to a supplier "recovery" backdoor. This takes the form of an ISO-TP over CAN "command shell" which is accessed if CBOOT is invalid or a specific timing of rising edges is met on two PWM pins. This "emergency recovery" mechanism seems to be loosely shared across many modern ECUs, although the signal specifics and CAN commands seem to differ between different hardware. In Simos18, this command shell is accessed using two square-edge signals running at the same frequency but with a specific phase shift. This command shell allows the upload of an RSA-signed bootstrap loader which is executed with Flash unlocked for Write access (unlike the native Tricore BSL, which is unsigned but cannot access Flash at all). The signature-checking functionality is provided by the customer-supplied One Time Programming area starting at 80014000, and the RSA signature is validated against a public key with identifier `0x136` which is the third public key in the OTP area.

Startup pretty much happens in this order, as well. First, SBOOT sets up a large number of peripheral registers using initliazation data hardcoded in Flash. Next, SBOOT initalizes the main clocks, importantly, setting the PLL base frequency for timers to 8.75Mhz.

# Happy Path

If a "Recovery" break-in doesn't occur at this stage, first a check is made against Flash at 80840500 to see if a "CBOOT_temp" awaits promotion. If it does, the CRC is validated and the CBOOT_temp is copied into the "normal" CBOOT area. SBOOT responds with some hardcoded Wait responses on CAN to keep the tester alive, and then the new CBOOT is loaded.

If no "CBOOT_temp" awaits promotion, the Valid flags (H$B) of the "main" CBOOT are checked (notably, not its CRC), a strange set of commands is sent to the Flash controller, and then CBOOT is executed.

If no Valid flags are set for "main" CBOOT, the recovery shell is entered.

# Recovery Shell Entry Details (GPT/PWM/"PW")

SBOOT sets up CAN message handlers, using the standard UDS channel IDs 0x7E0 and 0x7E8, and then sets up the "GPTA" comparator peripheral on the Tricore to measure two signals for "break-in." The two signals are measured as follows:

Signal 1 triggers a reset of a GPTA timer at each rising edge.
Signal 2 triggers an interrupt at each rising edge.

When the Signal 2 interrupt is triggered, two measurements are made:
* The GPTA timer counter value. This measures the distance between posedge(Sig1) and posedge(Sig2).
* The CPU central timer counter difference between the last triggered interrupts. This measures the distance between posedge(Sig2) and posedge(Sig2).

These two measurements are checked to be < 0x80A and < 0xAAA, respectively. Given the 8.75Mhz GPT and STM clocks, this yields 235.2uS for the Sig1->Sig2 measurement and 312uS for the Sig2->Sig2 measurement, setting the base frequency to 3.2kHz and the phase shift to roughly 1/4.

Please see the included Python script for a way to set these two frequencies up using pigpiod's WavePWM on the Raspberry Pi. Note that the outputs from the Pi need to be level shifted up to 5V to be registered by the Tricore in Simos18.

<img width="325" src="https://user-images.githubusercontent.com/210793/110823789-ab209a80-824f-11eb-80a8-bbac9875b787.png">

If the PWM signals match, two CAN messages are checked. 

First, a message `59 45` (`80002b70`). If this message is present, `0xA0` is returned. Next, a message `6B` is expected (`80004a48`), to which `0xA0` is again returned. If these conditions are met, SBOOT will jump into the "recovery shell" awaiting setup of a bootstrap loader.

# Recovery Shell Tasks

The recovery shell now runs four tasks.

* The first task is an ISO-TP command REPL based on a state machine, documented below.
* The second task appears to be part of the ISO-TP message pump logic for that command REPL. It also handles soft rebooting the ECU with reason code `0x200` if the REPL's state machine is set to 10dec (0xA), which occurs if a specific password is sent to the "7D" command. I am not yet sure if `0x200` as the restart reason is handled in a special way.
* The third task runs from RAM at d001fd80 . It services the Tricore Watchdog Timer and bit-bangs a fixed message out to the Port3 GPIO. It's not yet certain what the purpose of the bit-bang is - perhaps it is servicing an external watchdog peripheral, or is for debugging. More investigation is required. This is definitely a side-channel through which information can be exfiltrated (as the watchdog is serviced outside of the usual timer schedule before certain flash tasks), but probably isn't the source of an existing exploit. This also increments a timer value which is used in the main REPL to require a delay before requesting Seed/Key authentication. 
* The fourth task just counts up.

The recovery shell now sets up ISO-TP framing and expects the following messages:

```
TESTER -> EXPECTED REPLY

7D 41 43 57 -> A0, sets the "current state" to 0xA which seems to exit the command shell.
7D 4E 42 30 ("NB0") -> A0, just a part number check? Doesn't seem to alter any other state.

6B -> A0 02. This is the SBOOT version of TesterPresent, works regardless of state.
30 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? (???) -> A0 . This one is weird. It checks that int 3 is equal to a value at 8000081C, but there's nothing there... It then sets two words at D0000010 to the first two words of the payload. All 0s seems to work. This command is required to esclate the state from 0 to 2 to enable the 54 command to be accessed.
54 -> A0 XX XX XX XX 00 01 36 followed by 0x100 bytes of Seed material. This configures the Public Key with ID "0x0136", then uses a Mersenne Twister seeded with the system timer to generate 256 random bytes, encrypts the generated data using the RSA Public Key 0x136, and sends the encrypted data to the tester.
65 ??x100 -> A0 Key Response. The ECU expects the tester to be able to decrypt the random Seed data which was encrypted using the public key and send it back. This, of course, requires the corresponding private key, unless the value can be guessed or generated another way (which it can!). 
78 AA AA AA AA XX ... -> Set Address -> Value. Value is a varargs length based on the framing length from CAN. AA AA AA AA is a bounds-checked offset, which will be translated to an address between B0010000 and B0015000. This command be used any number of times to set up the BSL in RAM.
```

The BSL structure which is set up using the `78` command looks like so:   

```
B0010000 -> CRC Seed / Start Value
B0010004 -> Expected CRC Value
B0010008 -> CRC Range Count - Checked against 1
B001000C -> CRC Start Address - Bounds checked to be <= 0xb0010131
B0010010 -> CRC End Address - Bounds checked to be >= 0xb001012f
B0010130 -> BSL entry address pointer - loaded after signature check, and execution jumps there. 
```
One final command is available:

`79 -> Validate and execute BSL.` This first copies the CRC header into a RAM structure for CRC validation. Next, it iterates through the CRC checksum, 0x100 bytes at a time, and then checks it against the stored value. Finally, it calls the "validate signature" function in the OTP cryptography library, which generates the SHA256 of the bootloader, checks it against the sum embedded in the RSA signature, and if everything matches, Flash is unlocked and then the entry point is executed in RAM.

# Exploit Methodology

This exploit revolves around a simple mistake: the Mersenne Twister PRNG used to generate the random Key data is seeded using the value of the System Timer (STM0) register.

So, we know that the Mersenne Twister seed number is deterministic, since no entropy is correctly introduced: it is seeded off of the system timer only, which is not an entropy source since with the same inputs, the CPU should produce the same outputs. Furthermore the seed is passed through `| 1` (odds only), so it is an even more poor source of entropy - the keyspace is only 2^31, and the realistic keyspace is very small due to the use of a deterministic source in the form of the timer.

Since CAN messages are also buffered and processed in a loop pumped using the same timer, with some plausibly tight timing it is possible to "hit" a very close range of Mersenne Twister seed numbers. And, because the random data which is encrypted and passed back is not dynamically salted at all, it is possible to pass back the desired data by performing the generation algorithm again outside of the ECU, which gives us access to both the random data (Key) and encrypted random data (Seed). By brute-forcing until we find Key data which produces a matching Seed, we can pass the seed/key algorithm with [this tool](twister.c) .

So far, my testing indicates that even with a Raspberry Pi as the tester, having very poor timing constraints, the system timer value can be reproduced to within a few hundred thousand cycles by sending CAN messages in the same order. This reduces the size of the Seed/Key space to a size which can be easily brute-forced on modern hardware. On my M1 MacBook Air, I can enumerate roughly 500,000 pairs per second across all cores. There is a trivial protection against this attack in that the bootloader requires 100 iterations of the watchdog service routine to have executed before it will allow the Seed to be generated, but again, this is deterministic and doesn't actually represent a protection (it just makes an attack slightly harder).

Next, we can use the poor bounds-checking on the CRC header to begin a checksum process against the boot passwords as stored in Flash. By then quickly resetting the CPU into BSL, before the process has a chance to run multiple iterations, we are able to inspect the state of the checksum routine. And by sliding the start of the CRC, it is possible to determine the influence of each set of bytes on the CRC and to back-calculate the bytes in question, revealing the boot passwords.

# Exploit Step-By-Step

The exploit process has been proven out, but not yet automated. So far, the process works like this:

Clone this repository and https://github.com/bri3d/TC1791_CAN_BSL into sibling directories.

Install OpenMP (Optional, if installed and compiled twister will be multicore) and GNU MP (gmp, Required) as well as their development headers.

Compile `twister.c` using something like `gcc twister.c -o twister -O3 -march=native -mtune=native -mcpu=native -Xpreprocessor -fopenmp -lgomp -lgmp` .

Set up ECU for Bootstrap Loader using https://github.com/bri3d/TC1791_CAN_BSL as a guide. Unfortunately, you will need to open the ECU and probe or solder to several very small vias. This is necessary to allow the use of the CPU RST pin, as well as the upload of a low-level Tricore Bootstrap Loader to read the CRC data out of RAM and access Flash once passwords are recovered.

Correctly configure SocketCAN and CAN-ISOTP for your Raspberry Pi hardware and kernel.

Connect all pins and CAN hardware.

Use "bootloader.py" from that same repository to perform the following steps:

```
~/TC1791_CAN_BSL $ python3 bootloader.py
Welcome to Tricore BSL. Type help or ? to list commands, you are likely looking for upload to start.

(BSL) upload
(...)
Device jumping into BSL... Draining receive queue...
```

The SBOOT entry process is much more reliable when starting from the Bootstrap Loader rather than running ASW. I believe this is due to CAN buffering in Linux getting in the way of the entry messages.

Next:

# Manual Process

```
(BSL) sboot
Setting up PWM waveforms...
Resetting ECU into Supplier Bootloader...
Sending 59 45...
Timestamp: 1615681990.162002        ID: 0004    S E              DLC:  8    00 08 00 00 00 00 68 00     Channel: can0
Timestamp: 1615681990.162629        ID: 0004    S E              DLC:  8    00 20 00 00 00 00 80 00     Channel: can0
Timestamp: 1615681990.166569        ID: 0004    S E              DLC:  8    00 08 00 00 00 00 7f 00     Channel: can0
Timestamp: 1615681990.166708        ID: 07e8    S                DLC:  8    a0 ff ff ff ff ff ff ff     Channel: can0
Got A0 message
Sending 6B...
Timestamp: 1615681990.168746        ID: 07e8    S                DLC:  8    a0 02 ff ff ff ff ff ff     Channel: can0
Got A0 message
Switching to IsoTP Socket...
Sending 0x30 to elevate SBOOT shell status...
Success
Sending 0x54 Generate Seed...
Success
Calculating key for seed: 
60b107433dc4972bcabbdb2f4d43b0e7eb27a223b1108826babe820753738614e9ad973a51d672a961a911f6360e1e30bdff139b24108ad167f687d26e98edf252392baf86290ddfb92823d237bc6fd4186dc9ba72c5a2c81df8c4524da1b3d8f1d7dee13f2d272430732cc0ef946c1cf3c6ad0d2463db93a9e1d20c76af696682b092225db5683263aec2c792dbfe9ce586874ef1768159355e4d501cf1a8a4a0a4aae15590f56a10c0acd1140e299a5e9a3adf1887484ae59a4620a3a0355afadce9a01c512fe75c5cde1a5dfd2e306513360eb15d7349591bd69916ab977b2e01e874f1ede7da51168f711e672c92463b0c7097a32479746d7af3a8f28ca2
Key calculated : 
AD358BA56451FEF5B5B54B202BC15A4FC4B0CF541272B8DF7FEAFE47E11996BD690AF4A350EA4FDE960FA159C08B6592FDE22BDDEAEE2D74C20D5DF5F609BE14E07E10810AE4AC904610DAC0E5A0F554855AC67CBD2139130EA70B4E7E860BB87BFDC08E3AC27FD4037F9372060E00880721CFF80E479D578EAE72C853E6641EF9D06494B7B4AE7D8AF0795290EDEEC3FB417038E628961583725808F32E72E109C8C5837E762EA370102823F46CD1ED504B79869BB3A3CB1244F93D667B14F759465B5CFD2F9015651603B61E0A17FB44B2646D13AC3D6C6DF9471B12C27BD5310906C50CE1659997CC8EB441C49D3CBA599181F07700EFF875785700025B56
Sending 0x65 Security Access with Key...
Success
```

There's the Seed data, produced by encrypting unknown Key data generated using the Mersenne Twister PRNG. This was recovered back into Key data by brute-forcing Seed values until we found a match. The timing is controlled enough that the timer value should be somewhere around 01D00000. If this value is incorrect for your setup, it could take a long time or be impossible to find a match (it should take 10-15 seconds tops). In this case I recommend running `twister` on faster hardware with the starting value set to `0`, to see what the correct "start" value for your hardware is:

```
Simos18_SBOOT % ./twister 00000000 be284541
**** FOUND ****
Seed: 01D1E4EE

Key Data: 
 DA0E1828 21C7E6B5 A2BBD143 12F85EFD 9E682543 FE14518B 7611F711 8249AF0E A3C3E370 771EFA0B 172B321E 1B709E32 D7217199 48621483 23B90A28 DFA71B4A 925B0A34 1D304CC9 A6E36B73 70DBDCD5 EA05EDE2 800BB6E0 17793D5C 1A338EF0 AD960625 89C66E02 061D10DE FFDD040D EC59123B E833FDB6 B745EF44 2D32DCF0 81F1CFA8 28B0BD61 E351C52A C7A6F379 5E91489C 4C872318 5888FD89 2C1A3106 DFF03E42 1342237F 3963F5F2 180FCF17 23306BC3 EF5A73EF F734D53A 8B1AC3AD 6CFA8BE3 A2120DF7 38B5B940 181EB813 8816814A 68B604A6 F17221AA A6117FF3 70A251B5 997520F8 EC472D57 092B8960 E5A644C8 6AC5007F 6CEFE33A 0002405E
Seed Data: 
 BE284541 338A69A6 BF21CAA3 69D3079A 815F9D5C 71359E92 2495BC27 DBE86E6E 441702D8 1E07CA31 8A98B2E3 F83BED6F 7E91F3CE 375A9703 013284E8 DE83343D E688D33F 5E8D4BF2 5F9CAE3B 191A8167 43C32A59 67414C24 9658E421 EA040B5D F7B30B9E 69C0A647 6653F729 DB1C7567 5CE71C09 04BE57A6 94C43E24 3468118B A8CDC1E4 B6C23A73 C115EEDE 9950051A 239D9759 8707779E 14C8EC45 84E2A9E1 55CADFC8 D526D778 66047070 B13D3405 7CF311B9 FA6D9FF5 223026FD AA4B7223 96C57B78 58BC290E 4320D80C 4C1B31DC D1E02CB5 E2EC7B4A 1AA18FB7 68A10342 328AD5C5 E042A8CD FEBA7607 161308AE ABC71C89 A819AE6A 43A2FA3B 4CF45E49
 ```
 
Here, we use `twister` from this repo manually to generate the Seed/Key data. Again, this is only necessary if the automated process above took too long (it should take ~10 seconds on a Pi 3B+). 

The two parameters are 00000000, the initial timer value to iterate up from, and BE284541, the first 32 bits of the Seed from the above commands. Again, this is only necessary if your tester has different timings from mine (I used a Raspberry Pi 3B+), in which case you may need to increase or decrease the 01D00000 value in `bootloader.py` to find a match in a reasonable amount of time. The faster the machine you can run `twister` on for this process, the better. It's multithreaded and grossly parallel with OpenMP, so if you find a big enough machine you could iterate the whole keyspace in a manner of minutes. It takes about a minute to iterate up from 0 to 01D1E4EE on my M1 MacBook Air. Once you have found one Seed/Key pair this way, you can put a number close to the `Seed` int back into the `bootloader.py` script to speed things up.

Now, we go back to the `bootstrap.py` script.

Next up, we'll calculate the CRC for just 0x100 bytes of data anywhere in Flash, by sending a CRC header with the lower bound set to the location we wish to checksum, then quickly resetting the CPU after just one iteration of the CRC loop:

```
((BSL) sboot_crc_reset 8001421C
Resetting ECU into HWCFG BSL Mode...
Setting initial CRC to 0x0...
Success
Setting expected CRC to 0x0...
Success
Setting start CRC range count to 1...
Success
Setting start CRC start address to boot passwords at 8001421c...
Success
Setting start CRC end address to a valid area at 0xb0010130...
Success
Uploading valid part number for part correlation validator...
Success
Starting Validator and rebooting into BSL...
Sending BSL initialization message...
Sending BSL data...
100%|██████████████████████████████████████████████████████████████████████████████████████████████████████| 621/621 [00:00<00:00, 745blocks/s]
Device jumping into BSL... Draining receive queue...
CRC Address Reached: 
0x8001431c
CRC32 Current Value: 
0x4e5cecf4
```

Here, we upload a small fake code block to the SBOOT command shell, consisting of a CRC header telling the shell to begin CRC checking at a specified address, 0x8001420C, and some fixed part number data necessary to pass a validator.

Next, we read back the CRC and end address (to make sure only a single iteration of the CRC algorithm was allowed to progress). Now we have the CRC value for 0x8001421C-0x8001431C! An astute reader will note that this range is actually after the boot passwords, and contains some crypto data from OTP (the Mersenne Twister seed and the beginning of the AES K-values, I think) - this lets us verify that our system is working, since we can checksum the known data and compare the result.

We copy the known data from this address in an SBOOT ROM dump, and calculate its checksum - it's a match, so we know the mechanism works!

```
crchack % ./crchack -x 00000000 -i 00000000 -w 32 -p 0x4c11db7 ../Simos-Crypto/21C.bin
4e5cecf4
```

Now, to actually recover passwords, we need to repeat this process, sliding the checksum value backwards 4 bytes at a time. Each time we slide the checksum value backwards, we can use https://github.com/resilar/crchack to generate the necessary data to match the checksum. By performing this process iteratively 4 bytes at a time, sliding back towards the boot passwords at 0x8001420C, we can infer their value.

```
crchack % ./crchack -x 00000000 -i 00000000 -w 32 -p 0x4c11db7 -b :4 ../Simos-Crypto/218.bin 0x4F2527F4 > 218.WRITE_PASSWORD_2.bin
```

The data at 80014200-8001420C is also known - it's the unique ID of the MCU, which is available as the output of the "deviceid" command in this BSL shell. So, I believe you could also complete this attack by sliding the CRC region forward rather than backwards using that known data rather than the data out of an SBOOT ROM dump.

# Automatic Process Example

```
$ python3 bootloader.py 
Welcome to Tricore BSL. Type help or ? to list commands, you are likely looking for upload to start.

(BSL) extract_boot_passwords
[... , device will start 4 times to find 4 CRC values, should take ~2 minutes]
CRC32 Current Value: 
0xf427254f
80014218 - 0x80014318 -> 0xf427254f
abf425508513c27314e31d3542b92b1b
(BSL) send_read_passwords abf42550 8513c273
(BSL) dumpmem AF000000 18000 PMU0_DFlash.bin
(BSL) dumpmem AF080000 18000 PMU1_Dflash.bin
(BSL) dumpmem 80000000 200000 PMU0_PFlash.bin
(BSL) dumpmem 80800000 100000 PMU1_PFlash.bin

```

If everything works (I recommend testing a single iteration using the step-by-step instructions above), the `crchack` and reset process has now been automated to dump boot passwords with a single command - the last line of output is the passwords. This relies on the seed start-value being valid for your setup (see above around `twister`) and everything being connected properly. The four example `dumpmem` commands will "back up" your ECU and produce a "bench read" - the PMU PFlash and DFlash. You *cannot* currently use these to clone from ECU to ECU - the OTP area at 80014000 is married to the PMU ID and boot passwords, and the DFlash is encrypted using the PMU ID as part of the initialization material.
