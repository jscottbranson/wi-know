# Firmware configuration

Choose a configuration for the installation and select the hardware with `--board`:

| Installation | Configuration | Board flag |
| --- | --- | --- |
| Standalone N32R8 | `settings.standalone.example.json` | `--board n32r8` |
| Standalone XIAO | `settings.standalone.example.json` | `--board xiao` |
| Parent N32R8 | `settings.parent.example.json` | `--board n32r8` |
| Child XIAO | `settings.child.example.json` | `--board xiao` |

Standalone and parent configurations enable LCD, GPS, microSD, buzzer, trend LEDs,
and the capture switch. The standalone configuration disables RS-485; the parent
and child configurations enable it. Child nodes have no parent peripherals or
capture switch. Flash the same child image to every XIAO; the parent assigns IDs
from `bus.first_node_id` through `bus.last_node_id` at runtime. Leave `node.id`
at 0 and omit `--node-id`. The parent also selects scanning/follower roles using
`follower.enabled`. Update all bus nodes together when replacing fixed-ID firmware.

The duplicate wiring/capture examples have been consolidated into these three
role configurations. `settings.json` remains the default for existing universal
builds, with parent peripherals and capture control disabled. Universal firmware
requires its separate [identification harness](../docs/universal.md).

All paths in settings are relative to the repository root unless absolute.
Detection rules live separately in `../rules/`.

Optional `"board": {"model": "xiao"}` or `"board": {"model": "n32r8"}` selects
the corresponding documented GPIO mapping for manual roles. `custom` (the default)
keeps the pins in the JSON. `tools/flash.py --board` and `WIKNOW_BOARD` override the
stored choice. Profile pins replace JSON pins before conflict validation; enable
flags and standalone/bus settings are preserved. Use `custom` for custom wiring or
universal images. See [board selection](../docs/flashing.md#select-the-board-wiring).

| Peripheral | XIAO | N32R8 manual parent |
| --- | --- | --- |
| LCD SDA / SCL | 23 / 24 | 4 / 5 |
| GPS RX / TX | 12 / 0 | 10 / 8 |
| SD SCK / MOSI / MISO / CS | 8 / 10 / 9 / 11 | 23 / 24 / 2 / 25 |
| Buzzer | 7 | 9 |
| LEDs increasing / decreasing | 1 / 25 | 0 / 1 |
| Bus TX / RX / RTS | 11 / 12 / 7 | 11 / 3 / 7 |

All values are SoC GPIO numbers. The XIAO full peripheral profile requires the bus
to be disabled because it shares those pins. The N32R8 profile uses the manual
schematic's GPS RX GPIO10, not the universal harness's GPIO26.

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
| `node` | Default `universal`, ID 0 placeholder; explicit `parent`/`child`/`follower` builds also use parent-assigned worker IDs |
| `universal` | Optional verified hardware profile: strap GPIO10; parent bus 11/3/7; worker bus 11/12/7 |
| `wifi` | US receive channel allowlist and dwell milliseconds; includes DFS (52–64 and 100–144 in steps of 4) and channel 165 |
| `ble` | Passive BLE scan switch, used by parent and follower |
| `bus` | RS-485 UART/pins/baud, discoverable node ID range, poll timeout, membership/assignment leases |
| `detection` | Rules directory and enabled categories |
| `follower` | Enable target commands; release after no fresh matching observation |
| `gps` | Air530/Air530Z UART (9600 baud), maximum fix age, capture-time uncertainty limit |
| `sd` | SPI pins and conservative 4 MHz initial clock; automatic destructive `format_if_mount_failed` (default true; false opts out) |
| `logging` | `matches` or `all`; flush period, rotation bytes, retained files; flash fallback caps apply |
| `display` | I²C pins/address and page period; 16×2 PCF8574 backpack driver |
| `buzzer` | PWM pin, frequency, duration, global alert cooldown |
| `signal_trend` | Optional active-high LED pins (`-1` omits each LED), median window, samples, deadband and confirmations |

With `signal_trend.enabled: true` and both LED pins wired, a fresh matching
signal lights both LEDs while trend samples accumulate or the trend is steady;
increasing/decreasing trends select their respective LEDs. All LEDs turn off
after `signal_trend.stale_ms` without a fresh target observation. Unmatched
observations logged in `all` mode do not light these LEDs.

For multiple boards, flash the same universal config and binary onto every board.
Only detected parents initialize the configured peripherals. The generator checks
the strap against active parent peripheral pins and the selected parent bus pins.
The optional `universal` section defaults to the verified profiles above; changing
those pins requires a reviewed hardware port, not a per-board build override.
Explicit role overrides disable harness detection but retain discovery and
parent-assigned worker IDs. Use USB Serial/JTAG for the console, UART0 for GPS and UART1 for RS-485.

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

GPS supplies UTC and receiver position. Air530Z also supplies receiver-clock
fallback when its TIM message reports valid time and leap seconds. The backup
battery setting does not establish runtime validity. See [time selection and
hardware acceptance](../docs/air530-time.md).

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
The shared example enables LCD, GPS, storage, LEDs, buzzer, and capture control.
Select `--board xiao` or `--board n32r8` for the connected board; see the
[standalone wiring guide](../docs/wiring.md#standalone-mini-xiao-esp32-c5).
Unconnected LCD/GPS devices do not stop scanning; an absent or failed SD card
falls back to internal flash storage. GPS location becomes `null` after
`gps.max_fix_age_ms` without a valid RMC update; GPS and RTC time also expire without fresh valid updates. Missing LEDs or buzzer simply produce no output.
The capture input has an internal pull-down so an absent switch stays OFF.
For XIAO, the peripheral pins are: LCD on GPIO23/24, GPS RX on GPIO12 and TX on GPIO0,
SPI microSD on GPIO8/9/10 with CS GPIO11, buzzer on GPIO7, and LEDs on GPIO1
(D0, green/increasing) and GPIO25 (D2, red/decreasing).
All 11 header GPIOs are used; remove the former amber LED and repurpose its pin for GPS TX. For the
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

This trades coverage for needing only one board: with the example’s 36 channels,
500 ms Wi-Fi dwells and 120 ms BLE windows, each Wi-Fi channel is revisited roughly
every 22.32 seconds,
plus switching/processing overhead. BLE also has scan interval/window gaps within
its allotted time. Packets outside a radio/channel's window are missed. Increase
following-alert gap and signal-staleness settings if needed for the longer sweep.
The screen shows the current radio/channel or a scan error; a failed tune is
retried on the next sweep. No children or dedicated follower are used. Observations
still flow through the normal parent rules, GPS tagging, tracking, logs and alerts.
Physical ESP32-C5 validation of switching and reception remains necessary.

## Detection category defaults

All shipped profiles enable every category in the [expanded detection pack](../rules/EXPANDED.md),
including `cameras`, `unifi_cameras`, and `ble_trackers` (Tile, SmartTag, Apple
Find My/iBeacon). `detection.disabled_categories` is empty in every profile.
To disable a category, remove it from `detection.enabled_categories` and add it to
`detection.disabled_categories`, then rebuild/reflash. Listing a category in both
places is a validation error. Existing configs may omit `disabled_categories`.
Category selection controls classifications and rule actions, not radio reception.
Unifi/Ubiquiti OUIs also identify non-camera equipment. Generic camera keywords
can independently match the same device even when its vendor category is off.

Optional per-address persistence and following scoring is documented in
[following scoring](../docs/following-scoring.md). `following_alert.scoring_enabled`
is false in every shipped profile; enabling it requires GPS and `following_alert.enabled`.
### Optional connected BLE discovery

All profiles ship with `gatt.enabled: false`. Active discovery requires an explicit
stable-address target list, enabled GATT-specific rules, and a central/security-disabled
SDK build. It transmits and connects. See [the configuration, limits, and hardware
validation checklist](../docs/gatt-discovery.md) before enabling it.

## Capture switch

`capture_switch` defaults to `{ "enabled": false, "gpio_pin": -1,
"internal_pulldown": false, "debounce_ms": 50 }`. Enabled configurations require
a manual parent role and explicit `xiao` or `n32r8` board profile. Pins are checked
against board restrictions and enabled peripherals. Individual LED pins set to
`-1` stay disabled when applying a board profile. The maintained active-high
switch uses the internal pull-down in the shipped configs; an external 10 kΩ
pull-down is also supported. See [full-capture operation](../docs/capture-switch.md) and
[wiring](../docs/wiring.md#optional-full-capture-switch-manual-parent--standalone).
Use `settings.parent.example.json` or
`settings.standalone.example.json` for the illustrated switch option.
All shipped manual parent and standalone configurations enable the switch with
`gpio_pin: -1`, which selects GPIO4 for `--board xiao` and GPIO26 for
`--board n32r8`. Explicit nonnegative switch pins remain user overrides and are
validated against the selected board and other peripherals. Set `enabled: false`
to disable the switch. Universal configurations explicitly keep it disabled.

Air530Z retained-time startup is requested over GPS TX on the former amber LED
pin. Valid TIM receiver time is used until active GPS time arrives, and again on GPS
loss. Older Air530Z firmware may not support TIM/`PCAS60`; checksum-valid void-RMC
date/time is then accepted as approximate `rtc` time, without a valid position or
GPS synchronization. Accuracy is unknown. See [receiver time limitations](../docs/air530-time.md). If neither source is available, logs have null UTC and the clock shows dashes.
Boot and scanning never wait for either source.

The amber LED and `signal_trend.steady_pin` setting are removed. Delete that key
from existing custom configurations. Both remaining LEDs light for steady or
accumulating samples; unknown/stale turns both off. Board profiles assign the
freed pin to `gps.tx_pin` (N32R8: 8; XIAO: 0). Custom profiles can retain `tx_pin: -1`
for receive-only GPS. Remove the amber LED/resistor before connecting GPS RX.

GPS time is optional at startup. Air530Z output configuration uses nonblocking
FIFO writes and periodic retries. Each GPS update reads at most 512 bytes with
zero wait. `gps.max_fix_age_ms` also bounds clock-source freshness. The model must
be `Grove GPS Air530Z` for TIM reception; Air530 remains active-RMC-only.
`gps.backup_battery_present: false` does not disable reception or bypass validity.
If UART initialization fails, firmware logs a warning and continues without GPS.
An initially silent receiver can begin reporting later without an MCU reboot.

The full US passive receive list contains 36 channels. Bus protocol 6 widens scan
coverage reporting; rebuild and flash all bus nodes together. Tune failures remain
visible as scan errors; reception on the added channels needs ESP32-C5 hardware
validation. This does not enable active Wi-Fi scanning or transmission.
