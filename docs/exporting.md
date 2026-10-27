# Sharing and mapping detections

Copy the `WK*.LOG` JSON Lines files from the parent's microSD card. Export them
with Python 3; no additional packages are needed:

```sh
python3 tools/export_observations.py /path/to/logs/WK*.LOG --format csv -o detections.csv
python3 tools/export_observations.py /path/to/logs/WK*.LOG --format geojson -o detections.geojson
```

CSV contains source file/line, receiver node and boot/session IDs, address, radio,
channel, SSID/name, matched rule, category, confidence, signal strength, capture
time, UTC when known, and receiver coordinates for both the capture and its peak.
Text that could execute as a spreadsheet formula receives a leading apostrophe.
The original JSONL and GeoJSON retain the unmodified text.

GeoJSON can be opened in GIS software as a point layer. Coordinates describe
**where the receiver observed the signal**, not the transmitter's position.
Unknown locations stay as null geometry; they are never placed at zero latitude
and longitude. The original capture and peak fields remain in feature properties.
Use `--location peak` to map the receiver's position at the strongest observed
sample for that track. Following alerts have no peak and retain null geometry in
peak mode.

Use `--matches-only` to include rule matches and following alerts while excluding
unmatched observations. Every matching rule gets a separate record, so an
observation that matches several rules appears several times with the same
`receiver_boot`, `node`, `boot`, and `sequence`. Count those identities, rather
than rows, when counting distinct captures. Tracks are scoped to a source node,
worker boot, radio, address and channel; different receivers' RSSI values are
not directly calibrated. Input files retain their supplied order.

Capture and output queues are bounded. Busy radio traffic, many simultaneous rule
matches, a slow console, or slow storage can drop records; monitor the device's
loss counters. An export describes recorded observations and cannot establish
complete radio coverage or recover records that never reached storage.

Malformed input stops the export without replacing an existing output. After a
power interruption, a final JSONL line can be incomplete; `--skip-invalid`
explicitly skips malformed records and reports their file and line on stderr.
Status events are omitted from detection exports but remain in the source logs.
An output path cannot replace an input log. Omitting `-o` writes the result to
stdout. Files are processed one record at a time; output is staged on disk before
publication so large logs do not require loading the complete dataset in memory.

Archive the generated `settings.rules.json` beside the logs. It records the rule
predicates, source provenance and effective enabled state for that build. Keep it
with the matching firmware and settings; a later rebuild may use different rules
with the same IDs. The shipped ruleset is empty, and unavailable categories are
reported during generation; exports do not add signatures or infer device types.
