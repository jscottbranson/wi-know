# Easy firmware flashing

Use the local helper from the repository root. Choose one of four installations:

```sh
python3 tools/flash.py --config config/settings.standalone.example.json --board n32r8
python3 tools/flash.py --config config/settings.standalone.example.json --board xiao
python3 tools/flash.py --config config/settings.parent.example.json --board n32r8
python3 tools/flash.py --config config/settings.child.example.json --board xiao
```

The helper asks for the USB port, validates settings, builds, and then flashes.
Children receive IDs from the parent; no ID prompt is needed. Update parent and
children together when upgrading from fixed-ID firmware. Parent and standalone
configurations assume LCD, GPS, microSD, buzzer, LEDs, and capture-switch wiring;
there are no separate wiring or capture variants. Missing LCD/GPS/indicators do
not stop scanning. An absent SD card falls back to internal flash, and an absent
switch stays OFF via its internal pull-down. The helper prints the resolved
capture GPIO before flashing.

## One-time prerequisites

- Install **ESP-IDF v5.5.5** with its **ESP32-C5** tools using
  [Espressif's setup guide](https://docs.espressif.com/projects/esp-idf/en/v5.5.5/esp32c5/get-started/index.html).
- Activate ESP-IDF before starting the helper. On Linux/macOS, source the installed
  checkout's `export.sh`. On Windows, open its ESP-IDF terminal and use `python`
  instead of `python3` if necessary.
- Connect the board with a USB **data** cable. Close any serial monitor already
  using that port. The full-size parent and mini children/follower all use C5 firmware.

The helper itself uses only Python's standard library; it uses pyserial for port
listing if available in your environment, otherwise falls back to platform discovery.
It does not download/install the SDK, change your shell environment, or run a full-chip
`erase-flash`. Normal flashing writes the firmware's bootloader, partition table and app.

### Upgrading from ESP-IDF v5.5.1

Activate an ESP-IDF **v5.5.5** installation before rebuilding. This release includes
[Espressif's USB reset fix](https://github.com/espressif/esp-idf/commit/ffc08c2)
for ESP32-C5 silicon revisions 1.0 and earlier: with USB Serial/JTAG as the primary
console, the old SDK could disable a UART0 clock needed by ROM during a USB reset.
Affected XIAO children may need one final manual **BOOT/RESET** sequence to flash
the rebuilt firmware. Subsequent USB flashes should enter download mode automatically
when this SDK bug was the cause; verify that behavior on the actual boards.

The firmware retains native USB Serial/JTAG as its primary console. Do not switch
the console to UART0 as a workaround: its default GPIO11/12 signals overlap the
RS-485 wiring, and parent configurations may use UART0 for GPS. The SDK fix changes
clock handling without changing the board's GPIO assignments.

## Commands without the menu

Standalone configs automatically select the parent role when `--role` is omitted:

```sh
python3 tools/flash.py --port /dev/ttyACM1 --config config/settings.standalone.example.json
```

### Select the board wiring

Use `--board xiao` or `--board n32r8` to apply the GPIO assignments from the
corresponding wiring diagram. This is an explicit build-time selection; the
firmware does not detect the carrier board or discover connected peripherals.
For a standalone N32R8 with LCD, GPS, SD, buzzer and both LEDs enabled:

```sh
python3 tools/flash.py --config config/settings.standalone.example.json --board n32r8 --dry-run
python3 tools/flash.py --config config/settings.standalone.example.json --board n32r8
```

Use `--board xiao` with the same config for the standalone mini XIAO schematic.
The helper prints the resolved GPIOs before building and flashing. Board selection
preserves peripheral enable flags, standalone/bus mode, rules, and timing settings;
it replaces every mapped pin, including individual LED assignments and GPS TX
(GPS TX is GPIO8 on N32R8 and GPIO0 on XIAO). For omitted individual
LEDs or other custom wiring, use `--board custom` and set the GPIOs in your config.

You can persist the choice as `"board": {"model": "n32r8"}` in settings. The CLI
option takes precedence. Without either, `custom` preserves existing pin settings.
The helper uses the role stored in the config. A board profile applied to a
universal config selects the parent role; pass
`--role child` or `--role follower` for workers. The parent selects
scanning/follower roles at runtime.
Board profiles cannot be combined with `--role universal`: keep the existing
universal configuration and identification harness for that workflow.

For direct IDF builds, use `-DWIKNOW_BOARD=n32r8` (or `xiao`) together with
`-DWIKNOW_ROLE=parent`. The generator also accepts `--board`. Each board profile
gets an isolated flash-helper build directory, and firmware logs its selected
board at startup. This selects wiring only; flash size and PSRAM settings remain
controlled by the existing SDK configuration.

Explicit `--role` choices override the config and are validated; standalone mode
requires `parent`. The default `settings.json` uses universal mode.

Universal image (repeat for every board, changing only the USB port):

```sh
python3 tools/flash.py --port /dev/ttyACM0 --config config/settings.json
python3 tools/flash.py --build-only --config config/settings.json
python3 tools/flash.py --dry-run
```

The following explicit roles use the configured wiring without the identification
harness. All bus-enabled builds use discovery and parent-assigned worker IDs.
Parent:

```sh
python3 tools/flash.py --role parent --port /dev/ttyACM0
```

Child with a parent-assigned ID:

```sh
python3 tools/flash.py --role child --port /dev/ttyACM0
```

Worker using the follower build option, followed by a serial monitor
(the parent determines its runtime scanning/follower role):

```sh
python3 tools/flash.py --role follower --port /dev/ttyACM0 --monitor
```

On Windows, replace the port with a value such as `COM5`. On macOS, use the listed
`/dev/cu.*` device. Exit the ESP-IDF monitor with **Ctrl+]**.

To list ports or inspect the exact commands without building/flashing:

```sh
python3 tools/flash.py --list-ports
python3 tools/flash.py --role parent --dry-run
python3 tools/flash.py --role child --dry-run
```

A dry run needs neither an installed SDK nor a board. It still validates the config
and rules. Missing port is shown as `<serial-port>` without probing devices.

Build without touching hardware:

```sh
python3 tools/flash.py --role child --build-only
```

Use a different settings file, SDK location, or flashing speed:

```sh
python3 tools/flash.py --role parent --port /dev/ttyACM0 --config /path/to/settings.json
python3 tools/flash.py --role parent --port /dev/ttyACM0 --idf-path /path/to/esp-idf
python3 tools/flash.py --role child --port /dev/ttyACM0 --baud 115200
```

`--idf-path` locates an installed SDK; its tools/Python environment still needs
activation. The helper accepts exactly the tested release, **v5.5.5**.

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
build/devices/<role>-<config-board-sdk-version-and-defaults-hash>/
```

Each directory has its own generated header and `sdkconfig`. Workers receive
their IDs at runtime. Switching roles,
board profiles, or config paths selects a separate build directory. Workers with
the same settings share a build. Changes to the same settings/rule files are
picked up by subsequent builds. The build tree is ignored by Git.

The directory hash also includes the pinned SDK version and `firmware/sdkconfig.defaults`.
When the SDK version or defaults change, the helper uses a new build directory so
existing SDK settings cannot silently keep an old memory profile or SDK cache.
Previous build outputs are left intact. If
you flash manually with `@flash_args`, use the **Build:** directory printed by the
latest helper invocation. Manual `idf.py` users should use a fresh build directory
and an explicit `-DSDKCONFIG=/absolute/path/to/new-build/sdkconfig` when adopting
a new SDK version or changed SDK defaults.

The memory profile enables optional quad PSRAM at 40 MHz, allocates the parent
tracking table there when available, and disables Wi-Fi IRAM throughput
optimizations. It retains both 1M and coded-PHY extended BLE scanning. Startup
prints the tracking-table location and available internal heap. Missing PSRAM
does not prevent boot: parents fall back to internal tracking RAM; workers do
not allocate the table. Hardware tests still need to verify radio and storage
headroom on boards without PSRAM. Task stacks and radio/DMA buffers stay internal.

`CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=n` retains the previous static allocation of
NimBLE host state covered by that option. This avoids a newly enabled startup
allocation path that can return early on out-of-memory without failing port
initialization. BLE still uses heap allocations elsewhere; total memory use and
radio/storage headroom must be measured on the boards.

If flashing cannot connect, check the selected port, USB data cable, and operating
system serial-port permissions. If needed, hold **BOOT**, tap **RESET**, release
**BOOT**, and retry. Lower the baud rate with `--baud 115200` if transfers fail.
A failed build skips flashing; a failed flash skips monitoring, and the helper
returns a nonzero exit status. Ctrl+C cancels the current operation.

The tool's build and control flow are tested without attached hardware. USB
flashing/reset behavior still needs verification on the physical boards. See
[the wiring guide](wiring.md) before connecting the RS-485 bus and peripherals.
