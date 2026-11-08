# System architecture

The firmware targets ESP32-C5 with ESP-IDF v6.1. Portable C++17 components
implement parsing, detection, scheduling, tracking and bus framing; ESP-IDF
adapters provide radio reception, RS-485 transport and parent peripherals.
Settings and rules are validated and compiled into the image. Changes require a
rebuild and reflash; there is no runtime configuration reload.

## Roles and transport

The parent owns discovery, channel assignments, rule evaluation, follower
selection, GPS/time, storage, LCD, buzzer and trend LEDs. Children collect Wi-Fi
observations and forward bounded metadata and supported broadcast payloads. A
follower is reserved for focused reception and excluded from normal Wi-Fi channel
allocation. All nodes are assumed to travel together when using the parent's GPS.

Manual builds select parent or worker at build time. The universal image detects
parent hardware through a passive identification harness. In either workflow,
the parent assigns worker addresses and scanning/follower roles at runtime.
Workers start unassigned and stop capture when their leases expire.
See [universal provisioning](universal.md) and [discovery](discovery-protocol.md).

The wired RS-485 bus uses protocol 6 CRC-protected framing, boot sessions and
assignment tokens. A worker retains an in-flight observation until acknowledged;
the parent suppresses duplicate processing. Queues and deduplication state are
bounded and held in RAM. Reboots can lose queued observations or cause a retained
observation to be processed again. CRC detects corruption, not malicious peers.

## Scanning and coverage

The scheduler allocates up to 36 configured channels across 16 workers according
to their advertised capabilities. Children cycle through assigned channels with
configurable dwell. Surplus workers remain idle. More workers reduce revisit
time; they do not guarantee capture of every packet.

With no eligible Wi-Fi workers, the parent alternates local Wi-Fi and BLE scans.
It returns to BLE when workers provide Wi-Fi coverage. A manual standalone parent
uses the same radio time-sharing approach with the bus disabled. Status separates
planned coverage from channels confirmed by successful tuning; neither measures
uninterrupted RF reception. See [firmware limits](firmware.md).

Radio callbacks enqueue bounded data for parsing outside the callback. Wi-Fi
reception includes beacon/probe metadata, bounded probe-request information
elements, NAN discovery and selected MAC headers. BLE reception includes standard
and extended advertisements with bounded reassembly. Optional, explicitly targeted
[GATT discovery](gatt-discovery.md) opens connections to discover primary services.
It does not read characteristics or pair with devices.

## Detection and following

JSON rules specify category, radio, all/any predicates, confidence, priority,
actions and source provenance. The generator rejects malformed rules and compiles
bounded tables. Every matching rule produces classification evidence; the
highest-priority match controls follower and buzzer actions. The shipped rules
include reviewed community signatures; see [coverage and limitations](../rules/README.md).

OUI-only matches indicate possible vendor equipment, not a product or operator.
Randomized addresses, shared chipsets and spoofing cause false positives and
missed detections. [Remote ID](remote-id.md) decodes supported broadcasts and
retains their evidence, but broadcast identity and position are unverified claims.

The follower receives a selected address, radio and channel. Wi-Fi following
starts on the last channel, searches configured channels after target silence,
and releases stale targets. BLE following filters broadcasts for the requested
address. Address rotation does not establish identity continuity.

Optional [following-device scoring](following-scoring.md) combines repeated
sightings with receiver displacement. It maintains encounters per radio, address
type and address, with an explicit ignore list and configurable thresholds.

## Time, location and signal strength

The parent parses checksum-validated GPS RMC messages and keeps bounded receiver
fix history. Air530Z receiver time can supply approximate UTC without a position
fix. Time and position expire independently; missing or stale time yields null
UTC, not indefinite holdover. See [receiver time](air530-time.md).

Worker observation age maps capture time onto the parent's monotonic clock.
Bus round-trip time contributes to timing uncertainty. Captures receive the
nearest sufficiently fresh receiver fix; uncertain associations remain null.
Receiver coordinates are not transmitter coordinates. RSSI is neither distance
nor bearing.

A 64-entry tracker retains first/last sightings and per-source/channel/session
peak RSSI with the receiver fix at the peak. Eviction starts a new in-memory
history; recorded logs retain earlier evidence. Median RSSI windows, deadband and
confirmation counts drive two LEDs. Increasing lights green, decreasing lights
red, and steady or accumulating samples light both. Stale/unknown targets turn
both off.

## Storage and peripherals

Parents prefer writable SD storage and fall back to bounded internal flash.
Separate matched and full-capture streams use size rotation, file-count limits
and matched-priority retention. Output queues are bounded; slow storage or serial
output can drop records. Workers do not write persistent logs. See [storage](storage.md)
and [exporting](exporting.md).

A maintained [capture switch](capture-switch.md) starts and stops full structured
observation sessions without changing detection or radio assignments. It does not
provide raw packet capture. The LCD reports scanning, detection, time and storage
state; the buzzer shares a global alert cooldown.

Board profiles define GPIO mappings for the N32R8 parent and XIAO workers or
standalone parent. The [wiring guide](wiring.md) specifies RS-485, GPS, SD, LCD,
buzzer and LED connections. Hardware acceptance still requires physical tests
of radio coverage, bus throughput, timing, power, storage failure recovery and
peripheral operation. Host tests and firmware builds validate software behavior;
they do not establish electrical or RF performance.
