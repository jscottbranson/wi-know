# Easy firmware flashing

Use the local helper from the repository root:

```sh
python3 tools/flash.py
```

The helper asks only for the USB port. It builds the **same universal image** for
all boards: hardware identification selects the parent profile, and the parent
assigns worker IDs and child/follower roles over RS-485. Fit the identification
harness described in [universal wiring](universal.md) before first boot.

It validates settings/rules and builds successfully before flashing. Connect each
board in turn and run the same command with the same configuration. For wired
parent peripherals use `--config config/settings.universal.example.json` on **every**
board. Do not pass role or node-ID arguments. The default leaves peripherals off.

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

Standalone configs automatically select the parent role when `--role` is omitted:

```sh
python3 tools/flash.py --port /dev/ttyACM1 --config config/settings.standalone-wiring.example.json
```

Other configs default to the universal role. Explicit `--role` choices are still
validated; standalone mode requires `parent`.

Universal image (repeat for every board, changing only the USB port):

```sh
python3 tools/flash.py --port /dev/ttyACM0 --config config/settings.universal.example.json
python3 tools/flash.py --build-only --config config/settings.universal.example.json
python3 tools/flash.py --dry-run
```

The following explicit roles retain manual provisioning for a **separate legacy
bus**. Manual and universal nodes must not share a bus. Parent:

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

### Parent boot indicators

After reset, an identified parent tests enabled, assigned signal LEDs in order:
increasing, decreasing, then steady, for 200 ms each. An enabled buzzer then chirps
for 100 ms at its configured frequency. All outputs return to off before normal
operation. This adds at most 700 ms to startup and does not consume an alert
cooldown. Disabled peripherals and LED pins set to `-1` are skipped; workers do
not run the test. The default configuration leaves these peripherals disabled.

With the LCD enabled, the screen then shows `wi-know` / `Starting...` before
cycling through status pages. The serial log reports parent peripheral enable
states (or that a worker skipped them), followed by the LCD SDA/SCL GPIOs and I2C
address. A completed LED/buzzer test confirms software drove the outputs, not
that the attached hardware responded; verify the light and sound physically.

For a blank LCD, check the serial identity message says `detected=parent`, verify
the flashed config enables the display, and compare its logged GPIOs/address with
your wiring. `No response at configured I2C address` means the initial LCD write
failed. If the backlight is on but characters are absent, check the backpack's
contrast adjustment and its PCF8574 mapping against the wiring guide. The boot
test does not diagnose or repair LCD wiring.

Build products are stored under:

```text
build/devices/<role>-<node-id>-<config-and-sdk-defaults-hash>/
```

Each directory has its own generated header and `sdkconfig`. Switching roles,
IDs, or config paths cannot accidentally reuse another device's settings. Changes
to the same settings/rule files are picked up by subsequent builds. The build tree
is ignored by Git.

The directory hash also includes `firmware/sdkconfig.defaults`. When those defaults
change, the helper uses a new build directory so existing SDK settings cannot
silently keep an old memory profile. Previous build outputs are left intact. If
you flash manually with `@flash_args`, use the **Build:** directory printed by the
latest helper invocation. Manual `idf.py` users should use a fresh build directory
and an explicit `-DSDKCONFIG=/absolute/path/to/new-build/sdkconfig` when adopting
changed SDK defaults.

The memory profile enables optional quad PSRAM at 40 MHz, allocates the parent
tracking table there when available, and disables Wi-Fi IRAM throughput
optimizations. It retains both 1M and coded-PHY extended BLE scanning. Startup
prints the tracking-table location and available internal heap. Missing PSRAM
does not prevent boot: parents fall back to internal tracking RAM; workers do
not allocate the table. Hardware tests still need to verify radio and storage
headroom on boards without PSRAM. Task stacks and radio/DMA buffers stay internal.

If flashing cannot connect, check the selected port, USB data cable, and operating
system serial-port permissions. If needed, hold **BOOT**, tap **RESET**, release
**BOOT**, and retry. Lower the baud rate with `--baud 115200` if transfers fail.
A failed build skips flashing; a failed flash skips monitoring, and the helper
returns a nonzero exit status. Ctrl+C cancels the current operation.

The tool's build and control flow are tested without attached hardware. USB
flashing/reset behavior still needs verification on the physical boards. See
[the wiring guide](wiring.md) before connecting the RS-485 bus and peripherals.
