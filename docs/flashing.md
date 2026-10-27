# Easy firmware flashing

Use the local helper from the repository root:

```sh
python3 tools/flash.py
```

It asks you to select:

1. **Parent**, **child**, or **follower**.
2. A node ID for a child/follower; the parent always uses ID 0.
3. The connected USB serial port, with a rescan option.

It validates configuration/rules, checks the SDK version, builds the selected
firmware, and flashes only after the build succeeds. The repo's settings file is
not rewritten. Connect one board at a time for the simplest setup. Every child
and follower needs a different ID within the configured discovery range (currently
1–8); the helper cannot detect IDs already assigned to disconnected boards.

## One-time prerequisites

- Install **ESP-IDF v5.5.1** with its **ESP32-C5** tools using
  [Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c5/get-started/index.html).
- Activate ESP-IDF before starting the helper. On Linux/macOS, source the installed
  checkout's `export.sh`. On Windows, open its ESP-IDF terminal and use `python`
  instead of `python3` if necessary.
- Connect the board with a USB **data** cable. Close any serial monitor already
  using that port. The full-size parent and mini children/follower all use C5 firmware.

The helper itself uses only Python's standard library; it uses pyserial for port
listing if available in your environment, otherwise falls back to platform discovery.
It does not download/install the SDK, change your shell environment, or run a full-chip
`erase-flash`. Normal flashing writes the firmware's bootloader, partition table and app.

## Commands without the menu

Parent:

```sh
python3 tools/flash.py --role parent --port /dev/ttyACM0
```

Child with node ID 3:

```sh
python3 tools/flash.py --role child --node-id 3 --port /dev/ttyACM0
```

Dedicated follower with node ID 2, followed by a serial monitor:

```sh
python3 tools/flash.py --role follower --node-id 2 --port /dev/ttyACM0 --monitor
```

On Windows, replace the port with a value such as `COM5`. On macOS, use the listed
`/dev/cu.*` device. Exit the ESP-IDF monitor with **Ctrl+]**.

To list ports or inspect the exact commands without building/flashing:

```sh
python3 tools/flash.py --list-ports
python3 tools/flash.py --role parent --dry-run
python3 tools/flash.py --role child --node-id 3 --dry-run
```

A dry run needs neither an installed SDK nor a board. It still validates the config
and rules. Missing port is shown as `<serial-port>` without probing devices.

Build without touching hardware:

```sh
python3 tools/flash.py --role child --node-id 3 --build-only
```

Use a different settings file, SDK location, or flashing speed:

```sh
python3 tools/flash.py --role parent --port /dev/ttyACM0 --config /path/to/settings.json
python3 tools/flash.py --role parent --port /dev/ttyACM0 --idf-path /path/to/esp-idf
python3 tools/flash.py --role child --node-id 3 --port /dev/ttyACM0 --baud 115200
```

`--idf-path` locates an installed SDK; its tools/Python environment still needs
activation. The helper accepts exactly the tested release, **v5.5.1**.

## Build isolation and troubleshooting

Build products are stored under:

```text
build/devices/<role>-<node-id>-<config-path-hash>/
```

Each directory has its own generated header and `sdkconfig`. Switching roles,
IDs, or config paths cannot accidentally reuse another device's settings. Changes
to the same settings/rule files are picked up by subsequent builds. The build tree
is ignored by Git.

If flashing cannot connect, check the selected port, USB data cable, and operating
system serial-port permissions. If needed, hold **BOOT**, tap **RESET**, release
**BOOT**, and retry. Lower the baud rate with `--baud 115200` if transfers fail.
A failed build skips flashing; a failed flash skips monitoring, and the helper
returns a nonzero exit status. Ctrl+C cancels the current operation.

The tool's build and control flow are tested without attached hardware. USB
flashing/reset behavior still needs verification on the physical boards. See
[the wiring guide](wiring.md) before connecting the RS-485 bus and peripherals.
