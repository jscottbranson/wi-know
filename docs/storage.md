# SD and internal-flash logging

At boot, a parent tries the configured microSD interface if `sd.enabled` is true.
If the card mounts and a new log can be opened with enough free space, logging
uses SD. Otherwise it tries the internal `logs` partition. With SD disabled,
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
Reboot to select storage again. SD cards are never automatically formatted.

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

Both media store JSON Lines in `WK########.LOG`, starting a new file on each boot.
Only files with those exact names are eligible for retention deletion. Before a
new file opens, oldest logs are removed to satisfy the file-count limit and leave
space for a full next file plus 8 KiB of metadata headroom. Unrelated files are
never deleted; if they occupy the space needed, logging cannot open a new file.

SD uses `logging.rotate_bytes`, `logging.max_files`, and
`logging.flush_interval_ms`. Flash caps those first two settings at **32 KiB per
file and eight files**, including the active file; smaller configured limits
still apply. Flash flushes dirty data no more often than every five seconds
(or the configured interval if longer), using buffered writes and wear levelling.
Recent buffered records can be lost at power failure. Flash has finite write
endurance; SD is preferable for sustained `logging.mode: "all"` captures.

Storage starts after radio initialization. Its queue holds two records; slow
mounts or writes can cause records to be dropped, counted in `local_drops` along
with other local losses. If there is insufficient RAM for the storage task,
serial reports the failure and console logging continues. Hardware validation
must check free heap, flash writes during Wi-Fi/BLE reception, and drop counts.

## Reading flash logs

Close the serial monitor and put the board in download mode before readback.
For this partition layout, in an activated ESP-IDF v5.5.1 terminal:

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
`WK*.LOG` files to [the export tool](exporting.md), just like SD logs. Tap RESET
with BOOT released afterward to resume operation. Entering download mode can
lose records that were still buffered in RAM.

## Hardware acceptance check

1. With a formatted, writable SD card inserted and SD configured, boot and verify
   `storage=SD`, generate a lab detection, and verify a JSON record on the card.
2. Power down, remove the card, and boot. Verify `storage=FLASH` and the LCD's
   storage page, generate detections, and read back the flash logs.
3. Reboot without a card and confirm a new file is created while earlier logs
   remain. Exercise rotation with a small configured limit.
4. Reinsert the SD card while powered off. Confirm the next boot selects SD and
   leaves earlier flash logs intact. Confirm Wi-Fi/BLE remain active in each case.
