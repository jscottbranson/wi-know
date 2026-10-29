# Passive Wi-Fi receiver evidence

Wi-Fi scanning retains MAC headers from ordinary data (subtype 0) and QoS data
(subtype 8), plus the existing probe-request, probe-response, beacon and supported
NAN action frames. Reception is passive. No association, decryption or data-body
logging is performed. Physical ESP32-C5 promiscuous delivery and memory/load
measurements remain to be validated; a successful firmware build is not that test.

## Address and signal meaning

`Observation.address`, `mac`, `oui`, RSSI, signal trends, generic following
encounters and follower commands always refer to the transmitter, address 2.
Receiver predicates `wifi_receiver_mac` and `wifi_receiver_oui` explicitly use
address 1. A unicast receiver named in another device's frame is **indirect
evidence**: it does not establish that the receiver was present or transmitted.
OUI matching rejects locally administered and multicast receiver addresses.
Exact MAC matching accepts local addresses but rejects multicast and the zero
address. Existing transmitter predicate behavior is unchanged.

For supported three-address frames the retained roles are:

| ToDS | FromDS | Receiver | Transmitter | Source | Destination | BSSID |
| --- | --- | --- | --- | --- | --- | --- |
| 0 | 0 | A1 | A2 | A2 | A1 | A3 |
| 1 | 0 | A1 | A2 | A2 | A3 | A1 |
| 0 | 1 | A1 | A2 | A3 | A1 | A2 |

Management frames require both DS bits clear. Four-address WDS frames, fragments
(More Fragments or nonzero fragment number), retry data frames, HT-control data
frames and QoS A-MSDU frames are rejected. Unsupported control/data subtypes are
rejected. Protected data headers are supported: only the unencrypted MAC header
is used. Management retry behavior is unchanged. Repeated non-retry observations
remain separate captures; bus retransmissions use the existing boot/sequence
receipt deduplication.

Indirect rules must have `possible` confidence and `follow: false`. Any rule
containing a receiver predicate, including a mixed `any` rule, is treated as
indirect. It may log and buzz through the shared cooldown, but never changes the
LCD's signal target, RSSI trend LEDs, or follower target. The strongest direct
match is selected independently, so a high-priority indirect match cannot hide a
lower-priority direct target. All matching classifications are retained. The
ordinary observation/track address and RSSI remain those of the transmitter,
including when an indirect rule is logged; no receiver signal estimate exists.

## Bounds and retained evidence

The callback strips the four FCS bytes specified by ESP-IDF `sig_len` and rejects
receive errors. Data captures copy only 24 or 26 header bytes, regardless of the
body's length. Management frames still require a complete frame of at most 512
bytes excluding FCS. Data does not consume the 255-byte payload used by probe IEs
and Remote ID. Header validation rejects incomplete headers; data body length or
validity is deliberately not inferred from header-only evidence.

Data admission is limited to one frame per 50 ms and requires more than four free
slots in the existing eight-entry raw queue. This reserves four queue slots for
management traffic and limits parsing, matching and bus load under busy traffic.
Dropped data admissions increment the existing dropped counter. It is a lossy
sample, not traffic accounting; management can still overflow its bounded queue.
All parsing after admission runs in the worker. Static per-observation storage
grows by a 26-byte header and one size byte (plus compiler padding), including the
bounded tracker/queue copies. No heap allocation is added to capture or parsing. Log-line capacity grows from
2304 to 2816 bytes to accommodate maximum probe evidence plus address roles and
GPS metadata: +512 bytes per stack-local line and +2 KiB across the two bounded
two-entry disk/console queues. Preserve previous stack headroom by adding 512
bytes to each console/logger stack (4608/6656 bytes), and 1024 bytes to the main
stack (13312 bytes) for its nested formatting/write buffers. These stacks add
2 KiB in total; hardware stack-watermark and heap checks remain necessary. A
compile-time check rejects cached `firmware/sdkconfig` files with the old main
stack budget: set `CONFIG_ESP_MAIN_TASK_STACK_SIZE=13312` or greater, or regenerate
that local configuration from the updated defaults.

Bus protocol **4** carries the header length and 26-byte padded header field.
Maximum report payload is 387 bytes, or 419 with the universal identity wrapper,
within the existing 512-byte payload and conservative bus timing budget. All
nodes must be rebuilt/flashed together: earlier protocol versions are rejected.
Wire decoding checks the subtype, header shape, transmitter identity, zero
padding and absence of a data payload. Existing manually constructed management
observations without header evidence remain usable but cannot match receiver
predicates.

JSONL schema 1 adds `wifi_header_hex`, `wifi_receiver`, `wifi_transmitter`,
`wifi_source`, `wifi_destination`, `wifi_bssid`, `rssi_role: "transmitter"`, and
`match_evidence: "indirect_receiver"` or `"transmitter"`. Data uses distinct
`wifi_payload_format: "header_only_v1"` and synthetic `wifi_subtype` values 16
(data) / 24 (QoS data); management keeps its existing subtype values. Header hex
preserves actual frame-control, sequence, DS, protected and QoS bits so the roles
can be reproduced. JSON/JSONL/CSV/TSV/GeoJSON exports retain these fields.

## Production rule review and remaining hardware checks

The receiver behavior was reviewed in OUI Spy Unified Blue at
`46eb13b2746ae72ad7070bbf5e42deb09b573623`,
`src/raw/flockyou_promiscious.cpp`, and Flock You at
`6704f011f3e6c9cfe94fe18c593f863032c9c954`, `main.cpp`. Both are MIT;
existing notices are retained in `rules/sources`. No upstream parser code was
copied. Their address-1 detection motivated the explicit receiver predicate;
address 3 is decoded by DS role rather than assumed to always be a BSSID.

`rules/receiver.json` adds only Flock's registered `b4:1e:52` receiver prefix for
probe responses, data and QoS data, at possible confidence. That assignment was
already independently checked in the retained IEEE registry extract. Shared
Qualcomm/Liteon/Espressif prefixes and the upstream local-address prefix are not
imported as Flock identities. No category settings are changed. Disabling the
existing `flock` category disables these rules too. Existing transmitter OUI
rules now also observe supported data headers; their confidence is unchanged.

Host fixtures exercise DS roles, protected/QoS headers, forbidden forms,
truncation, global/local/group addresses, forged wire evidence, export and target
selection. Before claiming complete issue #39 acceptance, capture controlled
three-address traffic on a physical C5 in each DS direction, with protected and
QoS frames, and compare retained headers to an independent capture. Check
standalone/worker-parent parity, management/NAN coverage during busy traffic,
heap headroom, queue drops and scan dwell/channel behavior. No such on-device
validation was available during implementation.
