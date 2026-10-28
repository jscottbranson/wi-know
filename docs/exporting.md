# Sharing and mapping detections

Copy the `WK*.LOG` JSON Lines files from the parent's microSD card. Export them
with Python 3; no additional packages are needed:

```sh
python3 tools/export_observations.py /path/to/logs/WK*.LOG -o detections.json
python3 tools/export_observations.py /path/to/logs/WK*.LOG -o detections.csv
python3 tools/export_observations.py /path/to/logs/WK*.LOG -o detections.geojson
```

For internal flash, close the serial monitor, activate ESP-IDF v5.5.1, and run:

```sh
python3 tools/export_flash.py --port /dev/ttyACM1 --format csv
```

This automatically resets the board into download mode, backs up its logs, and
restarts it after readback. It then extracts the logs and writes `detections.csv`
in a new `tmp/flash-export-*` directory in the repository. No BOOT/RESET button presses are normally
needed on USB connections supporting automatic reset. Omit `--port` to select it
interactively. JSON is the default; JSONL, TSV and GeoJSON are also supported.
See [flash export options and recovery](storage.md#reading-flash-logs).

The output extension selects the format; use `--format` to override it. With no
`-o`, the default is JSON on stdout. Supported formats:

| Format | Use |
| --- | --- |
| `json` | A JSON array for uploads and sharing; preserves all record fields |
| `jsonl` | One JSON object per line for downstream processing |
| `csv` | Spreadsheet imports with a header row |
| `tsv` | The same columns separated by tabs |
| `geojson` | A FeatureCollection for map imports and sharing |

All formats include observations, following alerts, and decoded drone detections.
JSON, JSONL and GeoJSON preserve nested fields and raw evidence. CSV/TSV include
Remote ID protocol, message identifiers, authentication flag, raw bytes, and the
nested `data` object as JSON in a single cell.

CSV contains source file/line, receiver node and boot/session IDs, address, radio,
channel, SSID/name, matched rule, category, confidence, signal strength, capture
time, UTC when known, and receiver coordinates for both the capture and its peak.
Text that could execute as a spreadsheet formula receives a leading apostrophe.
JSON, JSONL and GeoJSON retain the unmodified text.

GeoJSON can be opened in GIS software as a point layer. Coordinates describe
**where the receiver observed the signal**, not the transmitter's position.
Unknown locations stay as null geometry; they are never placed at zero latitude
and longitude. The original capture and peak fields remain in feature properties.
Use `--location peak` to map the receiver's position at the strongest observed
sample for that track. Following alerts have no peak and retain null geometry in
peak mode.

For a map, export `detections.geojson`, then import or upload it into a mapping
tool that accepts GeoJSON. Share that file with recipients who need the same
points and detection details. For drone broadcasts, choose which position to map:

```sh
python3 tools/export_observations.py /path/to/logs/WK*.LOG -o aircraft.geojson --location aircraft
python3 tools/export_observations.py /path/to/logs/WK*.LOG -o operators.geojson --location operator
```

Aircraft points come from Remote ID Location messages; operator points come from
System messages. These are self-reported broadcast coordinates. Records without
the selected position retain null geometry. `location_role` identifies receiver,
aircraft, or operator points; no positions are inferred or merged across messages.
Exports contain addresses, identifiers and potentially precise locations; review
these before uploading publicly. The script creates local files and does not
upload them to a service.

Use `--matches-only` to include rule matches, drone detections and following alerts while excluding
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
