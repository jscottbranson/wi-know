# Firmware configuration

**Edit `settings.json`.** It is the configuration consumed by the build.
`settings.example.json` is a reference copy of the initial defaults. All paths in
settings are relative to the repository root unless absolute. Detection rules
live separately in `../rules/`.

`settings.parent-wiring.example.json` is a separate N32R8 parent peripheral
profile matching the [LED/buzzer/LCD/GPS/microSD wiring diagram](../docs/wiring.md#parent-leds-buzzer-screen-and-gps).
It enables those peripherals and changes parent bus RX to GPIO3, avoiding the
onboard USB-UART TX connected to GPIO12. Check the board revision before using it with
`-DWIKNOW_CONFIG=/absolute/path/settings.parent-wiring.example.json`.

Validate without an ESP32 toolchain:

```sh
python3 tools/generate_config.py
```

The build generates a typed C++ header with no runtime JSON parser. Settings and
rules are compiled into firmware; changing either requires rebuilding/reflashing.
The generator rejects unknown/missing settings, wrong types, duplicate JSON keys,
invalid channels, duplicate rule IDs, invalid predicates, UART conflicts, duplicate
active GPIOs, and enabled peripherals with unassigned GPIOs.

The RS-485 bus is enabled on UART1 with TX GPIO11 (D6), RX GPIO12 (D7), and
RTS GPIO7 (D3). GPS, SD, LCD, buzzer and LEDs remain disabled with GPIO `-1`.
BLE is enabled on the parent. GPIO numbers are **SoC GPIO numbers**, not
XIAO `D` labels. The validator checks numeric bounds/conflicts, not your board's
reserved pins or exposed pads. Confirm the parent board and wiring before enabling.

| Section | Key settings |
| --- | --- |
| `node` | Role `parent`/`child`/`follower`; parent ID 0, other IDs unique in 1–16 |
| `wifi` | US receive channel allowlist and dwell milliseconds; initial policy excludes DFS |
| `ble` | Passive BLE scan switch, used by parent and follower |
| `bus` | RS-485 UART/pins/baud, discoverable node ID range, poll timeout, membership/assignment leases |
| `detection` | Rules directory and enabled categories |
| `follower` | Enable target commands; release after no fresh matching observation |
| `gps` | Air530 UART (9600 baud), maximum fix age, capture-time uncertainty limit |
| `sd` | HW-125 SPI pins and conservative 4 MHz initial clock |
| `logging` | `matches` or `all`; flush period, rotation bytes, number of retained logs |
| `display` | I²C pins/address and page period; 16×2 PCF8574 backpack driver |
| `buzzer` | PWM pin, frequency, duration, global alert cooldown |
| `signal_trend` | Three active-high LED pins, median window, samples, deadband and confirmations |

For multiple boards, use one config with CLI role/ID overrides or pass a separate
config with `-DWIKNOW_CONFIG=/absolute/path/settings.json`. Parent peripherals are
only initialized in parent firmware. Each worker's node ID must be in the parent's
configured bus range. All nodes should share the country/channel policy. Use USB
Serial/JTAG for the console, leaving UART0 for GPS and UART1 for the bus. Firmware
currently targets ESP32-C5; a different parent chip needs a platform port.

The default LCD address 39 (`0x27`) and PCF8574 mapping are assumptions to verify.
Do not infer HW-125 supply voltage from its header labels. The RS-485 bus requires
3.3 V-compatible transceivers, common ground, correct termination/biasing, and RTS
wired to the transceiver's transmit-enable/receive-enable control. Bare UART TX
pins must not be tied together. Transport is optional until this hardware exists.

GPS supplies UTC and receiver position; its onboard backup battery is recorded in
configuration. The first driver trusts active, checksum-valid RMC date/time and
coordinates. Battery-retained time availability without a fix still requires a
hardware check. No external RTC driver or fabricated UTC fallback is present.
