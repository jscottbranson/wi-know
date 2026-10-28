# One image for every board

Build and flash the same universal image and configuration on the N32R8 parent
and every XIAO ESP32-C5. Do not pass per-board role or ID arguments:

```sh
python3 tools/flash.py --config config/settings.universal.example.json --port /dev/ttyACM0
```

Repeat with each USB port. `settings.json` also builds a universal image, with
parent peripherals disabled. The universal example enables the documented LCD,
GPS, LEDs, buzzer and microSD. Keep the generated rules manifest with the image.
For repeatable deployment, build once and flash that build directory to each
board; record the SHA-256 of `wi_know.bin` and read back/verify each flash.

## Passive parent identification

The image does **not** infer hardware from an ordinary read of an LED or buzzer
output, or from any device acknowledging an I²C address. Instead, add a passive
identification contact to each of the LCD, buzzer and LED wiring harnesses. All
three contacts share the reserved GPIO10 identification input. Any one intact
identification branch identifies the parent, so unplugging one or two peripheral
harnesses does not change its role. Broken display electronics, a failed buzzer,
or a burned-out LED do not affect the passive contact.

| Connection | Universal wiring |
| --- | --- |
| GPIO10 on every board | 10 kΩ to local GND; input only during detection |
| Each of three parent harness ID branches | Local 3V3 → BAT54 Schottky diode anode; cathode → 1 kΩ → common GPIO10 |
| Parent GPS TX | **GPIO26**, replacing the manual drawing's GPIO10 |
| Parent RS-485 TX / RX / RTS | GPIO11 / GPIO3 / GPIO7 |
| XIAO RS-485 TX / RX / RTS | GPIO11 / GPIO12 / GPIO7 |
| Every RS-485 transceiver DE and `/RE` | Tied together, GPIO7, plus 10 kΩ pull-down to keep transmit disabled through reset |

Use 3.3 V only for the identification branches, never the LCD/buzzer's 5 V rail.
Route identification as separate contacts/wires in each harness; do not connect
GPIO10 to I²C, PWM, LED-drive outputs or GPS TX. The diode isolates branches;
with a 10 kΩ pull-down and one 1 kΩ branch, confirm the input is above the chip's
specified VIH on the actual hardware. All boards require the pull-down, including
workers. A parent with all three harnesses disconnected reads as a worker unless
the persistent parent override is set. A broken common ID wire or short to ground
cannot be distinguished from a worker; use the override for recovery.

Firmware waits 20 ms, samples the input 16 times at intervals of at least 2 ms (rounded up to
the FreeRTOS tick, normally 10 ms), and requires
all samples to agree. Mixed samples halt startup with a diagnostic rather than
selecting a profile. No unknown peripheral is driven during this process. Only
after selecting the profile are UART1 and parent peripherals initialized. GPIO26
is never configured for GPS on a worker; on the XIAO it is battery-sense control.

The pin choices follow the [Seeed XIAO pin map](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
and [Waveshare N32R8 schematic](https://files.waveshare.com/wiki/ESP32-C5-WIFI6-KIT-NXRX/ESP32-C5-WIFI6-KIT-NXRX-Schematic.pdf):
GPIO10 is XIAO D10 and is exposed on the parent, while parent GPIO26 is available
for GPS RX. Parent GPIO12 is connected to the USB-UART bridge output and must not
be used for transceiver RX. GPIO6 is battery sensing, GPIO13/14 are native USB,
GPIO15 is unavailable on the PSRAM module, GPIO27 drives the onboard RGB LED and
GPIO28 is BOOT; these are not identification pins. GPIO0/1 retain the existing
requirement that the parent's optional 32 kHz crystal is unpopulated.
The existing [wiring illustrations](wiring.md) show the manual profile: apply the
GPS relocation and identification branches above before using this image.

## Provisioning and recovery

Hardware identification is the normal path. An optional NVS `u8` key `identity`
in namespace `wiknow` overrides it: `0` (or absent) samples hardware, `1` forces
parent, `2` forces worker. Any other value halts startup. Overrides take precedence
over the strap and do not change the image, so recovery does not need a separate
firmware build. Set overrides only after confirming the board and its wiring;
forcing parent on a worker would enable the parent peripheral profile.

Provision with Espressif's NVS partition generator and the matching partition
table offset/size, using a CSV such as:

```csv
key,type,encoding,value
wiknow,namespace,,
identity,data,u8,1
```

Read the NVS offset and size from the generated partition table rather than
assuming an address. Preserve existing NVS data if needed; writing a replacement
NVS partition replaces its other contents. Restore automatic detection by writing
`identity=0` or removing that key. Normal application flashing does not erase NVS.
No worker role, short address or lease is saved there: reboot obtains a fresh
assignment, avoiding duplicate saved addresses. Factory MAC identity is unchanged
by NVS reset. Never erase factory eFuses.

## Compatibility

Explicit `--role parent`, `--role child --node-id N`, and
`--role follower --node-id N` retain manual provisioning for an isolated legacy
bus. Do not mix manually addressed and universal nodes. Universal discovery uses
new packet types and session identity checks; older peers cannot participate.
CRC protects against corruption, not a malicious device on the local bus.

See [discovery protocol and scheduling](discovery-protocol.md) for assignment,
leases, collision recovery, follower selection and local scan policy.

## Physical acceptance procedure

Host tests and CI compilation do not demonstrate electrical or radio behavior.
The following procedure is required on the real boards; no physical result is
claimed by this implementation.

1. With power off, verify every pin, common ground, 3.3 V transceiver logic,
   DE pull-downs, end termination and bus biasing. Confirm GPS moved to GPIO26.
   Disconnect LCD/buzzer power while checking the ID branches for isolation.
2. Flash one exact binary to the parent and at least three XIAOs. Record its
   SHA-256, board factory MACs and serial startup logs. Use no role/ID overrides.
3. Boot the parent with each harness alone, each pair, and all three. Disconnect
   or simulate a nonfunctional peripheral while leaving its ID contact connected.
   Confirm parent selection and RX GPIO3 every time. Verify all-disconnected
   behavior and force-parent recovery. Induce an unstable strap and confirm halt.
4. Boot each worker with its ID pull-down and confirm RX GPIO12, no parent
   peripheral initialization, and silence until a parent response opportunity.
5. Power all boards simultaneously repeatedly. Capture the RS-485 traffic and
   confirm unique short addresses, explicit acknowledged roles, two children and
   one follower, and retries resolving collided discovery responses. Repeat with
   hot-plug/reconnect and with an intentionally noisy response slot.
6. Fill the configured ID range, attach another board, and confirm capacity
   diagnostics without duplicate allocation. Remove a board, let its lease
   expire, and confirm replacement allocation. Exercise same-MAC/different-boot
   conflicts in a simulator; never alter factory MAC eFuses to test this.
7. Reboot workers and then the parent independently, replay stale polls and
   assignments, and verify expired sessions cannot capture or send new reports.
   Verify already admitted buffered observations survive reassignment and drain
   after a valid session returns. RAM observations cannot survive worker reboot.
8. Attach a second identified parent, including simultaneous boot and hot-plug.
   Verify the implemented conflict policy prevents continued competing bus control
   and exposes the conflict in serial/UI. Resolve by disconnecting/reprovisioning
   the unintended parent, then use the documented recovery path.
9. Start with no workers and observe local Wi-Fi/BLE phases while discovery
   continues. Join one worker, then the other children and follower; remove them
   again. Verify local/cluster transitions, queue preservation and truthful tuned
   coverage. A planned channel must not count as tuned until reported successful.
10. Repeat with missing/broken LCD, SD, GPS, LEDs and buzzer. Identification must
    depend on passive contacts or override, not successful peripheral operation.
    Record radio tuning failures, discovery failures, assignment leases, conflicts
    and scan coverage in the test report, together with hardware revisions.
