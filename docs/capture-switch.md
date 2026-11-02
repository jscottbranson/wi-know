# Physical full-capture sessions

The optional maintained switch enables full structured observation logging live,
without rebooting or changing scan assignments, detection rules or alert actions.
Follow the [wiring and example configurations](wiring.md#optional-full-capture-switch-manual-parent--standalone).
The feature is disabled and unassigned by default. Existing configurations remain
valid; add this section to enable it with a supported board and available pin:

```json
"capture_switch": {
  "enabled": true,
  "gpio_pin": 8,
  "internal_pulldown": false,
  "debounce_ms": 50
}
```

This example is N32R8 GPIO8, with `board.model: "n32r8"` and
`signal_trend.steady_pin: -1`. Use GPIO0 and `board.model: "xiao"` on standalone
XIAO. Flash the matching capture example. GPIO configuration requires a rebuild;
switch changes then apply live. Startup samples the physical level immediately;
later changes require a stable level for `debounce_ms` (10–2000 ms). Sampling
runs in the parent loop, so bus polling/storage latency adds to response time.

ON closes any previous WU stream and opens a fresh numbered `WUxxxxxxxx.LOG`
with a `capture_mode` event. All subsequent supported observations go into this
session. Matching records continue in `WMxxxxxxxx.LOG` and are also copied to
WU, including all matching classifications. WM files retain their ordinary size
rotation. WU may rotate at the configured size or under retention pressure, so a
long session can span multiple files. `boot` plus the ON event's `at_ms` identify
its start. OFF appends an end event, synchronizes storage and closes WU. Ordered
commands keep queued records on the correct side of each session boundary.
Sessions are selected when the controller processes observations; worker/queue
backlogs can carry older capture timestamps across a switch edge.
An ON switch at boot also creates a fresh session; switch bounce does not.

OFF returns to `logging.mode`: `matches` omits ordinary unmatched observations,
while `all` continues the existing unmatched stream in a subsequent WU file.
Existing Remote ID and following/persistence event behavior is preserved. With
the switch disabled, normal WM/WU logging remains in effect. JSON schema fields
for observations remain unchanged: available radio/address/address type,
SSID/name, channel, RSSI, payload, capture timestamp, UTC and GPS/time-quality
information are retained. Capture-mode events include uptime, receiver boot ID,
reason, effective mode, switch level, UTC/time source and available receiver fix.

The LCD has a separate `Capture:ALL` / `Capture:MATCH` page with storage destination
(`SD`, `FLASH`, `OFF`) and switch state. Mode is independent of storage health;
ALL does not mean records are being saved if storage is OFF. Transition events
also go to the console. Queue, parsing, serialization, retention and storage
failures contribute to the local drop counter; worker counters are reported
separately. Storage errors and retention drops also appear in console diagnostics.

## Capture and retention limits

This is structured JSON capture of supported observations actually received by
the configured scanner, not PCAP, decrypted traffic, all RF transmissions, or
simultaneous coverage of all channels. Switching mode does not change any radio
filter or worker assignment. The current path was audited:

- Wi-Fi management/data parsing supports the documented frame types; unsupported
  and invalid frames are discarded before observation creation. Raw Wi-Fi and
  BLE parse queues are bounded. BLE assembly/payload limits and optional GATT
  allowlists still apply; full capture does not enable active GATT discovery.
- Parent admission accepts observations independently of detection rules. Child
  admission covers assigned Wi-Fi; the dedicated follower stays restricted to
  its assigned target. The parent scans BLE; standalone alternates Wi-Fi/BLE.
- Observation queues reserve 16 matching and 32 ordinary slots and drain matches
  first. Worker reports carry one observation per transaction, retain pending
  observations through retries, and expose drop counters. Bus baud, polling,
  retries and node count limit aggregate throughput; dense traffic can overflow.
- During switch capture, the two-entry storage queue reserves one slot for matched/control records;
  unmatched records cannot consume it. Matched records are written first, then
  mirrored into the capture file. The console is separately bounded. No finite
  queue guarantees lossless capture; slow SD/flash or console output can lose data.
- WU capture files (including their matched copies) keep unmatched retention
  priority. Existing WM matching evidence is protected from WU eviction. WU
  writes can be refused when only protected files remain; counters expose this.
  Transition metadata is also best-effort if storage fails or retention blocks it.
  OFF still attempts to flush/close prior data if the end event cannot be stored.
- Flash uses small bounded rotating files and at least a five-second ordinary
  flush interval. Transitions request immediate sync; frequent switching adds
  flash writes. Use SD for sustained capture. Power down before removing media.

Validate both switch boot positions, scanning-time edges and GPS timing on the
physical assembly before field use. Host tests exercise policy/debounce,
configuration validation, event JSON, session file separation and retention;
firmware builds cannot establish electrical or sustained-throughput performance.

## GPS stamps and first/last seen

Every saved ordinary observation, matched or unmatched, includes `capture_ms`,
`utc_ms`, `time_source`, `gps_sync_age_ms`, `timing_uncertainty_ms` and
`receiver_fix` (latitude, longitude and fix uptime). GPS tagging uses capture time,
not file-write time or the latest GPS position. Coordinates describe the receiver.
Without GPS time, UTC is `null`; without a sufficiently fresh fix, location is
`null`. GPS time can continue in holdover even when a current location is unavailable.

Each observation also has `first_seen_ms`, `last_seen_ms`, `first_seen_utc_ms`
and `last_seen_utc_ms`. Those are the live track's endpoints: receiver/node boot,
radio/address type/address, channel and BLE acquisition kind identify a track.
The 64-entry tracker can evict old entries, and restarting resets it. These fields
therefore do not claim lifetime first/last detection. Out-of-order captures can
extend first seen backwards but cannot move last seen backwards.

For a clear address-level report across all selected capture files, use:

```sh
python3 tools/export_observations.py WU*.LOG --sightings -o sightings.csv
```

Each row groups radio + address type + address within a logging-controller boot,
combining receivers and channels. It gives first/last capture uptime, UTC
milliseconds, readable UTC timestamps, and first/last receiver GPS fixes. JSON,
JSONL and TSV are also supported. Input order and duplicated WM/WU matching
copies do not change endpoints. The report uses actual observation records rather
than the bounded track's cached history. Choose only the session's files for a
session-specific report; include all its rotated files. Reboots remain separate
rows because uptime resets. Different/randomized addresses remain separate signals.

These are first and last sightings **in the supplied logs**, not proof of when a
device arrived or left. Queue drops, channel dwell gaps, retention and missing files
can hide earlier/later sightings. Unknown endpoint UTC/location stays unknown.
