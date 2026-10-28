# Build, configure, and run

## Layout

```text
config/settings.json                 edit hardware, scanning, logging and categories here
rules/*.json                         active detection rule documents
rules/examples/                      inactive rule templates
components/wiknow/include/wiknow/     shared C++ interfaces
components/wiknow/src/                portable scheduler, parsers, matcher, tracking and protocol
firmware/main/                       ESP-IDF radios, node roles, and peripheral drivers
tools/generate_config.py             configuration/rule validation and C++ generation
tools/flash.py                       universal image build/flash and USB port selection
tests/                               host-side core and configuration tests
```

## Easy flashing

Use `python3 tools/flash.py` in an activated ESP-IDF v5.5.1 environment to choose
the USB port. The default image is universal; use the same config for all boards.
See [universal provisioning and validation](universal.md) before wiring. See [the flashing guide](flashing.md) for the
interactive workflow and command-line examples. It isolates each device's build
and SDK settings; the manual commands below remain available.

## Toolchain and builds

Use ESP-IDF **v5.5.1** with the ESP32-C5 toolchain. Follow
[Espressif's installation instructions](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c5/get-started/index.html),
then activate its environment with `export.sh` before these commands. Run from the
repository root. Nothing is flashed by a build.

```sh
python3 tools/generate_config.py
idf.py -C firmware -B build-universal -DIDF_TARGET=esp32c5 build
# Explicit manual roles below require an isolated legacy bus.
idf.py -C firmware -B build-parent -DIDF_TARGET=esp32c5 -DWIKNOW_ROLE=parent -DWIKNOW_NODE_ID=0 build
idf.py -C firmware -B build-child-1 -DIDF_TARGET=esp32c5 -DWIKNOW_ROLE=child -DWIKNOW_NODE_ID=1 build
idf.py -C firmware -B build-follower -DIDF_TARGET=esp32c5 -DWIKNOW_ROLE=follower -DWIKNOW_NODE_ID=2 build
```

Give each additional worker a distinct nonzero ID, within the parent's bus range.
To flash a particular build, repeat its options and add your port:

```sh
idf.py -C firmware -B build-child-1 -DWIKNOW_ROLE=child -DWIKNOW_NODE_ID=1 -p /dev/ttyACM0 flash monitor
```

Use separate build directories for separate role/ID/config combinations. Custom
settings: add `-DWIKNOW_CONFIG=/absolute/path/to/settings.json` on configure/build.
`firmware/sdkconfig.defaults` selects passive BLE observation with NimBLE extended
advertisements, a USB Serial/JTAG console, and a single factory app partition for
2 MiB or larger flash, including a 512 KiB internal log partition. There is no OTA updater. GPIO assignments and enabled
peripherals still require your actual board's pin map.

For host tests, use a C++17 compiler, CMake 3.20+ and Python 3:

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

CI runs host tests, both universal hardware profiles, the manual standalone image,
and all three manual firmware roles with the pinned SDK.

## Initial hardware setup

Follow [universal provisioning](universal.md) for the shared image, identification
harness, pin changes, recovery and physical validation procedure. Parent bus RX
is GPIO3; worker RX is GPIO12. Default peripherals are off. The universal example
enables the parent peripherals and moves GPS RX to GPIO26. Every board receives
the exact same image. Roles and short addresses are assigned at runtime.

Explicit manual `parent`, `child` and `follower` builds remain available for a
separate legacy bus, using unique nonzero worker IDs. Their original parent wiring
example and protocol remain available; never mix manual and universal bus nodes.
Add reviewed rules under `rules/` and flash the same rule set to every board.

## Implemented behavior

- Fixed-capacity C++ core; radio callbacks never perform filesystem or LCD writes.
- Deterministic capability-aware allocation of up to 32 channels across 16 nodes,
  heartbeat expiry, and exclusion of the follower from normal Wi-Fi assignments.
- Manual mode uses a versioned little-endian polling/report protocol with CRC32, node boot IDs,
  command sequences and assignment leases. Protocol version 2 adds observation
  acknowledgments: a worker retains its in-flight observation until the parent
  acknowledges its boot ID and observation sequence. Lost replies or acknowledgments
  are retried on subsequent polls; the parent suppresses duplicate processing.
  Rebuild and flash every node together; version 1 firmware cannot communicate
  with version 2 firmware. Local RS-485 bus is assumed trusted;
  CRC detects corruption and is not authentication.
- Receive-only Wi-Fi beacon/probe-response metadata and passive BLE advertising
  (legacy and complete extended reports). Wi-Fi frames over 512 bytes and BLE
  advertising payloads over 255 bytes are currently skipped; fragmented reports
  are not reassembled. Arbitrary data traffic is not captured.
- [Drone Remote ID decoding and storage](remote-id.md) for BLE and Wi-Fi beacon
  broadcasts: identity, location, velocity, system and operator data, with raw evidence.
  Supported detections are retained in matches-only mode without custom rules.
- Build-time JSON rule loading: OUI, SSID/name prefix, BLE manufacturer prefix and
  16-bit service UUID, category switches, UTF-8 text prefixes, highest-priority
  matching actions, and a separate logged classification for every matching rule.
  The generated `settings.rules.json` retains source provenance for sharing.
- Matching MAC/protocol follower selection. Wi-Fi follower dwells on the last channel
  then searches configured channels after 3 seconds without a target sample; parent
  releases stale targets. BLE follower filters broadcasts for the requested address.
- Checksum-validated Air530 RMC parsing, receiver-fix history, GPS UTC holdover,
  null UTC before validated synchronization, and capture-age-based association for
  buffered child observations. Full bus round-trip time is a conservative timing
  uncertainty; uncertain associations get null location. All nodes must be co-located.
  Tagging includes timing uncertainty in the fix-age limit, and the parent drains
  GPS input after bus polls before associating observations. Each log records the
  receiver location at capture and preserves the location of the strongest sample;
  a stronger sample without a usable fix records a null peak location.
- Per-source/channel/session peak RSSI with the receiver fix at that peak. Tracking
  is bounded to 64 entries and evicts the least-recently observed entry when full.
  Logged history persists, but an evicted entry begins a new in-memory peak history.
- Median RSSI windows, deadband and confirmation counts, with three active-high
  trend LEDs and an unknown/off state for stale data. These indicate strength, not bearing.
- PWM buzzer with global cooldown, common PCF8574/HD44780 16×2 LCD driver, and
  SD-over-SPI logging. LCD driver mapping: RS=P0, RW=P1, E=P2, backlight=P3, D4–D7=P4–P7.
- JSONL on console and SD with [internal-flash fallback](storage.md); matching-only/all-observation modes; bounded
  SD writer queue, periodic flush, size rotation and file-count retention.
  Existing files are not appended on reboot. SD formatting requires an explicit
  `sd.format_if_mount_failed` opt-in (disabled by default);
  only a completely erased flash log partition is initialized automatically.

## First-version limits and validation still needed

The firmware must be exercised on physical hardware; compilation does not validate
RF sensitivity, pin wiring, SD power, Air530 battery retention, LCD mapping or the
bus. The N32R8 parent now has a header map and peripheral wiring example;
the actual HW-125 and LCD backpack variants still require confirmation. SD mount/open failures
fall back to flash at boot. Later storage failures surface on the console and LCD
status; recovery currently requires reboot. There
is no runtime configuration reload, external RTC driver, automatic signature import,
Wi-Fi NAN Remote ID reception, RFID reader, Bluetooth Classic
support or triangulation in this version. These are explicit next implementation
stages, not simulated telemetry.

The bus returns at most one observation per node per poll. Separate bounded queues
reserve 16 entries for observations matching local rules and 32 for ordinary
observations; matching observations are drained first. Install the same reviewed
rules on parent and workers so they agree on priority. The parent still evaluates
the final detection rules. Accepted captures survive reassignment and lease expiry;
expiry stops new captures, while previously accepted observations can still be sent.

Busy environments can exceed bus or radio-parser throughput, and sustained matching
traffic can fill the matching queue. Queue overflows are counted, not made lossless
by acknowledgments. Parent console/LCD status distinguishes local losses from worker
losses; structured worker status events retain per-node totals across worker reboots
and offline periods for the parent's current uptime. Reports retained only in RAM
are lost if their worker reboots. Parent deduplication state is also in RAM, so a
parent reboot may process an unacknowledged observation again. RSSI peaks represent
observations that reach the parent.

The passive Wi-Fi memory profile uses four static RX buffers, at most sixteen
dynamic RX buffers, and disables AMPDU/AMSDU aggregation. The raw Wi-Fi parsing
queue holds eight frames. Console and SD output queues each hold two 2304-byte
lines; slow output or bursts can drop records, counted by the existing local loss
counter. These bounds trade burst capacity for RAM available to Wi-Fi/BLE startup
with optional PSRAM used for the parent tracking table. Workers do not allocate
that table. Wi-Fi throughput IRAM optimizations are disabled to leave more internal
RAM available; stacks and radio buffers remain internal. Serial startup logs report free internal heap and the
largest available block before each radio and after initialization. Radio init
errors are also written to an enabled parent LCD before the normal fatal error
handler runs. Hardware testing must verify startup headroom and capture loss under
load; successful builds do not measure either.

Parent status separates online children, planned channels, and channels confirmed
by successful tuning. Each report echoes the applied assignment generation and a
bitmap of channels actually visited; a first-channel success does not confirm the
rest. Reassignment, expired leases, worker reboots, and tuning errors invalidate
that evidence. Tuning failures identify the failed channel in `worker_scan` events
and are reported before retrying. Idle workers and BLE followers contribute no
Wi-Fi coverage. These confirmations describe successful driver tuning, not
uninterrupted RF reception or capture of every packet.

Timestamps mapped from worker packet age are approximate; driver buffering and bus
latency remain. Only active RMC sentences establish UTC initially; backup time before
GPS fix must be validated on the Air530. Satellite quality/HDOP and GGA parsing are
not yet implemented. A peak receiver location is not a transmitter location.
