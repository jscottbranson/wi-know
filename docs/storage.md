# SD and internal-flash logging

At boot, a parent tries the configured microSD interface if `sd.enabled` is true.
If the card mounts and its log directory and free-space information can be read,
logging uses SD. Files open lazily when records arrive; startup does not delete
evidence just to create an empty log. Otherwise it tries the internal `logs` partition. With SD disabled,
the parent goes directly to flash; unassigned SD pins are never accessed.
Workers do not mount either filesystem. USB console output remains independent.

Serial startup and periodic parent status identify `storage=SD`, `FLASH`, or
`OFF`; the LCD's coverage/storage page shows the same destination. `WAIT` means
storage initialization has not finished. In standalone mode, the scan page and
storage page alternate with the other status pages.

Selection happens once per boot. Insert/remove the card while powered off, then
restart. There is no hot-swap support or automatic copying between flash and SD.
If the selected medium later fails to write, flush, or rotate, persistent logging
stops with `OFF` and an error on serial; scanning and console output continue.
Reboot to select storage again. SD formatting is disabled by default; see the
explicit opt-in below.

## Preparing an unformatted SD card

In the settings file you actually pass to the flashing helper, set these fields
inside the existing `sd` object (keep your verified GPIO settings):

```json
"enabled": true,
"format_if_mount_failed": true
```

Rebuild and flash with that config, then boot with the card inserted. ESP-IDF
first initializes the card and attempts a normal FAT mount. With this option
enabled it may format a filesystem reported as missing/unrecognized or internally
damaged (`FR_NO_FILESYSTEM` or `FR_INT_ERR`) and retry mounting. This can erase
existing files, including a non-FAT filesystem; use only a card whose contents
you intend to replace. A readable FAT filesystem is mounted without formatting.
Memory-allocation, absent-card, and communication failures do not authorize
formatting. `ESP_ERR_NO_MEM` must be fixed before a card can be initialized.

The helper prints a warning when formatting is enabled. After the card mounts
successfully, set the option back to `false` and rebuild/reflash to prevent future
mount failures from formatting the card. Old settings files that omit the option
retain the safe default `false`. Formatting is not offered for internal flash
through this setting.

## Flash layout and retention

The partition table reserves 512 KiB at `0x180000` for a wear-levelled FAT
filesystem. The app starts at `0x10000` and has `0x170000` bytes available. The
whole layout still fits 2 MiB boards; an 8 MiB device does not need a different
image. This reduces the old app partition by 512 KiB, so use a full normal flash
(bootloader, partition table, and app), not an app-only update. Build checks
enforce the smaller app limit. Normal subsequent flashes leave the logs partition
alone; a full-chip erase or an older/larger app can destroy it. Back up logs before
changing firmware layouts.

A completely erased flash log partition is formatted on first use. Existing
nonempty contents are never automatically reformatted after a mount failure.
This includes old firmware bytes left in that region, a damaged filesystem, or
an interrupted first-time format. Such failures report `OFF`; preserve/read back
the partition before deliberately erasing it for recovery.

Both media store separate JSON Lines streams: `WM########.LOG` contains records
from observations that match at least one enabled rule; `WU########.LOG` contains
unmatched observations and status records. All records from one matched
observation (including every matching classification, decoded Remote ID record,
and any following alert) carry the matched classification explicitly through the
storage queue. A decoded Remote ID broadcast or following alert **without a rule
match is unmatched**, even in matches-only logging mode. Neither implies a rule
match. Console JSON and export schemas remain unchanged.

The two streams share one file-count budget. Before opening a file, retention
removes **unmatched files first, oldest sequence number first**, then (only for
incoming matched records) the oldest matched files. Unmatched arrivals never
delete matched evidence: if it cannot make room, the record is discarded,
`local_drops` increases, and serial reports a rate-limited retention-drop total.
Logging stays enabled so later matched records can still be stored. Deletion is
whole-file, not per observation: rotating a matched file can remove several
observations/classifications together.

At most two files are open. Retention never unlinks an open file: a matched
arrival may first close the unmatched stream to reclaim it. Both streams count
against the configured limit, including a limit of one. Space reservations cover
a full file per open stream (including buffered writes), rounded up to the
mounted volume's actual allocation-cluster size, plus 8 KiB of metadata headroom. Unrelated files are never deleted; if they occupy all usable space,
incoming records are dropped. Filesystem/open/write/flush failures instead stop
persistent logging with `OFF`. Do not modify the mounted filesystem concurrently.

Existing `WK########.LOG` files may contain mixed data. They are never rewritten,
appended, or scanned for JSON classification; they receive **matched retention
priority** to protect unknown evidence, and can only be deleted for new matched
records. New files use sequence numbers above all existing owned names, so a
restart never appends to an earlier session. Only regular files with exactly the
`WK`, `WM`, or `WU` prefix, eight decimal digits, and `.LOG` suffix are eligible
for deletion. Export accepts all three prefixes, preserving legacy compatibility.

SD uses `logging.rotate_bytes`, `logging.max_files`, and
`logging.flush_interval_ms`. Flash caps those first two settings at **32 KiB per
file and eight files**, including both active streams; smaller configured limits
still apply. Flash flushes dirty data no more often than every five seconds
(or the configured interval if longer), using buffered writes and wear levelling.
Recent buffered records can be lost at power failure. Flash has finite write
endurance; SD is preferable for sustained `logging.mode: "all"` captures.

Storage starts after radio initialization. Parent tracking state uses PSRAM when
available, leaving internal RAM for radio, storage, and task allocations. Its queue holds two records; slow
mounts or writes can cause records to be dropped, counted in `local_drops` along
with other local losses. If there is insufficient RAM for the storage task,
serial reports the failure and console logging continues. Hardware validation
must check free heap, flash writes during Wi-Fi/BLE reception, and drop counts.

## Reading flash logs

Close the serial monitor, connect the parent over USB, and run this in an
activated ESP-IDF v5.5.1 terminal:

```sh
python3 tools/export_flash.py --port /dev/ttyACM1 --format csv
```

The helper uses automatic USB reset to enter download mode and restart the board
after readback, so BOOT/RESET button presses are normally unnecessary. Automatic
reset depends on the board's USB connection and still needs physical validation.
Scanning pauses during readback, and resetting can lose records buffered in RAM.
The helper reads only the `logs` partition described by this repository's
`firmware/partitions.csv`; use it with firmware using that same layout.
It does not build, flash, or erase firmware.

Omit `--port` for an interactive port menu. Each invocation creates a new
`tmp/flash-export-*` directory in the repository containing the raw `logs.bin` backup, an `extracted/`
directory of original logs (including status records), and `detections.json` by
default. Use `--format csv`, `tsv`, `jsonl`, or `geojson` to change the export.
Use `--directory my-backup` to name the directory; it must not already exist.
The repository's `tmp/` directory is ignored by Git. Raw and extracted data are
retained if extraction or conversion fails.

Re-export a saved backup without connecting a board:

```sh
python3 tools/export_flash.py --image tmp/flash-export-EXAMPLE/logs.bin --format geojson
```

Both modes accept `--location`, `--matches-only`, and `--skip-invalid` as described
in [the export guide](exporting.md). Offline extraction still requires ESP-IDF's
FAT parser and activated Python environment. `--idf-path` can select that checkout.
Malformed records fail conversion by default; use `--skip-invalid` explicitly
to recover complete records from a log with an interrupted final write.

If automatic connection fails, check the port, USB cable, and serial permissions.
For boards requiring manual reset, hold BOOT, tap RESET, release BOOT, then run
the helper with `--manual-reset`. Tap RESET afterward to resume scanning. A failed
or interrupted read may also leave the board in download mode; reset it if needed.

For manual readback and extraction of this partition layout:

```sh
mkdir -p flash-log-backup
cd flash-log-backup
python -m esptool --chip esp32c5 --port /dev/ttyACM1 --baud 115200 \
  --before no_reset --after no_reset --no-stub \
  read_flash 0x180000 0x80000 logs.bin
python "$IDF_PATH/components/fatfs/fatfsparse.py" --wl-layer enabled logs.bin
```

Use the board's current port. Keep `logs.bin` as a raw backup. ESP-IDF's parser
extracts the FAT directory tree under its volume-label directory in the current
directory; use a fresh backup directory for each extraction. Feed the extracted
`W[KMU]*.LOG` files to [the export tool](exporting.md), just like SD logs. Tap RESET
with BOOT released afterward to resume operation. Entering download mode can
lose records that were still buffered in RAM.

## Hardware acceptance check

1. With a formatted, writable SD card inserted and SD configured, boot and verify
   `storage=SD`, generate a lab detection, and verify a JSON record on the card.
2. Power down, remove the card, and boot. Verify `storage=FLASH` and the LCD's
   storage page, generate detections, and read back the flash logs.
3. Reboot without a card and generate another detection. Confirm a new file is
   created while earlier logs remain. Exercise rotation with a small configured limit.
4. Reinsert the SD card while powered off. Confirm the next boot selects SD and
   leaves earlier flash logs intact. Confirm Wi-Fi/BLE remain active in each case.
