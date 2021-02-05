# About

This project provides documentation about the Supplier Bootloader (SBOOT) provided in Simos18 ECUs, with a focus on the OTP/Customer Protection module supplied by VW AG in VW vehicles.

For more background about Simos18 in general and the later-stage (Customer Bootloader/CBOOT) exploit used for unsigned code execution, please see [my other documentation](https://github.com/bri3d/VW_Flash/blob/master/docs.md).

The SBOOT is the first part of the ECU's trust chain after the CPU mask ROM. It starts at 0x80000000 (the beginning of Program Memory) and has a few basic responsibilities:

* Early-stage startup of the Tricore CPU's peripherals and clock initialization and re-sync.
* Validation of the "Validity flags" for CBOOT, the next bootloader stage.
* Promotion of a customer-supplied CBOOT update (CBOOT_temp) into the CBOOT area, after validating the "validity flags" and CRC checksum.
* Access to a supplier "recovery" backdoor. This takes the form of an ISO-TP over CAN "command shell" which is accessed if CBOOT is invalid or a specific timing of rising edges is met on two PWM pins. This "emergency recovery" mechanism seems to be loosely shared across many modern ECUs, although the signal specifics and CAN commands seem to differ between different hardware. In Simos18, this command shell is accessed using two square-edge signals running at the same frequency but with a specific phase shift. This command shell allows the upload of an RSA-signed bootstrap loader which is executed with Flash unlocked for Write access (unlike the native Tricore BSL, which is unsigned but cannot access Flash at all). The signature-checking functionality is provided by the customer-supplied One Time Programming area starting at 80014000, and the RSA signature is validated against a public key with identifier `0x136` which is the third public key in the OTP area.

Startup pretty much happens in this order, as well. First, SBOOT sets up a large number of peripheral registers using initliazation data hardcoded in Flash. Next, SBOOT initalizes the main clocks, importantly, setting the PLL base frequency for timers to 8.75Mhz.

Next, SBOOT sets up CAN message handlers, using the standard UDS channel IDs 0x7E0 and 0x7E8, and then sets up the "GPTA" comparator peripheral on the Tricore to measure two signals for "break-in." The two signals are measured as follows:

# Recovery Shell Details (GPT/PWM/"PW")

Signal 1 triggers a reset of a GPTA timer at each rising edge.
Signal 2 triggers an interrupt at each rising edge.

When the Signal 2 interrupt is triggered, two measurements are made:
* The GPTA timer counter value. This measures the distance between posedge(Sig1) and posedge(Sig2).
* The CPU central timer counter difference between the last triggered interrupts. This measures the distance between posedge(Sig2) and posedge(Sig2).

These two measurements are checked to be < 0x80A and < 0xAAA, respectively. Given the 8.75Mhz GPT and STM clocks, this yields 235.2uS for the Sig1->Sig2 measurement and 312uS for the Sig2->Sig2 measurement, setting the base frequency to 3.2kHz and the phase shift to roughly 1/4.

Please see the included Python script for a way to set these two frequencies up using pigpiod's WavePWM on the Raspberry Pi. Please note that the outputs from the Pi need to be level shifted to be registered by the Tricore in Simos18.

If the PWM signals match, two CAN messages are checked. First, a message `59 45` (`80002b70`). If this message is present, `0xA0` is returned. Next, a message `6B` is expected (`80004a48`), to which `0xA0` is again returned. If these conditions are met, SBOOT will jump into the "recovery shell" awaiting setup of a bootstrap loader.

The recovery shell now sets up ISO-TP framing and expects the following messages:

```
TESTER -> EXPECTED REPLY

7D 41 43 57 -> A0, sets the "current state" to 0xA which seems to exit the command shell.
7D 4E 42 30 ("NB0") -> A0, just a part number check? Doesn't seem to alter any other state.

6B -> A0 02. This is the SBOOT version of TesterPresent, works regardless of state.
30 ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? ?? (???) -> A0 . This one is weird. It checks that byte 3 is equal to a value at 8000081C, but there's nothing there... It then sets two bytes at D0000010 to the first two bytes of the payload. Not sure if this is for engineering ECUs or what. All 0s seems to work.
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

There is an exploit in this mechanism which allows exfiltration of the boot passwords stored at `8001420c`. I am currently researching how this works - by my reading of the SBOOT disassembly. Since the CRC header is only bounds-checked in one direction, my suspicion is that there is a sign-extension / overflow issue in the CRC length calculation or a timing exploit where the CRC base address can be set to 8001420c and the ECU can be rebooted before it bus faults while the checksum value still resides in RAM at `d0010778`.

There may also be an exploit in the RSA validation itself, but I believe this to be less likely as the RSA validation methods are stored in OTP and are additional used for all validation of CBOOT-provided reflashes, so an exploit here would be usable generally.

I am currently trying to figure out whether any earlier stages of the process are exploitable, or whether there is an issue in the seed/key validation that I have not yet found - because at least in theory, the seed/key validation seems quite secure as encrypting the random data with the public key and expecting it back decrypted should require knowledge of the private key.

# Happy Path

If the "Recovery" break-in doesn't occur at this stage, first a check is made against Flash at 80840500 to see if a "CBOOT_temp" awaits promotion. If it does, the CRC is validated and the CBOOT_temp is copied into the "normal" CBOOT area. SBOOT responds with some hardcoded Wait responses on CAN to keep the tester alive, and then the new CBOOT is loaded.

If no "CBOOT_temp" awaits promotion, the Valid flags (H$B) of the "main" CBOOT are checked (notably, not its CRC), a strange set of commands is sent to the Flash controller, and then CBOOT is executed.

If no Valid flags are set for "main" CBOOT, the same recovery shell is entered.

