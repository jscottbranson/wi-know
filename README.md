# wi-know

A distributed wireless observation system built around ESP32 microcontrollers.
Children divide the configured 2.4 GHz and 5 GHz Wi-Fi channels; a parent
coordinates them, scans BLE, evaluates detection rules, and records observations.
A dedicated follower spends additional receive time on a selected matching signal.

## Project status

The initial C++ firmware targets ESP-IDF v5.5.1 on ESP32-C5, with parent, child,
and follower roles. It includes passive radio reception, coordination, matching,
GPS/peak tracking, and peripheral drivers. Host tests and firmware builds check the
software; physical hardware validation is still required. Production signatures and
Remote ID telemetry decoding are not yet included.

## Hardware

| Role / peripheral | Purpose |
| --- | --- |
| Full-size ESP32-C5-WROOM-1 parent, header layout TBD | Coordination, BLE reception, matching, storage, UI and time |
| Seeed Studio XIAO ESP32-C5 children | Passive Wi-Fi observation across assigned channels |
| Dedicated XIAO ESP32-C5 follower | Focused observation and target reacquisition |
| Parent microSD adapter, probably HW-125 (unconfirmed) | SPI storage; matching observations or all observations, selectable |
| Parent passive buzzer | Configurable match alerts |
| Parent 1602A LCD with backpack | Two rows of 16 characters; assume I²C pending backpack identification |
| Parent Grove GPS (Air530), with onboard backup battery | Receiver coordinates and UTC from NMEA over UART; internal RTC backup |
| Three parent LEDs | Selected signal strength increasing, decreasing, or steady |
| Optional separate battery-backed RTC | Fallback only if Air530 startup time availability proves insufficient |

The C5 supports 2.4/5 GHz Wi-Fi and Bluetooth LE. Bluetooth Classic requires
additional compatible hardware. A node listens to one Wi-Fi channel at a time;
dual-band support does not mean simultaneous observation of both bands. Wi-Fi
and BLE coexistence must be measured on the chosen parent.

GPS coordinates describe the **receiver**, not the detected transmitter. Drone
position, identity, altitude, velocity and direction are available only when
transmitted in a supported, decoded broadcast such as Remote ID. RSSI is not a
distance or bearing measurement. RFID requires a separate suitable reader;
Remote ID and RFID are different technologies. Proprietary drone links and
non-Wi-Fi RF are outside the initial receiver's capabilities.

## Design

See [architecture](docs/architecture.md) for scheduling, protocol, detection,
GPS association, peak RSSI, LED behavior, clock fallback, and implementation stages.
**Edit [config/settings.json](config/settings.json)** for device settings and GPIOs.
**Store matching rules in [rules/](rules/README.md)** as JSON documents.
See [configuration help](config/README.md) and [build/run instructions](docs/firmware.md).
Settings and rules are validated and compiled into firmware; rebuild after changes.
Parent peripheral pins and physical header positions remain unassigned. Nodes are
assumed to travel together when sharing the parent's GPS.

## Wiring

![Parent, children and follower on the shared RS-485 bus](docs/images/rs485-network.png)

See the [wiring guide](docs/wiring.md) for child/follower pin connections and parallel
power distribution, with editable SVGs and PNG copies. The full-size parent uses
an ESP32-C5-WROOM-1 module; its physical header layout and power-input pin remain
provisional until the specific development board is identified.

## Quick start

After installing and activating ESP-IDF v5.5.1, connect a board by USB and run:

```sh
python3 tools/flash.py
```

Select **parent**, **child**, or **follower**, choose its node ID and serial port,
and the helper builds and flashes it. See [easy flashing instructions](docs/flashing.md)
for setup, command-line options, dry runs and troubleshooting.

For a manual build without flashing:

With ESP-IDF v5.5.1 activated, run from this directory:

```sh
python3 tools/generate_config.py
idf.py -C firmware -B build-parent -DIDF_TARGET=esp32c5 -DWIKNOW_ROLE=parent -DWIKNOW_NODE_ID=0 build
```

The RS-485 bus is enabled on TX GPIO11 (D6), RX GPIO12 (D7), and RTS GPIO7 (D3).
Unassigned peripherals remain disabled. The default
ruleset is empty; a disabled test template is in `rules/examples/`. See the build
instructions for child/follower builds, flashing, and current implementation limits.

## Primary references

- [XIAO ESP32-C5 hardware](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/)
- [ESP32-C5 datasheet](https://www.espressif.com/sites/default/files/documentation/esp32-c5_datasheet_en.pdf)
- [ESP-IDF C5 RF coexistence](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c5/api-guides/coexist.html)
- [Grove Air530 documentation and NMEA serial example](https://wiki.seeedstudio.com/Grove-GPS-Air530/)
- [OpenDroneID Core C](https://github.com/opendroneid/opendroneid-core-c)
- [Eye Spy](https://github.com/simeononsecurity/eye-spy)
- [OUI Spy Unified Blue](https://github.com/colonelpanichacks/oui-spy-unified-blue)

Review source licenses and exact revisions before incorporating signatures or code.
No third-party signatures or code have been imported.
