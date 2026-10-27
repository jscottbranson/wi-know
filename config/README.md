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
The generated directory also contains `settings.rules.json`: a portable rule
manifest retaining the exact predicates, source provenance and effective enabled
state after category filtering. Keep it with exported logs so rule IDs can be
traced back to the signatures used by that firmware build. The generator reports
enabled categories that have no enabled rules; selecting a category does not
install detection signatures.
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

## Following-device alerts

Set `following_alert.enabled` to `true` in `settings.json`, enable and wire GPS,
and rebuild/flash the parent. This works without detection rules, including in
`matches` logging mode. Bluetooth support covers BLE advertisements only.

```json
"following_alert": {
  "enabled": true,
  "distance_m": 200,
  "delay_ms": 30000,
  "max_gap_ms": 15000,
  "ignore_devices": [
    {"radio": "wifi", "address_type": 0, "address": "AA:BB:CC:DD:EE:FF"},
    {"radio": "ble", "address_type": 1, "address": "11:22:33:44:55:66"}
  ]
}
```

Use addresses from observation logs. Wi-Fi uses address type 0; BLE uses 0 for
public and 1 for random addresses. Ignore entries match the full address, radio,
and address type and suppress only following alerts, not rule alerts.

For each unfamiliar device, the first valid GPS-tagged sighting establishes the
receiver's starting location. The delay starts at the first further sighting at
least `distance_m` away from that location (straight-line displacement, not path
length). Further sightings must remain beyond that radius for `delay_ms` before
an alert is emitted. Returning inside the radius restarts the delay. Sightings
may be at most `max_gap_ms` apart; a longer gap or an observation without a valid
GPS fix resets the encounter. Set this gap above the expected scan/channel sweep
time. Delayed or duplicate reports do not advance timers, and stale buffered
captures cannot trigger an alert. Zero delay alerts on the first qualifying
sighting after the distance threshold is crossed.

Each continuous encounter produces one `following_alert` JSON event over serial
and SD when available, a 30-second LCD message, and a buzzer pulse when enabled
(subject to the shared buzzer cooldown). Configure `buzzer.enabled` and its GPIO
for audible alerts. A new encounter can alert again after a sighting gap. Up to
64 identities are tracked; the least recently seen identity is replaced when full.
State is cleared on reboot.

This is a movement correlation heuristic: GPS locates the receiver, and the
configured radius is an assumed plausible radio range. GPS error and long-range
transmitters can produce false alerts. Randomized Wi-Fi/BLE addresses can break
continuity and require ignore-list updates; rotating identities are not linked.
