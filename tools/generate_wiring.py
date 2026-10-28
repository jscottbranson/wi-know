#!/usr/bin/env python3
"""Regenerate deterministic wiring SVGs; --png also needs resvg-py."""
import argparse
import json
from html import escape
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'docs/images'
FONT_FILES = []
INK = '#183044'
MUTED = '#526577'
BLUE = '#176aa5'
GREEN = '#147d64'
ORANGE = '#a85e08'
RED = '#b83543'
GROUND = '#405165'


class Diagram:
    def __init__(self, title, subtitle, height=850):
        self.width, self.height = 1440, height
        self.parts = [
            f'<svg xmlns="http://www.w3.org/2000/svg" width="1440" height="{height}" viewBox="0 0 1440 {height}" role="img" aria-labelledby="title description">',
            f'<title id="title">{escape(title)}</title>',
            f'<desc id="description">{escape(subtitle)}</desc>',
            '<defs><marker id="arrow-blue" markerWidth="8" markerHeight="8" refX="6" refY="3" orient="auto-start-reverse"><path d="M0,0 L0,6 L6,3 z" fill="#176aa5"/></marker></defs>',
            f'<rect width="1440" height="{height}" fill="#f6f8fb"/>',
        ]
        self.text(48, 50, 'wi-know / wiring', size=18, color=BLUE, weight=700)
        self.text(48, 99, title, size=34, weight=700)
        self.text(48, 133, subtitle, size=19, color=MUTED)
        self.line([(48, 159), (1392, 159)], '#d5dfe8', width=2)

    def text(self, x, y, text, size=20, color=INK, weight=400, anchor='start'):
        self.parts.append(f'<text x="{x}" y="{y}" fill="{color}" font-family="DejaVu Sans, Noto Sans, sans-serif" font-size="{size}" font-weight="{weight}" text-anchor="{anchor}">{escape(text)}</text>')

    def box(self, x, y, w, h, fill='white', stroke='#d0dce6', dashed=False):
        dash = ' stroke-dasharray="8 6"' if dashed else ''
        self.parts.append(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="14" fill="{fill}" stroke="{stroke}" stroke-width="2"{dash}/>')

    def line(self, points, color, width=3, dashed=False, arrow=False):
        coords = ' '.join(f'{x},{y}' for x, y in points)
        dash = ' stroke-dasharray="8 6"' if dashed else ''
        marker = ' marker-end="url(#arrow-blue)"' if arrow else ''
        self.parts.append(f'<polyline points="{coords}" fill="none" stroke="{color}" stroke-width="{width}" stroke-linejoin="round" stroke-linecap="round"{dash}{marker}/>')

    def dot(self, x, y, color):
        self.parts.append(f'<circle cx="{x}" cy="{y}" r="5" fill="{color}"/>')

    def note(self, y, heading, lines, color=BLUE):
        self.box(48, y, 1344, 56 + len(lines) * 28, fill='#edf3f8', stroke='#d0dce6')
        self.text(70, y + 33, heading, size=21, color=color, weight=700)
        for index, line in enumerate(lines):
            self.text(70, y + 66 + index * 28, line, size=18)

    def save(self, name, png, footer='Logical wiring • connectors not drawn to physical pin order • see docs/wiring.md'):
        self.text(48, self.height - 24, footer, size=15, color=MUTED)
        content = '\n'.join(self.parts + ['</svg>'])
        path = OUT / f'{name}.svg'
        path.write_text(content)
        if png:
            import resvg_py
            rendered = resvg_py.svg_to_bytes(svg_string=content, zoom=1.5, font_files=FONT_FILES, sans_serif_family='Noto Sans')
            (OUT / f'{name}.png').write_bytes(rendered)
        print(path.relative_to(ROOT))


def node_wiring(png):
    d = Diagram('XIAO child / follower connections', 'Same local wiring on every XIAO ESP32-C5. Add one manual-direction, 3.3 V RS-485 module per node.', 1000)
    d.box(70, 194, 410, 475)
    d.text(100, 236, 'XIAO ESP32-C5', size=27, weight=700)
    d.text(100, 267, 'Child or dedicated follower', color=MUTED, size=18)
    d.box(830, 194, 360, 475)
    d.text(855, 235, 'RS-485 module', size=26, weight=700)
    d.text(855, 267, '3.3 V supply + logic', color=MUTED, size=18)
    entries = [
        (320, 'D6 / GPIO11', 'TX', 'DI / D', BLUE, False),
        (390, 'D7 / GPIO12', 'RX', 'RO / R', BLUE, True),
        (460, 'D3 / GPIO7', 'RTS', 'DE', ORANGE, False),
        (580, '3V3 (output)', '3.3 V', 'VCC', RED, False),
        (630, 'GND', 'ground', 'GND', GROUND, False),
    ]
    for y, pin, label, module, color, reverse in entries:
        d.text(100, y + 7, pin, weight=700)
        d.text(700, y - 13, label, color=color, anchor='end', size=17)
        d.text(855, y + 7, module, weight=700)
        points = [(480, y), (830, y)]
        d.line(list(reversed(points)) if reverse else points, color, arrow=(color == BLUE))
        d.dot(480, y, color)
        d.dot(830, y, color)
    d.line([(755, 460), (755, 510), (830, 510)], ORANGE)
    d.dot(755, 460, ORANGE)
    d.text(855, 517, '/RE', weight=700)
    d.text(570, 541, 'Tie DE and /RE together', size=18, color=ORANGE)
    for y, label, color in [(350, 'A', GREEN), (420, 'B', BLUE)]:
        d.text(1157, y + 7, label, color=color, weight=700)
        d.line([(1190, y), (1315, y)], color)
        d.text(1210, y - 16, f'to shared {label}', size=17, color=color)
    d.text(1230, 491, 'A/B are bus lines.', size=16, color=MUTED, anchor='middle')
    d.text(1230, 517, 'Not ESP32 GPIOs.', size=16, color=MUTED, anchor='middle')
    d.note(700, 'Current config/settings.json bus values', [
        'enabled: true    uart: 1    tx_pin: 11    rx_pin: 12    rts_pin: 7    baud: 115200',
        'Use GPIO numbers in JSON. The D6 / D7 / D3 labels apply specifically to the XIAO ESP32-C5.',
    ])
    d.note(836, 'Power is separate from data', [
        'Supply each XIAO through USB or regulated 5 V at VBUS + GND. 3V3 powers its local transceiver.',
        'Do not join multiple boards’ 3V3 outputs. Never connect raw TX outputs together.',
    ], RED)
    d.save('xiao-rs485-wiring', png)


def bus_wiring(png):
    d = Diagram('Shared RS-485 data bus', 'One daisy chain connects the parent, scanning children, and dedicated follower. Power wiring is shown separately.', 1000)
    centers = [210, 550, 890, 1230]
    names = ['FULL-SIZE PARENT', 'CHILD 1', 'CHILD 2', 'FOLLOWER']
    for index, (x, name) in enumerate(zip(centers, names)):
        d.box(x - 142, 191, 284, 111, fill='#edf3f8' if index == 0 else 'white', dashed=index == 0)
        d.text(x, 225, name, size=20, weight=700, anchor='middle')
        d.text(x, 254, 'ESP32-C5-WROOM-1' if index == 0 else 'XIAO ESP32-C5', size=17, anchor='middle', color=MUTED)
        d.text(x, 280, 'TX 11 · RX 3 · RTS 7*' if index == 0 else 'D6 TX · D7 RX · D3 RTS', size=15, anchor='middle', color=MUTED)
        d.line([(x, 302), (x, 343)], ORANGE, dashed=index == 0)
        d.text(x + 13, 329, 'TX / RX / direction', size=12, color=MUTED)
        d.box(x - 115, 343, 230, 90)
        d.text(x, 375, 'RS-485 transceiver', size=18, weight=700, anchor='middle')
        d.text(x, 404, 'One at every board', size=15, color=MUTED, anchor='middle')
    rails = [(500, 'A', GREEN), (555, 'B', BLUE), (610, 'GND', GROUND)]
    for y, label, color in rails:
        d.line([(115, y), (1325, y)], color, width=4)
        d.text(55, y + 6, label, size=20, weight=700, color=color)
    for x in centers:
        for offset, (y, label, color) in zip([-42, 0, 42], rails):
            d.text(x + offset + 9, 455, label, size=14, color=color)
            d.line([(x + offset, 433), (x + offset, y)], color, width=2)
            d.dot(x + offset, y, color)
    # Termination across A/B at the two physical cable ends only.
    for x, label_x in [(115, 130), (1325, 1190)]:
        d.line([(x, 500), (x, 515)], INK, width=2)
        d.box(x - 9, 515, 18, 25, fill='white', stroke=INK)
        d.line([(x, 540), (x, 555)], INK, width=2)
        d.dot(x, 500, GREEN)
        d.dot(x, 555, BLUE)
        d.text(label_x, 537, '120 Ω', size=16, weight=700)
    d.text(720, 657, 'Dots mark electrical junctions. Unmarked crossings do not connect.', size=18, color=MUTED, anchor='middle')
    d.note(690, 'Bus layout', [
        'Run A and B as a twisted pair; keep node stubs short. Join the grounds of this non-isolated bus.',
        'Fit termination only at the two cable ends. Check existing module termination before adding resistors.',
        'Use identical module A/B conventions; provide one appropriate bias network if required by the modules.',
    ])
    d.note(853, 'N32R8 parent uses the separate peripheral example config', [
        '*Parent GPIO11 / 3 / 7; workers GPIO11 / 12 / 7. Parent GPIO12 is tied to USB-UART TX.',
    ], ORANGE)
    d.save('rs485-network', png)


def power_wiring(png):
    d = Diagram('Power distribution', 'XIAO boards receive regulated 5 V in parallel. N32R8 parent uses its native USB power input.', 1040)
    d.box(70, 195, 1300, 105, fill='#edf3f8')
    d.text(100, 235, 'REGULATED 5 V SUPPLY / USB POWER BANK', size=25, weight=700)
    d.text(100, 272, 'Verify output voltage and total current capacity for all boards and peripherals.', size=19, color=MUTED)
    d.line([(1240, 300), (1240, 345)], RED)
    d.line([(1320, 300), (1320, 395)], GROUND)
    d.text(1228, 327, '+5 V', size=15, color=RED, anchor='end')
    d.text(1308, 327, 'GND', size=15, color=GROUND, anchor='end')
    d.dot(1240, 345, RED)
    d.dot(1320, 395, GROUND)
    d.line([(110, 345), (1330, 345)], RED, width=4)
    d.line([(110, 395), (1330, 395)], GROUND, width=4)
    d.text(70, 351, '+5 V', size=19, weight=700, color=RED, anchor='end')
    d.text(70, 402, 'GND', size=19, weight=700, color=GROUND, anchor='end')
    centers = [230, 720, 1210]
    for x, name in zip(centers, ['CHILD 1', 'CHILD 2 / MORE', 'FOLLOWER']):
        d.line([(x - 40, 345), (x - 40, 480)], RED)
        d.line([(x + 40, 395), (x + 40, 480)], GROUND)
        d.dot(x - 40, 345, RED)
        d.dot(x + 40, 395, GROUND)
        d.text(x - 40, 465, 'VBUS', size=17, color=RED, anchor='middle')
        d.text(x + 40, 465, 'GND', size=17, color=GROUND, anchor='middle')
        d.box(x - 145, 480, 290, 100)
        d.text(x, 517, name, size=20, weight=700, anchor='middle')
        d.text(x, 550, 'XIAO ESP32-C5', size=18, anchor='middle', color=MUTED)
        d.line([(x - 40, 580), (x - 40, 625)], RED)
        d.line([(x + 40, 580), (x + 40, 625)], GROUND)
        d.text(x - 47, 607, '3V3 OUT', size=14, color=RED, anchor='end')
        d.text(x + 47, 607, 'GND', size=14, color=GROUND)
        d.box(x - 145, 625, 290, 76)
        d.text(x, 656, 'Local 3.3 V transceiver', size=18, anchor='middle', weight=700)
        d.text(x, 682, 'VCC from this XIAO only', size=16, color=MUTED, anchor='middle')
    d.note(730, 'N32R8 parent: native USB input; see parent header and peripheral diagrams', [
        'Use the left USB connector. Leave parent P1.14 / 5V unconnected with the separate LCD/buzzer supply.',
        'For this non-isolated RS-485 bus, connect parent GND to the same common ground.',
    ], ORANGE)
    d.note(868, 'Keep the voltage rails separate', [
        'Do not feed external power into XIAO 3V3_OUT. Do not connect different boards’ 3V3 outputs together.',
        'Do not attach the LCD backpack or HW-125 to a rail until its actual voltage requirements are confirmed.',
    ], RED)
    d.save('power-distribution', png)


def led_wiring(d, led_pins=(0, 1, 8), title='1  Three active-high trend LEDs'):
    d.box(48, 350, 650, 405)
    d.text(70, 388, title, size=25, weight=700)
    for y, pin, label, color in [(450, led_pins[0], 'Increasing / green', GREEN),
                                  (535, led_pins[1], 'Decreasing / red', RED),
                                  (620, led_pins[2], 'Steady / amber', ORANGE)]:
        d.text(70, y + 6, f'GPIO{pin}', size=19, weight=700)
        d.line([(155, y), (190, y)], BLUE)
        d.box(190, y - 10, 70, 20, stroke=INK)
        d.text(225, y - 20, '470 Ω', size=17, anchor='middle')
        d.line([(260, y), (320, y)], BLUE)
        d.parts.append(f'<path d="M320,{y-15} L320,{y+15} L346,{y} Z" fill="none" stroke="{color}" stroke-width="2"/>')
        d.line([(346, y - 15), (346, y + 15)], color)
        d.line([(346, y), (395, y)], GROUND)
        d.dot(395, y, GROUND)
        d.text(300, y + 30, 'A', size=15)
        d.text(350, y + 30, 'K', size=15)
        for dx in (0, 14):
            d.line([(330 + dx, y - 23), (344 + dx, y - 37)], color, width=2)
            d.parts.append(f'<path d="M{344+dx},{y-37} l-8,3 l5,5 Z" fill="{color}"/>')
        d.text(420, y + 6, label, size=18, color=color)
    d.line([(395, 450), (395, 683)], GROUND)
    d.text(395, 711, 'GND', size=18, anchor='middle', color=GROUND)
    d.text(70, 735, 'One resistor per LED. Cathode K = flat edge / short lead.', size=17, color=MUTED)


def buzzer_wiring(d, buzzer_pin=9, title='2  Passive buzzer + NPN driver'):
    d.box(720, 350, 672, 405)
    d.text(742, 388, title, size=25, weight=700)
    d.text(1055, 422, '+5 V regulated', size=18, color=RED, anchor='middle')
    d.line([(1055, 430), (1055, 453)], RED)
    d.box(1005, 453, 100, 65, stroke=ORANGE)
    d.text(1055, 481, 'BZ1 +', size=18, anchor='middle')
    d.text(1055, 505, '−', size=20, anchor='middle')
    d.line([(1055, 518), (1055, 550), (1030, 575)], INK)
    d.text(1045, 563, 'C', size=17)
    d.line([(1030, 565), (1030, 615)], INK, width=4)
    d.line([(1030, 605), (1055, 630), (1055, 657)], GROUND)
    d.parts.append(f'<path d="M1052,627 l-12,-4 l7,-7 Z" fill="{GROUND}"/>')
    d.text(1067, 634, 'E', size=17)
    d.text(1090, 583, 'Q1 2N3904', size=18, weight=700)
    d.text(1090, 610, 'NPN', size=17, color=MUTED)
    d.text(742, 584, f'GPIO{buzzer_pin}', size=18, weight=700)
    d.line([(820, 590), (845, 590)], BLUE)
    d.box(845, 580, 60, 20, stroke=INK)
    d.text(875, 568, '1 kΩ', size=17, anchor='middle')
    d.line([(905, 590), (1030, 590)], BLUE)
    d.text(1000, 578, 'B', size=17)
    d.dot(955, 590, BLUE)
    d.line([(955, 590), (955, 610)], GROUND)
    d.box(945, 610, 20, 30, stroke=INK)
    d.text(937, 633, '10 kΩ', size=17, anchor='end')
    d.line([(955, 640), (955, 657), (1055, 657)], GROUND)
    d.dot(1055, 657, GROUND)
    d.text(1055, 681, 'GND', size=18, anchor='middle', color=GROUND)
    # Flyback diode across the load: cathode at positive rail, anode at collector.
    d.line([(1055, 440), (1265, 440), (1265, 469)], RED)
    d.dot(1055, 440, RED)
    d.line([(1251, 469), (1279, 469)], INK)
    d.parts.append(f'<path d="M1251,495 L1279,495 L1265,469 Z" fill="none" stroke="{INK}" stroke-width="2"/>')
    d.line([(1265, 495), (1265, 535), (1055, 535)], INK)
    d.dot(1055, 535, INK)
    d.text(1290, 478, 'K', size=16)
    d.text(1290, 507, 'A', size=16)
    d.text(1158, 523, 'D1 1N4148', size=16, anchor='middle')
    d.text(742, 710, 'BZ1: 5 V passive magnetic buzzer, 20 mA max; D1 band to +5 V.', size=17)
    d.text(742, 737, 'Q1 lead order: check your part datasheet. No GPIO load drive.', size=17, color=MUTED)


def lcd_wiring(d, lcd_pins=(4, 5), board='PARENT', title='3  1602A LCD + 5 V PCF8574 backpack'):
    d.box(48, 780, 1344, 470)
    d.text(70, 821, title, size=25, weight=700)
    d.text(70, 851, 'Two bidirectional BSS138 channels. LV = 3.3 V; HV = 5 V; all grounds joined.', size=19, color=MUTED)
    d.box(70, 930, 210, 160, fill='#edf3f8')
    d.text(175, 960, board, size=21, weight=700, anchor='middle')
    d.box(570, 930, 300, 160)
    d.text(720, 958, 'I²C level shifter', size=21, weight=700, anchor='middle')
    d.text(720, 985, '2 × BSS138', size=18, anchor='middle', color=MUTED)
    d.box(1155, 930, 210, 160, fill='#edf3f8')
    d.text(1260, 960, 'LCD backpack', size=20, weight=700, anchor='middle')
    for y, pin, signal in [(1015, lcd_pins[0], 'SDA'), (1065, lcd_pins[1], 'SCL')]:
        d.text(90, y + 6, f'GPIO{pin} / {signal}', size=18)
        d.line([(280, y), (570, y)], BLUE)
        d.text(590, y + 6, f'LV {signal}', size=17)
        d.text(850, y + 6, f'HV {signal}', size=17, anchor='end')
        d.line([(870, y), (1155, y)], ORANGE)
        d.text(1175, y + 6, signal, size=18)
    # Pull-ups on both bus sections; SCL runs below SDA with unmarked crossings.
    for x, y, rail, color in [(345, 1015, '3.3 V', BLUE), (475, 1065, '3.3 V', BLUE),
                               (935, 1015, '+5 V', ORANGE), (1065, 1065, '+5 V', ORANGE)]:
        d.text(x, 888, rail, size=16, anchor='middle', color=color)
        d.line([(x, 896), (x, 907)], color)
        d.box(x - 9, 907, 18, 35, stroke=INK)
        d.text(x + 16, 930, '4.7 kΩ', size=16)
        d.line([(x, 942), (x, y)], color, width=2)
        d.dot(x, y, color)
    d.text(70, 1130, f'Shifter: LV supply to {board.lower()} 3V3; HV supply to regulated +5 V; GND to common ground.', size=18)
    d.text(70, 1160, 'Each MOSFET: gate to 3.3 V; source to LV signal; drain to matching HV signal.', size=18)
    d.text(70, 1190, 'LCD VCC to +5 V; GND to common ground. Configured address 0x27; verify address and adjust contrast.', size=18)
    d.text(70, 1220, 'Pull-ups shown are TOTAL per line/side: reuse fitted resistors, do not blindly add parallel sets.', size=18, color=MUTED)


def indicator_wiring(d, led_pins=(0, 1, 8), buzzer_pin=9, lcd_pins=(4, 5), board='PARENT'):
    led_wiring(d, led_pins)
    buzzer_wiring(d, buzzer_pin)
    lcd_wiring(d, lcd_pins, board)


def standalone_wiring(png):
    d = Diagram('XIAO ESP32-C5 / standalone with GPS + microSD',
                'Priority: screen > GPS > microSD > buzzer > LEDs. All five fit on the 11 header GPIOs.', 2790)
    d.note(182, 'Manual standalone parent / config/settings.standalone-wiring.example.json', [
        'Screen D4/23 + D5/24; GPS RX D7/12; microSD SCK D8/8, MISO D9/9, MOSI D10/10, CS D6/11.',
        'Buzzer D3/7; LEDs D0/1 increasing, D1/0 decreasing, D2/25 steady. Labels show D-number / GPIO.',
        'Use native USB for flash / console. Bus disabled; no RS-485 module or universal identification harness.',
    ])
    # Reuse the verified circuits, presenting the blocks in build-priority order.
    d.parts.append('<g transform="translate(0,-430)">')
    lcd_wiring(d, (23, 24), 'XIAO', '1  Screen / 1602A LCD + 5 V PCF8574 backpack')
    d.parts.append('</g>')

    d.box(48, 845, 1344, 265)
    d.text(70, 886, '2  GPS / Grove Air530 / receive-only UART0 at 9600 baud', size=25, weight=700)
    for y, left, right, color, incoming in [
        (928, 'D7 / GPIO12 / UART0 RX', 'GPS TX (module output)', BLUE, True),
        (970, 'XIAO 3V3 output', 'GPS VCC (3.3 V supply)', RED, False),
        (1012, 'XIAO GND', 'GPS GND', GROUND, False),
    ]:
        d.text(90, y + 6, left, size=19)
        d.text(920, y + 6, right, size=19)
        points = [(880, y), (525, y)] if incoming else [(525, y), (880, y)]
        d.line(points, color, arrow=incoming)
    d.text(70, 1054, 'GPS RX: leave disconnected (tx_pin = -1). Use connector signal labels to establish orientation.', size=18)
    d.text(70, 1083, '100 nF at GPS VCC/GND. Coordinates locate this receiver; a fresh valid fix is required for tagging.', size=18, color=MUTED)

    d.box(48, 1135, 1344, 455)
    d.text(70, 1176, '3  microSD / native 3.3 V SPI breakout / SPI2 at 4 MHz', size=25, weight=700)
    for y, left, right, color, incoming in [
        (1220, 'D8 / GPIO8', 'SCK / CLK (card contact 5)', BLUE, False),
        (1260, 'D10 / GPIO10', 'MOSI / DI / CMD (contact 3)', BLUE, False),
        (1300, 'D9 / GPIO9', 'MISO / DO / DAT0 (contact 7)', BLUE, True),
        (1340, 'D6 / GPIO11', 'CS / DAT3 (contact 2)', BLUE, False),
        (1380, 'XIAO 3V3 output', 'VDD (contact 4)', RED, False),
        (1420, 'XIAO GND', 'VSS (contact 6)', GROUND, False),
    ]:
        d.text(90, y + 6, left, size=19)
        d.text(920, y + 6, right, size=19)
        points = [(880, y), (525, y)] if incoming else [(525, y), (880, y)]
        d.line(points, color, arrow=(color == BLUE))
    d.text(70, 1464, '10 kΩ to 3.3 V on CMD and DAT0–DAT3, including unused DAT1 (8) and DAT2 (1); none on CLK.', size=18)
    d.text(70, 1493, 'Account for fitted pull-ups. Contact numbers are for the card, not the adapter header order.', size=18)
    d.text(70, 1522, '100 nF + 10 µF at VDD/VSS; keep SPI wires short. GPIO11 is CS: do not attach a UART console there.', size=18)
    d.text(70, 1551, 'HW-125 variants need supply/logic verification; this drawing assumes a native 3.3 V breakout.', size=18, color=MUTED)

    d.parts.append('<g transform="translate(-672,1265)">')
    buzzer_wiring(d, 7, '4  Buzzer / passive + NPN driver')
    d.parts.append('</g>')
    d.parts.append('<g transform="translate(694,1265)">')
    led_wiring(d, (1, 0, 25), '5  LEDs / three optional trend indicators')
    d.parts.append('</g>')

    d.box(48, 2045, 1344, 400)
    d.text(70, 2086, 'Power + antenna / common ground for every fitted block', size=25, weight=700)
    d.box(70, 2120, 310, 165, fill='#edf3f8')
    d.text(95, 2155, 'Computer / USB supply', size=22, weight=700)
    d.text(95, 2190, 'USB-C power', size=20)
    d.text(95, 2220, '+ data for flash / console', size=18)
    d.box(600, 2120, 310, 165, fill='#edf3f8')
    d.text(625, 2155, 'XIAO ESP32-C5', size=23, weight=700)
    d.line([(380, 2200), (600, 2200)], BLUE)
    d.text(490, 2180, 'USB-C', size=18, anchor='middle')
    d.text(625, 2195, '3V3 -> GPS + SD + LV', size=18)
    d.text(625, 2230, 'GND -> common ground', size=19)
    d.text(625, 2260, '5V / VBUS: leave open', size=18, color=MUTED)
    d.box(1060, 2120, 305, 165)
    d.text(1080, 2155, 'Supplied antenna', size=22, weight=700)
    d.text(1080, 2220, 'U.FL connector', size=20)
    d.line([(910, 2165), (1060, 2165)], GREEN)
    d.text(70, 2324, 'Separate regulated +5 V -> LCD VCC + shifter HV + buzzer positive; supply GND -> XIAO GND.', size=19)
    d.text(70, 2353, '100 nF at LCD VCC/GND; 10 µF across local +5 V/GND. Keep independently powered +5 V rails apart.', size=18)
    d.text(70, 2382, 'Allow regulator headroom for board/radio peaks, GPS and card writes; verify stable 3.3 V under load.', size=18)
    d.text(70, 2411, 'Do not feed external power into 3V3_OUT. Use a separate suitable 3.3 V supply if the load exceeds its budget.', size=18)
    d.note(2470, 'Build in priority order / remove optional blocks as needed', [
        'Screen: 2 GPIOs; + GPS: 3 total; + microSD: 7; + buzzer: 8; + three LEDs: 11. No free header GPIOs.',
        'All five sections are enabled in this example. Disable absent sections; omit individual LEDs with pin=-1.',
        'One LED shows only its assigned trend. Unknown/stale signal turns LEDs off. Rebuild and flash after edits.',
        'LCD remains a 1602A / PCF8574 at 0x27; small OLED displays need a different driver.',
        'Hardware unverified. Check Q1 pin order, module supply voltages and board silkscreen before powering.',
    ], ORANGE)
    d.save('xiao-standalone-wiring', png)


def parent_wiring(png):
    d = Diagram('Parent LEDs, buzzer, LCD, GPS and microSD',
                'ESP32-C5 WiFi6 N32R8 • GPIO/header mapping from supplied pinout and Waveshare schematic.', 2190)
    d.note(182, 'N32R8 parent GPIO map — config/settings.parent-wiring.example.json', [
        'LEDs: GPIO0 increasing / GPIO1 decreasing / GPIO8 steady. Buzzer PWM: GPIO9.',
        'LCD: GPIO4 SDA / GPIO5 SCL. GPS: GPIO10 RX (UART0). GPS RX is unused; tx_pin = -1.',
        'Parent RS-485: GPIO11 TX / GPIO3 RX / GPIO7 RTS. microSD: SCK23 / MOSI24 / MISO2 / CS25.',
    ])

    indicator_wiring(d)

    d.box(48, 1275, 1344, 235)
    d.text(70, 1316, '4  Grove GPS Air530 — receive-only UART0 at 9600 baud', size=25, weight=700)
    for y, left, right, color, reverse in [
        (1355, 'Parent GPIO10 / UART0 RX', 'GPS TX (module output)', BLUE, True),
        (1400, 'Parent 3V3 output', 'GPS VCC (use 3.3 V supply)', RED, False),
        (1445, 'Parent GND', 'GPS GND', GROUND, False),
    ]:
        d.text(70, y + 6, left, size=19, weight=700)
        d.text(930, y + 6, right, size=19)
        points = [(470, y), (900, y)]
        d.line(list(reversed(points)) if reverse else points, color, arrow=reverse)
    d.text(70, 1484, 'GPS RX: leave disconnected. Use connector signal labels; check orientation rather than cable color.', size=18, color=MUTED)
    d.box(48, 1535, 1344, 345)
    d.text(70, 1576, '5  microSD over SPI2 — 3.3 V logic and card supply', size=25, weight=700)
    for y, left, right, reverse in [
        (1620, 'GPIO23 / P2.5', 'SCK / CLK (card pin 5)', False),
        (1660, 'GPIO24 / P2.4', 'MOSI / DI / CMD (card pin 3)', False),
        (1700, 'GPIO2 / P1.3', 'MISO / DO / DAT0 (card pin 7)', True),
        (1740, 'GPIO25 / P1.13', 'CS / DAT3 (card pin 2)', False),
    ]:
        d.text(70, y + 6, left, size=19, weight=700)
        d.text(930, y + 6, right, size=19)
        points = [(370, y), (900, y)]
        d.line(list(reversed(points)) if reverse else points, BLUE, arrow=True)
    d.text(70, 1780, '3V3 to card VDD (pin 4); GND to VSS (pin 6). Add 100 nF + 10 µF near the socket.', size=18)
    d.text(70, 1810, '10 kΩ pull-ups to 3.3 V: CS, CMD, DAT0, DAT1 (pin 8), DAT2 (pin 1); no pull-up on CLK.', size=18)
    d.text(70, 1840, 'Use a native 3.3 V SPI breakout. HW-125 supply / regulator / level-shifter variant must be checked.', size=18, color=MUTED)
    d.text(70, 1868, 'Pull-ups may already be fitted. Card contact numbers are not the adapter header order.', size=17, color=MUTED)
    d.note(1905, 'Power, parts and configuration', [
        'Parent via documented USB input. External regulated +5 V feeds LCD and buzzer; join its GND to parent GND.',
        'Leave parent P1.14 / 5V unconnected with this external load supply. Keep +5 V off ESP32 GPIOs.',
        'Add 100 nF across VCC/GND at GPS and LCD, plus 10 µF across the local +5 V/GND load rail.',
        'Use efficient red/green/amber LEDs (~2–3 mA with 470 Ω). Verify parent 3V3 regulator headroom for GPS.',
        'Example config enables all five peripherals and remaps parent bus RX to GPIO3. Default config stays unchanged.',
        'Avoid GPIO6 (battery ADC), GPIO15 (PSRAM), GPIO27 (RGB), GPIO28 (BOOT), GPIO12 (USB-UART TX).',
    ], ORANGE)
    d.save('parent-peripherals', png)


def parent_headers(png):
    d = Diagram('N32R8 parent header connection map',
                'Component side up • antenna at top • USB connectors at bottom • P1/P2 numbers from board schematic.', 1250)
    left = [('3V3', 'GPS + shifter LV + native SD supply'), ('RST', 'Leave available'),
            ('GPIO2', 'SD MISO / DO'), ('GPIO3', 'Parent RS-485 RO / RX'),
            ('GPIO0', 'Green LED through 470 Ω'), ('GPIO1', 'Red LED through 470 Ω'),
            ('GPIO6', 'Reserved: battery ADC'), ('GPIO7', 'RS-485 DE + /RE'),
            ('GPIO8', 'Amber LED through 470 Ω'), ('GPIO9', 'Buzzer base through 1 kΩ'),
            ('GPIO10', 'GPS TX to parent RX'), ('GPIO26', 'Leave boot strap free'),
            ('GPIO25', 'SD CS'), ('5V', 'Board rail; power plan below'),
            ('GND', 'Common ground'), ('VBAT', 'Battery rail; not a 5 V output')]
    right = [('GND', 'Common ground'), ('TX / GPIO11', 'RS-485 DI / parent TX'),
             ('RX / GPIO12', 'Do not use: USB-UART TX attached'), ('GPIO24', 'SD MOSI / DI'),
             ('GPIO23', 'SD SCK / CLK'), ('GPIO15', 'Reserved: N32R8 PSRAM'),
             ('GPIO27', 'Reserved: onboard RGB'), ('GPIO4', 'LCD SDA through level shifter'),
             ('GPIO5', 'LCD SCL through level shifter'), ('NC', 'No connection'),
             ('GPIO28', 'Reserved: BOOT button / strap'), ('GND', 'Common ground'),
             ('GPIO14', 'Reserved: native USB D+'), ('GPIO13', 'Reserved: native USB D−'),
             ('GND', 'Common ground'), ('VBAT', 'Battery rail; not a 5 V output')]
    d.box(560, 200, 320, 795, fill='#edf3f8')
    d.text(720, 238, 'ESP32-C5', size=29, weight=700, anchor='middle')
    d.text(720, 270, 'WiFi6 N32R8', size=24, weight=700, anchor='middle')
    d.text(720, 306, 'ANTENNA / TOP', size=18, anchor='middle', color=MUTED)
    d.text(650, 955, 'USB', size=22, anchor='middle', weight=700)
    d.text(790, 955, 'UART', size=22, anchor='middle', weight=700)
    d.text(720, 980, 'Use USB for console', size=17, anchor='middle', color=BLUE)
    for side, entries in [('P1', left), ('P2', right)]:
        for index, (pin, use) in enumerate(entries, 1):
            y = 345 + (index - 1) * 38
            color = MUTED if any(x in use.lower() for x in ('reserved', 'leave', 'do not', 'no connection', 'battery rail')) else BLUE
            if side == 'P1':
                d.dot(560, y, color)
                d.text(575, y + 6, f'{index}  {pin}', size=17, color=color)
                d.line([(530, y), (560, y)], color, width=2)
                d.text(515, y + 6, use, size=17, anchor='end', color=color)
            else:
                d.dot(880, y, color)
                d.text(865, y + 6, f'{pin}  {index}', size=17, anchor='end', color=color)
                d.line([(880, y), (910, y)], color, width=2)
                d.text(925, y + 6, use, size=17, color=color)
    d.text(530, 307, 'P1 / LEFT', size=21, anchor='end', weight=700)
    d.text(910, 307, 'P2 / RIGHT', size=21, weight=700)
    d.note(1020, 'Board-specific details', [
        'Native USB port is left in the supplied pinout. GPIO12 is tied to CH343 TX; parent bus RX uses GPIO3.',
        'Use USB power for parent, external regulated +5 V for LCD/buzzer, common ground; do not tie +5 V rails.',
        'GPIO25 pull-up selects an SDIO clock strap; firmware uses SPI boot and SPI2 SD, not the SDIO peripheral.',
        'GPIO0/1 need optional 32 kHz crystal unpopulated. Compare silkscreen before using a different board revision.',
    ], ORANGE)
    d.save('parent-headers', png, 'Physical header map for supplied N32R8 board pinout • compare board revision • see docs/wiring.md')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--png', action='store_true', help='Also render PNGs using resvg-py')
    parser.add_argument('--font', action='append', default=[], help='Optional TTF/OTF font file (repeatable)')
    args = parser.parse_args()
    global FONT_FILES
    FONT_FILES = args.font
    bus = json.loads((ROOT / 'config/settings.json').read_text())['bus']
    if (bus['tx_pin'], bus['rx_pin'], bus['rts_pin'], bus['uart'], bus['baud'], bus['enabled']) != (11, 12, 7, 1, 115200, True):
        parser.error('Bus settings changed; update diagram labels/mappings before regenerating.')
    parent = json.loads((ROOT / 'config/settings.parent-wiring.example.json').read_text())
    expected = {'gps': {'enabled': True, 'uart': 0, 'rx_pin': 10, 'tx_pin': -1, 'baud': 9600},
                'display': {'enabled': True, 'sda_pin': 4, 'scl_pin': 5, 'i2c_address': 39},
                'buzzer': {'enabled': True, 'pin': 9},
                'signal_trend': {'enabled': True, 'increasing_pin': 0, 'decreasing_pin': 1, 'steady_pin': 8},
                'sd': {'enabled': True, 'sck_pin': 23, 'mosi_pin': 24, 'miso_pin': 2, 'cs_pin': 25},
                'bus': {'enabled': True, 'uart': 1, 'tx_pin': 11, 'rx_pin': 3, 'rts_pin': 7}}
    for section, values in expected.items():
        if any(parent[section][key] != value for key, value in values.items()):
            parser.error(f'Parent example {section} changed; update the peripheral diagram before regenerating.')
    standalone = json.loads((ROOT / 'config/settings.standalone-wiring.example.json').read_text())
    expected_standalone = {
        'node': {'role': 'parent', 'id': 0}, 'standalone': {'enabled': True},
        'bus': {'enabled': False},
        'gps': {'enabled': True, 'uart': 0, 'rx_pin': 12, 'tx_pin': -1, 'baud': 9600},
        'sd': {'enabled': True, 'sck_pin': 8, 'mosi_pin': 10, 'miso_pin': 9, 'cs_pin': 11, 'clock_khz': 4000},
        'display': {'enabled': True, 'sda_pin': 23, 'scl_pin': 24, 'i2c_address': 39},
        'buzzer': {'enabled': True, 'pin': 7},
        'signal_trend': {'enabled': True, 'increasing_pin': 1, 'decreasing_pin': 0, 'steady_pin': 25},
    }
    for section, values in expected_standalone.items():
        if any(standalone[section][key] != value for key, value in values.items()):
            parser.error(f'Standalone example {section} changed; update the standalone diagram before regenerating.')
    OUT.mkdir(parents=True, exist_ok=True)
    node_wiring(args.png)
    bus_wiring(args.png)
    power_wiring(args.png)
    parent_wiring(args.png)
    parent_headers(args.png)
    standalone_wiring(args.png)


if __name__ == '__main__':
    main()
