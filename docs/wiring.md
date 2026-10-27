# Wiring diagrams

The parent is a **full-size development board with an ESP32-C5-WROOM-1 module**.
The children and dedicated follower are **mini Seeed XIAO ESP32-C5 boards**.
These are logical connection diagrams, not physical header-position drawings.
The parent's exact development-board model/header layout is still unconfirmed;
no physical header positions or parent VIN/5V pin are assigned here.

The existing firmware targets the C5 chip family used by both boards. Current
`config/settings.json` assigns UART1 TX **GPIO11**, RX **GPIO12**, and RTS **GPIO7**.
On the parent, locate these by **GPIO number** after checking its board pinout;
XIAO D-number labels do not identify positions on the full-size board. GPIO11/12
may also be labeled TX0/RX0, but the firmware routes UART1 to these GPIOs. Use
USB Serial/JTAG for the console as configured, and verify the parent's USB connection
supports that interface rather than assuming an onboard USB-UART bridge is equivalent.

## Each mini child and follower

![XIAO ESP32-C5 child and follower wiring](images/xiao-rs485-wiring.png)

[Editable SVG](images/xiao-rs485-wiring.svg)

| XIAO label | GPIO / function | Local 3.3 V RS-485 module |
| --- | --- | --- |
| D6 | GPIO11 / TX | DI (sometimes D) |
| D7 | GPIO12 / RX | RO (sometimes R) |
| D3 | GPIO7 / direction | DE and active-low `/RE`, tied together |
| 3V3 | Regulated output | VCC for a module designed for 3.3 V |
| GND | Ground | GND |

Use a manually controlled, half-duplex transceiver with **3.3 V supply and logic
compatibility**. A module that drives RO at 5 V is unsuitable for direct ESP32 input.
This wiring corresponds to circuit B in
[Espressif's RS-485 interface documentation](https://docs.espressif.com/projects/esp-idf/en/v5.5.1/esp32c5/api-reference/peripherals/uart.html#interface-connection-options).
The D-to-GPIO mapping comes from the
[Seeed XIAO ESP32-C5 pin map](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/).

## Parent and all nodes on one data bus

![Shared RS-485 network](images/rs485-network.png)

[Editable SVG](images/rs485-network.svg)

Provide one transceiver per board, including the parent. Connect A to A, B to B,
and grounds together for this non-isolated bus. Use a twisted pair for A/B in a
linear daisy chain with short stubs. Connection dots indicate junctions; crossings
without dots are not connected. Physical cable order does not determine software
roles or node IDs.

Fit 120 Ω termination across A/B at each physical cable end, accounting for any
termination already on the modules. Middle nodes should not add termination.
Check module documentation for bias resistors/failsafe behavior and A/B naming;
use one appropriate bias network if required. See the
[TI RS-485 transceiver datasheet](https://www.ti.com/lit/ds/symlink/thvd1450.pdf)
for bus topology and termination guidance.

Do not tie bare ESP32 TX pins together. Give the parent ID 0 and each child/follower
a unique nonzero ID within the parent's configured discovery range. Node roles
and IDs are set when building; see [firmware instructions](firmware.md).

## Power distribution

![Parallel power distribution](images/power-distribution.png)

[Editable SVG](images/power-distribution.svg)

For the mini XIAOs, distribute **regulated 5 V to VBUS** and ground to GND, in
parallel, or power each board via USB. Their `3V3` header pin is documented as
`3V3_OUT`; use it to supply that board's own 3.3 V transceiver. Do not connect
different boards' 3V3 outputs together or feed external power into those outputs.
See the [Seeed power/pin documentation](https://wiki.seeedstudio.com/xiao_esp32c5_getting_started/).

For the full-size parent, use its documented USB power input for now. Its exact
external 5V/VIN input and connector arrangement must be checked against the actual
board. The ESP32-C5-WROOM-1 module itself is a 3.3 V device; a module supply pin must
not be mistaken for a development board's regulated 5 V input. See the
[Espressif module datasheet](https://documentation.espressif.com/esp32-c5-wroom-1_wroom-1u_datasheet_en.html).

Budget supply current for all boards, their transceivers and the parent's peripherals.
Do not assume the LCD backpack or probable HW-125 accepts a particular supply voltage
until identified. Parent GPS, LCD, SD, buzzer and LED pin wiring will be added after
its header layout and peripheral electrical requirements are confirmed.

## Image sources

These diagrams are generated deterministically from editable Python/SVG sources,
so labels and wire endpoints do not depend on image-model rendering. PNG copies
are provided for repository previews. Regenerate SVGs with standard Python:

```sh
python3 tools/generate_wiring.py
```

To also regenerate PNGs, install `resvg-py` in your chosen Python environment and run:

```sh
python3 tools/generate_wiring.py --png
```

The renderer needs an installed sans-serif font. On systems without fonts, pass
`--font /path/to/font.ttf` (repeatable); the included PNGs were rendered with Noto Sans.
No image-generation API or secret is needed. Update the diagrams when changing
pin assignments. These diagrams document the intended wiring; hardware operation
has not yet been verified on the physical boards.
