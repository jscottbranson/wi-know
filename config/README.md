# Firmware configuration

**Edit `settings.json`.** It is the configuration consumed by the build.
`settings.example.json` is a reference copy of the initial defaults. All paths in
settings are relative to the repository root unless absolute. Detection rules
live separately in `../rules/`.

The default role is `universal` with ID 0 as an unassigned build placeholder.
`settings.universal.example.json` enables all parent peripherals with GPS RX on
GPIO26, reserving GPIO10 for the passive identification harness. Flash the same
configuration onto all boards. See [universal provisioning](../docs/universal.md).

`settings.parent-wiring.example.json` is a separate **manual legacy** N32R8 parent peripheral
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

Universal mode selects UART1 TX GPIO11, RTS GPIO7 and parent RX GPIO3 or
worker RX GPIO12 before initializing the bus. GPS, SD, LCD, buzzer and LEDs remain disabled with GPIO `-1`.
BLE is enabled on the parent. GPIO numbers are **SoC GPIO numbers**, not
XIAO `D` labels. The validator checks numeric bounds/conflicts, not your board's
reserved pins or exposed pads. Confirm the parent board and wiring before enabling.

| Section | Key settings |
| --- | --- |
| `node` | Default `universal`, ID 0 placeholder; explicit manual `parent`/`child`/`follower` remain available |
| `universal` | Optional verified hardware profile: strap GPIO10; parent bus 11/3/7; worker bus 11/12/7 |
| `wifi` | US receive channel allowlist and dwell milliseconds; initial policy excludes DFS |
| `ble` | Passive BLE scan switch, used by parent and follower |
| `bus` | RS-485 UART/pins/baud, discoverable node ID range, poll timeout, membership/assignment leases |
| `detection` | Rules directory and enabled categories |
| `follower` | Enable target commands; release after no fresh matching observation |
| `gps` | Air530 UART (9600 baud), maximum fix age, capture-time uncertainty limit |
| `sd` | SPI pins and conservative 4 MHz initial clock; automatic destructive `format_if_mount_failed` (default true; false opts out) |
| `logging` | `matches` or `all`; flush period, rotation bytes, retained files; flash fallback caps apply |
| `display` | I²C pins/address and page period; 16×2 PCF8574 backpack driver |
| `buzzer` | PWM pin, frequency, duration, global alert cooldown |
| `signal_trend` | Optional active-high LED pins (`-1` omits each LED), median window, samples, deadband and confirmations |

With `signal_trend.enabled: true` and all three LED pins wired, a fresh matching
signal keeps one LED lit. The steady LED lights while trend samples accumulate;
increasing/decreasing trends select their respective LEDs. All LEDs turn off
after `signal_trend.stale_ms` without a fresh target observation. Unmatched
observations logged in `all` mode do not light these LEDs.

For multiple boards, flash the same universal config and binary onto every board.
Only detected parents initialize the configured peripherals. The generator checks
the strap against active parent peripheral pins and the selected parent bus pins.
The optional `universal` section defaults to the verified profiles above; changing
those pins requires a reviewed hardware port, not a per-board build override.
Manual role/ID overrides disable universal provisioning and require an isolated
legacy bus. Use USB Serial/JTAG for the console, UART0 for GPS and UART1 for RS-485.

The 5 V PCF8574 LCD backpack is configured at address 39 (`0x27`); verify its
address and pin mapping against the driver configuration.
Parents try enabled SD storage at boot, then fall back to internal flash if SD is
disabled or unavailable. See [storage selection, limits and readback](../docs/storage.md).
Unrecognized SD filesystems are automatically formatted by default; see the
[`sd.format_if_mount_failed` option](../docs/storage.md#preparing-an-unformatted-sd-card).
It may erase a damaged or unrecognized filesystem; turn it off again after setup.
The documented HiLetgo SD reader needs 5 V at module VCC and uses 3.3 V SPI signals. The RS-485 bus requires
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

## Standalone parent

Universal parents use automatic local fallback with the bus enabled; see
[the universal scheduling policy](../docs/universal.md). The following configuration
is the optional **manual**, bus-disabled standalone mode.

For a single board, set `standalone.enabled` to `true` and `bus.enabled` to
`false`, keeping role `parent` and ID `0`. Or build with
`-DWIKNOW_CONFIG=/absolute/path/config/settings.standalone.example.json`.
The example leaves peripherals disabled; copy your board's verified peripheral
settings to retain GPS, storage, screen, LEDs and buzzer. For a mini XIAO with
**screen → GPS → microSD → buzzer → LEDs** in priority order, use
[`settings.standalone-wiring.example.json`](settings.standalone-wiring.example.json)
and the [standalone wiring and selection guide](../docs/wiring.md#standalone-mini-xiao-esp32-c5).
This profile enables all five: LCD on GPIO23/24, receive-only GPS on GPIO12,
SPI microSD on GPIO8/9/10 with CS GPIO11, buzzer on GPIO7, and LEDs on GPIO1/0/25.
All 11 header GPIOs fit without changing the previous indicator wiring. For the
compact screen/GPS/storage build, disable `buzzer` and `signal_trend`. Disable
each other omitted peripheral through its `enabled` setting before rebuilding.
Set individual omitted trend LED pins to `-1`; they are not initialized or driven.
Use the native USB console; UART0 is reserved for GPS in this profile. The example
keeps `logging.mode: "matches"`; choose `"all"` for a capture check with no rules.

Flash the example with the normal helper (after activating ESP-IDF):

```sh
python3 tools/flash.py --config config/settings.standalone.example.json --role parent --node-id 0
```

The parent visits each configured Wi-Fi channel for `wifi.dwell_ms`, then scans
BLE for `standalone.ble_dwell_ms` (default 120 ms), repeating through the channel
list. Both scans are passive. Wi-Fi reception is paused during BLE windows and
BLE scanning is cancelled before tuning Wi-Fi. With `ble.enabled: false`, it hops
Wi-Fi continuously. Settings are compiled in; rebuild and flash after changes.
Old configs without a `standalone` section retain distributed operation.

This trades coverage for needing only one board: with the example’s 19 channels,
500 ms Wi-Fi dwells and 120 ms BLE windows, each Wi-Fi channel is revisited roughly
every 11.78 seconds,
plus switching/processing overhead. BLE also has scan interval/window gaps within
its allotted time. Packets outside a radio/channel's window are missed. Increase
following-alert gap and signal-staleness settings if needed for the longer sweep.
The screen shows the current radio/channel or a scan error; a failed tune is
retried on the next sweep. No children or dedicated follower are used. Observations
still flow through the normal parent rules, GPS tagging, tracking, logs and alerts.
Physical ESP32-C5 validation of switching and reception remains necessary.

## Detection category defaults

All shipped profiles enable the [expanded detection pack](../rules/EXPANDED.md),
except `ble_trackers` (Tile, SmartTag, Apple Find My/iBeacon) and `unifi_cameras`.
These are explicitly listed in `detection.disabled_categories` as examples of
default-disabled matches. To enable one, remove it from that list and add it to
`detection.enabled_categories`, then rebuild/reflash. Listing a category in both
places is a validation error. Existing configs may omit `disabled_categories`.
Category selection controls classifications and rule actions, not radio reception.
Unifi/Ubiquiti OUIs also identify non-camera equipment. Generic camera keywords
can independently match the same device even when its vendor category is off.
