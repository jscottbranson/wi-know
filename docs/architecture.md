# System architecture and requirements

This describes the target architecture. See [firmware status and limits](firmware.md)
for the implemented first version; not every requirement below is implemented yet.

## Roles and transport

The parent owns membership, assignments, rule evaluation, follower selection,
GPS/time, microSD, buzzer, LCD, and trend LEDs. Children collect Wi-Fi observations
and forward bounded metadata and supported broadcast payloads. The follower is
reserved for focused work and is excluded from the normal scan-worker count.

Proposed baseline: ESP-IDF firmware with shared components and role-specific
configuration. Pin assignments, parent board, transport and SDK version remain
implementation decisions. Prefer a wired control/data bus for co-located nodes:
Wi-Fi or ESP-NOW reporting competes with scanning and requires rendezvous on
compatible channels. A wired bus needs a concrete electrical design (for example,
addressed RS-485 with transceivers); bare multi-drop UART TX wiring is unsuitable.
Provide a transport abstraction so this choice does not enter the scheduler.

Every message carries protocol version, node ID, boot/session ID, sequence number,
type, payload length and integrity check. Define HELLO, HEARTBEAT, ASSIGN, ACK,
OBSERVATION, FOLLOW, STOP and STATUS. HELLO advertises role, bands, supported
channels, BLE/Remote ID support and firmware version. Commands are idempotent and
acknowledged; assignment generations reject stale commands. Nodes stop an expired
assignment and report awaiting assignment. Rate-limit reports and expose dropped
events, queue occupancy, clock uncertainty and receive coverage.

## Membership and spectrum division

1. Discover children, check compatibility and capability, then maintain heartbeat leases.
2. Construct a country-specific, explicitly allowed channel list. Validate actual
   driver support, passive reception and DFS behavior on hardware. Example settings
   intentionally omit DFS and some region-specific channels; they are not full coverage.
3. Sort nodes by stable ID. For identical nodes, divide the ordered channel list
   into groups differing by at most one channel; use round-robin allocation across
   nodes so bands are distributed. Each channel has one owner per generation.
4. For different capabilities, assign each channel only to an eligible child,
   choosing the least-loaded eligible child. Report any unassignable channel.
5. Recompute on joins, expired leases, capability changes or settings changes.
   Apply a complete generation with acknowledgements and bounded leases. Report
   temporary gaps during transitions rather than claiming uninterrupted coverage.

Children cycle through their assignments with configurable dwell and retune time.
Publish effective dwell and revisit interval. More children reduce revisit time;
they do not guarantee complete packet capture. Surplus workers are standby until
an explicit duplicate-coverage policy is implemented. With zero children, report
Wi-Fi coverage unavailable; do not silently steal the parent's BLE radio.

## Observations and detection rules

Preserve observation provenance: source node and boot ID, sequence, radio type,
band/channel, source address and address type, BSSID when applicable, RSSI dBm,
frame/advertisement type, bounded SSID/name/manufacturer/service data, capture
monotonic time and parent receive time. Parse received lengths defensively outside
radio callbacks; callbacks enqueue bounded data and never write to SD or the LCD.

Rules live separately from settings. Settings enable categories such as drones,
Flock, Axon, general ALPR, Ring and other doorbell cameras. A versioned rule has
ID, category, protocol, all/any predicates, confidence, priority, actions and source
provenance (URL, revision, license and review date). Supported predicate design:
MAC prefix, SSID/name, BLE service UUID/manufacturer payload, and decoded Remote ID.
Bound pattern lengths and rule count; reject malformed rules atomically.

OUI-only results are possible vendor matches, not proof of a product or operator.
Shared chipsets, spoofing and randomized addresses cause false positives/negatives.
Retain the matching evidence and confidence. Avoid OUI inference on random/local
addresses. No made-up production signatures ship with the example settings; enabled
categories with no loaded rules must appear as unavailable in status.

## Follower

The parent selects a matching target by explicit operator selection, otherwise by
rule priority and recency. It sends target identity, protocol, last channel,
evidence, expiry and assignment ID. The follower acknowledges, dwells on the last
known channel, filters matching frames, and returns additional observations.
After a configurable silence interval, it searches a bounded candidate-channel
list, then releases the target on timeout. Higher-priority work may preempt it.
The child that originally detected it keeps its normal assignment.

Wi-Fi and BLE follow modes time-share the follower radio where necessary. MAC
rotation cannot be treated as identity continuity without supporting evidence.
Use decoded Remote ID identity when available; preserve identity type and validity.
Decode broadcast identity, location and motion with a reviewed Remote ID library;
validate supported BLE advertising and Wi-Fi beacon/NAN paths independently on C5.
Missing data remains unknown. Broadcast telemetry is an unverified transmitter claim.

## GPS, clocks and observation location

The parent reads the Grove Air530 over hardware UART; Seeed's example uses 9600
baud. Parse NMEA with checksums and bounded buffers. Combine date/time and position
only from consistent epochs. Track time validity independently from position fix,
satellite count, HDOP and fix age. Reject invalid coordinates and stale fixes.

Use GPS UTC date/time to synchronize the parent's software clock once validated.
Continue from a monotonic clock during loss of GPS and mark the clock as holdover
with age/uncertainty. An optional battery-backed I²C RTC can initialize UTC after
power loss and be corrected from validated GPS. Check its oscillator/battery status
before trusting it. The Grove Air530 includes an onboard backup battery, and the
Air530 has an internal RTC and VBACKUP supply for retention/hot starts. Use this
battery-backed GPS clock as the baseline; a separate RTC is optional, not a required
addition. Bench-test power cycling and startup without satellite reception to verify
when retained date/time becomes available over UART and how validity is reported.
The backup battery does not power the parent or keep normal UART reporting active
while the GPS main supply is off. See the
[Air530 manual](https://files.seeedstudio.com/wiki/Grove-GPS_Air_530/Air530_GPS_User_Booklet.V1.7.pdf).
Without a trustworthy source, log UTC as null and preserve monotonic time and boot ID.
Never fabricate timestamps; log time corrections without reordering monotonic events.

Synchronize child monotonic clocks to the parent with periodic exchanges and retain
offset uncertainty. Associate observations with the nearest fresh GPS fix at capture
time using a bounded fix history, not blindly with the latest fix at message arrival.
Record fix age, time offset and transport delay; leave location null when timing is
too uncertain. The default association assumes all nodes are co-located. Remote
children need their own location source or explicit fixed coordinates.

## Peak strength and LEDs

Track first/last seen, count, latest RSSI, smoothed RSSI and peak raw RSSI for each
signal identity, source node, protocol and channel. A stronger peak is the larger
dBm value (for example -45 is stronger than -70). Snapshot the peak's capture time,
receiver fix and fix quality together. A later observation must not overwrite the
location of an earlier peak. Store a global observed maximum with its node/channel
provenance separately; antennas and receiver calibration make cross-node comparisons
approximate. Peak state is scoped to a tracking session; persisted logs retain history.

For one selected matching target, compare consecutive smoothed-RSSI windows from
the same node/protocol/channel. Use a median per window initially, a configurable
deadband (initially 3 dB) and confirmation windows before changing LED state.
Increasing: green; decreasing: red; steady: amber. Reset history on target or source
change. Insufficient samples, stale data and signal lost turn all trend LEDs off.
Never show a missing observation as steady. State is strength trend, not proof of
movement toward/away from the transmitter. Colors and thresholds are configurable.

## Parent UI, storage and electrical integration

Rotate bounded 16-character LCD pages: active child count/coverage, latest rule
match, selected target RSSI/trend/peak, GPS/time quality, and SD/follower status.
Show unavailable/lost states explicitly. Use nonblocking refreshes and buzzer PWM
with per-rule/per-target cooldowns. LEDs track only the target shown on the LCD.

Store JSON Lines observations and match events on microSD. Mode `matches` stores
matching observations and evidence; mode `all` stores all normalized observations,
not necessarily every raw packet. Include schema version, capture/receive time,
time quality, receiver fix, target identity, rule IDs, RSSI and peak-change events.
Rotate by size, enforce retention and free-space policy, periodically flush, and
recover from a truncated final line. Bounded RAM buffering and explicit loss counters
keep radio reception running if the SD card is missing, slow or full. Preserve
status/error events in either logging mode. Raw packet capture is a separate future option.

The available microSD adapter is probably an HW-125; identification is unconfirmed.
The user confirms header labels CS, SCK, MOSI, MISO, VCC and GND. CS, SCK and MOSI
are inputs to the adapter driven by the parent; MISO returns data to the parent.
VCC and GND provide power. Confirm the
actual board's regulator, buffer markings and supply requirements before wiring;
the model name alone does not establish its electrical behavior. Regulated modules
in this family commonly require about 5 V at VCC to produce the card's 3.3 V supply;
do not assume that feeding their regulator 3.3 V is sufficient. Keep ESP32-facing
signals within 3.3 V limits. Check that MISO releases when CS is inactive before
sharing the SPI bus. Leave supply voltage and GPIO assignments unset until checked.

Budget parent pins for I²C (LCD and optional RTC), GPS UART RX (TX optional), SD SPI,
buzzer PWM, three LEDs and node transport. Consider an I²C GPIO expander for LEDs
if needed. A small parent board may not expose enough pins. Set every GPIO explicitly
after selecting the parent. Verify LCD backpack voltage/pull-ups, logic levels,
SD adapter compatibility and buzzer drive current before wiring; ESP32 GPIO is 3.3 V.
Use LED current-limiting resistors and a buzzer driver where required. Exact LCD
expander type/address and optional RTC model remain TBD.

## Delivery stages and acceptance

1. Select parent/transport/pin map and pin an ESP-IDF release. Bring up one C5 on
   each configured Wi-Fi band, passive BLE, GPS, LCD, SD, buzzer and LEDs individually.
2. Implement bounded protocol and scheduler; verify zero/one/many workers, reconnects,
   leases, incompatible capabilities and follower exclusion using host tests.
3. Implement observation normalization and rules; replay known-positive/negative
   fixtures, randomized addresses and malformed packets. Review signature provenance.
4. Implement GPS/UTC association, peak snapshots and trends; test delayed messages,
   GPS loss, invalid fixes, reboot without UTC, clock corrections and source changes.
5. Implement follower and Remote ID decoding; test loss/reacquisition, timeout,
   preemption and partial telemetry with controlled broadcasts.
6. Measure radio coverage, bus throughput, dropped events, SD failure recovery,
   power stability and UI behavior with the intended number of physical boards.

Configuration must be validated before activation. Unknown versions, invalid pins,
invalid channel lists, empty enabled rule sets and unavailable peripherals receive
explicit diagnostics. Keep the last valid configuration on failed reloads.
