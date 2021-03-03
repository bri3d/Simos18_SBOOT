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
54 -> A0 XX XX XX XX 00 01 36 followed by 0x100 bytes of Seed material. This seems to configure the public key with identifier "0x0136" that will be used to verify the BSL, then uses the Mersenne Twister seeded with the system timer, encrypts the generated data using the RSA Public Key 0x136, then sends the encrypted data to the tester. XX XX XX XX is the addr of pubkeykey in ECU memory for whatever reason. I suppose this is useful if a factory tool has a large keychain for different ECUs and needs to know what it's working with.
65 ??x100 -> A0 Key Response. The ECU expects the tester to be able to decrypt the random data encrypted using the public key and send it back. This, of course, requires the corresponding private key. 
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

Since CAN messages are buffered and processed in a loop pumped using the same timer, with some plausibly tight timing it should be quite possible to "hit" a very close range of Mersenne Twister seed numbers. Because the random data which is encrypted and passed back is not dynamically salted at all, it should be possible to pass back the desired data and pass the seed/key algorithm, using a small table of Seed/Key data generated with https://github.com/bri3d/TC1791_CAN_BSL/blob/main/twister.c . So far, my testing indicates that even with a Raspberry Pi as the tester, the system timer value can be constrained to within a few hundred thousand cycles, reducing the size of the Seed/Key table which needs to be generated.

Next, we can hopefully use the poor bounds-checking on the CRC to begin a checksum process against the boot passwords as stored in Flash. By then quickly resetting the CPU into BSL, before the process has a chance to run too many iterations, a reasonably small amount of data should be processed before we are able to inspect the state of the checksum routine. And by sliding the start of the CRC, it should be possible to determine the influence of each set of bytes on the CRC and to back-calculate the bytes in question, revealing the boot passwords.

# Happy Path

If the "Recovery" break-in doesn't occur at this stage, first a check is made against Flash at 80840500 to see if a "CBOOT_temp" awaits promotion. If it does, the CRC is validated and the CBOOT_temp is copied into the "normal" CBOOT area. SBOOT responds with some hardcoded Wait responses on CAN to keep the tester alive, and then the new CBOOT is loaded.

If no "CBOOT_temp" awaits promotion, the Valid flags (H$B) of the "main" CBOOT are checked (notably, not its CRC), a strange set of commands is sent to the Flash controller, and then CBOOT is executed.

If no Valid flags are set for "main" CBOOT, the same recovery shell is entered.

