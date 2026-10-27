# Drone Remote ID detections

The receiver decodes ASTM/OpenDroneID **protocol version 2** broadcasts using:

- BLE Service Data (16-bit UUID `0xFFFA`, application code `0x0D`), including
  individual messages and message packs in complete extended advertisements.
- Wi-Fi beacon/probe-response vendor information elements (`FA:0B:BC`, type `0x0D`)
  carrying message packs. Children forward the vendor IE through the existing bus
  observation payload; the parent performs the same decoding as for local BLE.

No external matching rules are required. Valid supported broadcasts receive queue
priority and are logged even with `logging.mode` set to `matches`. Rule-based
buzzer/follower actions remain configurable through the existing rules. Build and
flash the updated parent and workers to capture Wi-Fi payloads end to end.

## Stored records

The existing console and optional microSD JSONL writers store an observation plus
one `event: "drone_detection"` record per message. Each record contains source MAC,
radio/address type, node, RSSI, channel, boot/sequence, receiver boot, capture time,
UTC when available, timing uncertainty and receiver GPS fix. The
`(node, boot, sequence, message_index)` tuple links each message to its observation;
`message_counter` retains the transmitter's counter. Each message's `raw_hex` and
the observation's `ble_ad_hex` or `wifi_remote_id_hex` preserve the received bytes.
Existing rotation, retention, queue limits and storage configuration apply.

`data` depends on `message_type`:

| Type | Decoded fields |
| --- | --- |
| 0 Basic ID | ID type, aircraft type, ID bytes; text ID for serial/registration types |
| 1 Location | Aircraft latitude/longitude, geodetic/barometric altitude, height/reference, horizontal/vertical speed, direction, status, accuracy codes, seconds after the hour |
| 2 Authentication | Authentication type and page; full page retained in `raw_hex` |
| 3 Self ID | Description type and text |
| 4 System | Operator position/type, classification, operation area count/radius/ceiling/floor, EU category/class, operator altitude, timestamp since 2019 |
| 5 Operator ID | Operator ID type and text |

Distances/altitudes are metres, speeds metres/second, direction degrees clockwise
from north, and coordinates decimal degrees. Geodetic altitude uses WGS84 HAE.
Unknown or out-of-range location/velocity fields are JSON `null`; latitude and
longitude both zero signify unavailable position. Numeric accuracy fields retain
protocol codes, not inferred metre values. Location time is relative to the hour,
not an independently established UTC capture timestamp.

Aircraft coordinates are in Location message `data`; operator coordinates are in
System message `data.operator_position`; receiver coordinates are in
`receiver_fix`. These are distinct locations. Records preserve individual message
updates rather than merging stale identity or location fields across transmissions.
Correlation across broadcasts can use source MAC/address type and the transmitted
IDs; MAC rotation and multiple Basic IDs require care in downstream analysis.

## Limits

This is broadcast decoding, not RFID tag reading or proprietary drone-link
reception. Broadcast contents are self-reported and are not authenticated;
`authenticated` is always false. Authentication pages are stored without signature
verification or reassembly. Older/newer protocol versions, unknown message types,
and malformed message envelopes are not classified as supported detections.

Wi-Fi NAN action frames are not supported. Existing capture bounds remain: 512-byte
Wi-Fi frames (excluding FCS), 255-byte BLE data, and complete unfragmented reports.
Each message is 25 bytes; packs contain at most nine messages. Reception depends on
channel assignments and scanning duty cycle. Bounded observation and writer queues
can drop records under load, including bursts of message packs. No hardware RF or
SD-card validation has been performed for this feature.

## Protocol reference and validation

The wire field layout and transport identifiers were checked against the
[OpenDroneID reference header](https://github.com/opendroneid/opendroneid-core-c/blob/master/libopendroneid/opendroneid.h),
[decoder](https://github.com/opendroneid/opendroneid-core-c/blob/master/libopendroneid/opendroneid.c),
and [Wi-Fi encapsulation](https://github.com/opendroneid/opendroneid-core-c/blob/master/libopendroneid/wifi.c).
The implementation uses explicit byte offsets and little-endian reads; no upstream
source code or signatures are vendored.

`remote_id_tests` exercises BLE and Wi-Fi encapsulation, bus round trips, stored
telemetry and raw evidence, unavailable values, negative coordinates/speed,
truncated or invalid packs, unsupported versions, escaped text and bounded output.
