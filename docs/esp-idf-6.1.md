# ESP-IDF 6.1 migration record

ESP-IDF **v6.1** is the only supported SDK. The flashing helper and direct CMake
build reject other releases, including development/dirty version strings. The SDK
reports the release as `v6.1` and its numeric CMake version as `6.1.0`.
[Issue #78](https://github.com/jscottbranson/wi-know/issues/78) remains open for
hardware validation. Adoption of this SDK does **not** establish capture reliability
or a runtime memory improvement; those measurements have not been made.

## Installation and isolation

Follow the [6.1 installation guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/get-started/index.html).
Install the ESP32-C5 tools and activate the new SDK's `export.sh` (or its Windows
terminal). `idf.py --version` must report `ESP-IDF v6.1`. Then run
`python3 tools/flash.py --build-only` before flashing. A message expecting 5.5.1 or
5.5.5 means the repository checkout still has the old helper; update it too.

The helper includes the SDK release and defaults in its build-directory hash.
Manual builds must also use a fresh directory **and** sdkconfig:

```sh
idf.py -C firmware -B build-6.1-universal -DIDF_TARGET=esp32c5 \
  -DSDKCONFIG="$PWD/build-6.1-universal/sdkconfig" build size
```

Retain the old SDK, source revision and build outputs separately for measurement
and rollback. The 5.5.5 baseline is commit `71653a1` (PR #77); use that revision
in a separate checkout with 5.5.5, rather than attempting to build current source
with an unsupported SDK. Never share sdkconfig or build trees across SDKs.

ESP-IDF 6.1 changes `idf.py flash` to fast reflash when cached flashed binaries
exist. It verifies device contents and falls back to a full flash on mismatch.
For a deliberate full write of the bootloader, partition table and application,
use `idf.py ... flash --all`. Neither mode is a full-chip erase; the logs partition
is not part of the application's flash arguments. Keep a partition backup before
hardware migration tests. See [exporting](exporting.md) and [storage](storage.md).

## Source and configuration audit

Reviewed the [5.5 to 6.0 migration guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/migration-guides/release-6.x/6.0/index.html),
[6.0 to 6.1 migration guide](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32c5/migration-guides/release-6.x/6.1/index.html),
and [6.1 release notes/known issues](https://github.com/espressif/esp-idf/releases/tag/v6.1).

- Zcmp is explicitly disabled and a compile guard rejects enabling it: 6.1 lists
  an ESP32-C5 incompatibility.
- FreeRTOS task declarations are included explicitly after driver transitive
  includes were removed. Task/queue creation no longer depends on assertions;
  allocation failures abort explicitly. The application owns the NimBLE host
  task so its creation can also be checked, preserving SDK stack, priority and
  affinity. Assertions-disabled builds are rejected because other initialization
  paths rely on `ESP_ERROR_CHECK`.
- `CONFIG_BT_NIMBLE_STATIC_TO_DYNAMIC=n` remains necessary: the
  [pinned host source](https://github.com/espressif/esp-nimble/blob/139cada0ae932957fa06ba37d17e3c9c2c95c773/nimble/host/src/ble_hs.c)
  still returns early from `void ble_hs_init()` on allocation failure. This is
  not a claim that all NimBLE allocations are static or infallible.
- Observer and extended scanning, including 1M/coded PHY reception, remain
  enabled. Periodic advertising/synchronization and reserved controller
  connection/advertising memory remain disabled. The GATT overlay retains one
  connection/procedure and disables security/pairing. The new pinned NimBLE
  fragment, cancellation and service-discovery paths were re-audited; see
  [Remote ID](remote-id.md) and [GATT discovery](gatt-discovery.md).
- Optional quad PSRAM at 40 MHz remains capability-allocated only; task stacks
  and radio/DMA buffers remain internal. Wi-Fi IRAM throughput optimizations
  remain disabled, with the same bounded RX buffers and disabled aggregation.
- FATFS's changed dynamic-buffer default is overridden with
  `CONFIG_FATFS_USE_DYN_BUFFERS=n`, retaining embedded sector buffers. Partition
  offsets, wear-levelling configuration and application log schema are unchanged.
- The new Picolibc default and FreeRTOS/ring-buffer placement may affect size,
  stack usage, latency and heap. They require measurement, not inferred savings.

## Validation and outstanding evidence

CI builds universal, distributed parent, child, follower, standalone XIAO and
N32R8, and optional GATT configurations using `espressif/idf:v6.1`. It runs the
host suite and prints firmware size reports. Build success does not validate RF
capture, timing, coexistence, flash reset, or peripherals.

Local validation on 2026-11-08 passed all 32 host tests and a fresh XIAO child
firmware build. Negative compilation checks rejected both enabled Zcmp and
disabled assertions. The real 6.1 export tool recovered two synthetic schema-v1
records from separately generated 5.5.5 and 6.1 512 KiB wear-levelled FAT images
(4096-byte sectors, long filenames); recovered source log bytes and raw backups
matched exactly. This checks offline extraction, not mounting/writing on a board.

A fresh 5.5.5 build of baseline commit `71653a1` and the migration build used
`settings.child.example.json`, `--role child`, and `--board xiao`. Their generated
`settings.hpp` files were byte-identical. Both maps were analyzed with the same
6.1 `esp_idf_size --format json2` parser:

| XIAO child metric | 5.5.5 baseline | 6.1 migration | Difference |
| --- | ---: | ---: | ---: |
| Application .bin bytes | 1,256,128 | 1,281,744 | +25,616 (+2.04%) |
| Static HP SRAM bytes | 183,192 | 175,881 | −7,311 |
| HP SRAM code bytes | 103,996 | 93,600 | −10,396 |
| HP SRAM data bytes | 42,036 | 42,825 | +789 |
| HP SRAM BSS bytes | 37,160 | 39,456 | +2,296 |
| Flash sections bytes | 1,109,986 | 1,145,206 | +35,220 |

This configuration has a larger binary and lower static SRAM occupancy. SDK
changes and migration source fixes both contribute; this is not a libc-only
comparison. Static SRAM is not runtime free heap, largest free block, minimum
heap or stack headroom. Those measurements and equivalent N32R8 results remain
pending. Build metadata can slightly change size in subsequent commits.

The following hardware results are **pending**, on both board profiles with the
same configuration/rules and comparable RF input as the 5.5.5 baseline:

| Area | Evidence to collect |
| --- | --- |
| Memory | Internal free heap, largest block, minimum heap, each task's stack high-water mark at boot and during sustained capture; PSRAM present/absent |
| Wi-Fi | 2.4/5 GHz promiscuous capture, channel changes, scheduling with BLE, observation drops and queue pressure |
| BLE | Legacy/extended reports, fragment reassembly, cancellation/restart, optional GATT discovery and recovery |
| Peripherals | RS-485 discovery/reporting, GPS, I²C display, capture switch, LEDs/buzzer |
| Storage | Existing SD/internal logs, append/remount, missing SD fallback, low-space/error handling, flash backup/export |
| Stability | Duration, scan failures, resets, minimum heap trend and stack headroom during matched sustained runs |
| USB | BOOT/RESET recovery and subsequent automatic download entry on XIAO and N32R8 |

Record board/silicon revision, source commit, SDK commit, generated sdkconfig,
config/rules hashes, test duration and both baseline/new observations. Do not
close #78 or claim that memory savings justify the migration until those results
and any capture/reliability tradeoffs have been assessed.
