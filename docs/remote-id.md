# Drone Remote ID detections

The receiver decodes ASTM/OpenDroneID **protocol version 2** broadcasts using:

- BLE Service Data (16-bit UUID `0xFFFA`, application code `0x0D`), including
  individual messages and message packs in complete extended advertisements.
- Wi-Fi beacon/probe-response vendor information elements (`FA:0B:BC`, type `0x0D`)
  carrying message packs. Children forward the vendor IE through the existing bus
  observation payload; the parent performs the same decoding as for local BLE.
- Passive Wi-Fi NAN Public Action service-discovery frames (`50:6F:9A`, type
  `0x13`) for the `org.opendroneid.remoteid` service (`88:69:19:9D:92:09`).
  No association, active NAN participation or network access is used. Children
  forward the complete bounded action body; standalone reception uses the same
  parser. Generic NAN service traffic does not produce a detection.

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
`drone_detection.transport` and observation `remote_id_transport` distinguish
`ble`, `wifi_beacon`, and `wifi_nan`; the `radio` remains `wifi` for both Wi-Fi
transports. CSV/TSV include these additional columns. For NAN, `wifi_remote_id_hex`
contains the entire action body starting with `04 09 50 6f 9a 13`, including the
service descriptor, counter, pack, extension and any accepted extra attributes.
The 802.11 header is not stored; its source MAC is retained as observation identity.
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

Existing capture bounds remain: 512-byte Wi-Fi frames (excluding FCS), 255-byte
BLE data, and complete unfragmented reports. NAN requires an unprotected,
unfragmented management action frame addressed to `51:6F:9A:01:00:00`, with the
supported public-action header and one Remote ID service descriptor. The supported
reference profile uses instance 1, requestor 0, service control `0x10`, and a single
instance-1 extension with control `0x0200` and matching update counter. Other
profiles are rejected rather than interpreted speculatively. Attribute lengths,
service-info length and every inner message are validated before any detection;
malformed suffixes invalidate the whole envelope. Well-formed unknown attributes
are retained, while duplicate descriptors/extensions are rejected.

The entire NAN action body must fit 255 bytes: the reference profile's nine-message
pack occupies exactly 255 bytes (6-byte header, 242-byte descriptor attribute,
7-byte extension). Smaller packs can include additional attributes within that
same limit. Frames over 512 bytes, NAN bodies over 255 bytes, and packs over nine
messages are rejected without truncation. The existing bus observation encoding
and protocol version are unchanged; the complete evidence fits within its 359-byte
report payload. Older firmware will preserve the bytes but cannot decode NAN, so
update parents and receivers together. No observation, capture queue or tracking
buffer was enlarged; NAN parsing uses bounded views and no heap allocation.
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

## NAN reference fixture and hardware validation

NAN wire offsets and the supported profile were checked against OpenDroneID
core-c revision [`849c03adbe0cdd68997a462fafab397edcfe53f3`](https://github.com/opendroneid/opendroneid-core-c/tree/849c03adbe0cdd68997a462fafab397edcfe53f3),
particularly `libopendroneid/wifi.c` and `odid_wifi.h`. That revision is licensed
under Apache-2.0. No upstream implementation is copied or linked into firmware.
`tests/fixtures/nan_reference.hpp` is synthetic output from that reference encoder,
containing a test Basic ID and location; it is **not a captured aircraft broadcast**.
Our fixture generator can be reproduced with a local checkout of that revision:

```sh
cc -I/path/to/opendroneid/libopendroneid tests/fixtures/generate_nan.c \
  /path/to/opendroneid/libopendroneid/opendroneid.c \
  /path/to/opendroneid/libopendroneid/wifi.c -lm -o /tmp/generate-nan
/tmp/generate-nan > tests/fixtures/nan_reference.hpp
```

`nan_tests` covers the reference fixture, full bus/evidence round trips, maximum
packs, every truncated prefix, malformed lengths, unrelated services, unsupported
versions, duplicates and invalid MAC fragmentation flags. It also verifies distinct
transport labels and bounded JSON output. Export tests preserve transport columns.

Physical C5 validation remains outstanding: confirm the pinned ESP-IDF
promiscuous callback delivers these management action frames, capture a supported
device or controlled test transmitter, and compare local versus forwarded JSONL
against an independent capture. Test the transmitter's actual channel with fixed
channel reception first, then measure delivery under configured channel/dwell
schedules and standalone BLE windows. The receiver does not synchronize with NAN
discovery windows, so channel scanning and BLE pauses can miss broadcasts. Measure
free/minimum internal heap, storage pressure and observation drops with Wi-Fi and
BLE active on the real board; unchanged buffer sizes do not prove RF coverage or
runtime headroom. No Mini 3 compatibility or other device interoperability is
claimed without a captured compatible broadcast. Captured-device fixtures and
those measurements must be added after hardware testing.
