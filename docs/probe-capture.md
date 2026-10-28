# Passive probe capture and evidence

Probe requests are admitted by the ESP-IDF management callback with a minimum
24-byte header plus the driver's four FCS bytes. FCS is removed exactly once;
the shared `parse_wifi` API takes a complete frame **without FCS**. Capture retains
at most 512 bytes excluding FCS. The callback copies only; the worker validates
management frame version/type, DS flags, More Fragments, fragment number,
Protected and Order/HT-control flags, then all IE bounds. Unsupported or malformed
frames are rejected. Ordinary probes need no beacon fixed parameters.

The transmitter at header offset 10 is the observed address; receiver and BSSID
are not substitutes. Probe subtype, SSID presence/length/raw bytes, ordered IEs,
capture channel, RSSI and timing survive parsing. The text `ssid` is display text;
embedded NUL truncates its display view, so `wifi_ie_hex` is the authoritative
binary evidence. A missing SSID and a present empty wildcard SSID stay distinct
in that evidence. The explicit `wifi_requested_ssid_prefix` rule can surface requested network-name
hints without attributing those names to the client manufacturer. A directed probe
does not prove a saved network or association, and cannot establish that the client
is a phone. No active probes or service discovery are transmitted.

For probe requests, all IEs must fit the existing 255-byte Observation payload
(maximum accepted probe frame: 279 bytes excluding FCS). Oversize probes are
rejected entirely, even if a prefix would match. This keeps the raw queue at eight
512-byte captures and avoids increasing payload/queue/track arrays. Adding one
subtype byte may add native struct alignment padding; firmware memory reports
and on-board stack/heap tests must be checked for each build profile. Raw capture
can still drop probes under load. The existing 32 ordinary/16 matching observation
queue slots prioritize matches after parsing, and reliable retained delivery,
boot/sequence deduplication, identity and tracking behavior are unchanged.

The subtype is an explicit wire field: report payload offset 104 (after the
little-endian IE/payload length at 102), followed by payload bytes at 105. Values
are 4=probe request, 5=probe response, 8=beacon, 13=supported NAN action, and
255=unknown/not applicable. BLE requires 255. Maximum report payload is now 360
bytes; the 32-byte universal envelope still fits the existing 512-byte packet.
Bus framing is **version 3** and rejects v1/v2; reflash all nodes together.
Subtype 4 selects complete `probe_ie_v1` evidence. Other Wi-Fi subtypes retain
existing Remote ID vendor-IE/NAN-body semantics; BLE retains its AD data.
Probe-request vendor IEs are not a supported Remote ID transport and cannot
accidentally be parsed as one. Existing beacon, probe-response and NAN Remote ID
paths are unchanged.

JSONL schema 1 gains additive fields `wifi_subtype`, `wifi_payload_format`, and
`wifi_ie_hex`. Probe evidence uses `probe_ie_v1`; other Wi-Fi payloads use
`remote_id`. `wifi_remote_id_hex` remains empty for probes. JSON/JSONL/GeoJSON
preserve these properties and CSV/TSV expose dedicated columns. No existing field
changes meaning. Raw IE bytes plus the generated `.rules.json` provenance are
sufficient to replay the exact fingerprint comparison; see the rule guide for
byte-order and normalization policy.

## Hardware acceptance checklist (not performed)

Host fixtures validate deterministic parsing, matching, forwarding and export;
they cannot establish ESP32-C5 promiscuous delivery, dwell coverage or resource
behavior. Issue #31 remains open until controlled testing on an actual XIAO
ESP32-C5 records these results. Do not claim production Flock detection from the
synthetic fixtures.

1. Flash all nodes with the same v3 firmware; test standalone and child/parent
   configurations. Use a controlled transmitter on a fixed known channel and a
   separate monitor capture to establish exactly which probes were sent.
2. Send directed (including binary SSID), wildcard, missing-SSID, reordered,
   repeated, vendor, malformed and >255-byte IE probes. Compare captured
   transmitter/subtype/IE bytes, channel/RSSI and timing to reference traffic.
   Verify malformed and oversize probes never produce matching records.
3. Enable only a synthetic all-rule with subtype, wildcard, a controlled address,
   and full IE fingerprint. Confirm identical standalone/forwarded priority,
   buzzer/follow actions and evidence. Confirm unrelated phones/APs and directed
   requests for a surveillance-like network name do not trigger that fingerprint
   rule or an advertised-network `ssid_prefix` rule. Separately enable an explicit
   requested-network rule and check that only its requested-name hints match.
4. Confirm there are no transmitted probe requests from the receiver. Exercise
   channel hopping, dwell changes and scan boundaries; compare passive delivery
   counts in each configuration, and repeat existing beacon/probe-response/NAN
   and BLE Remote ID checks.
5. Under sustained probe flood, record firmware image/static RAM size, free/minimum
   heap, task stack high-water marks, raw/observation drop counters, scan health and
   retained-report retry behavior. Check the watchdog remains healthy, matching
   queue priority works, and parent logging remains responsive. Record build ID,
   board, settings, transmitter/monitor capture and duration with results.
