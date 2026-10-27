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

    def save(self, name, png):
        self.text(48, self.height - 24, 'Logical wiring • connectors not drawn to physical pin order • see docs/wiring.md', size=15, color=MUTED)
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
        d.text(x, 280, 'TX 11 · RX 12 · RTS 7*' if index == 0 else 'D6 TX · D7 RX · D3 RTS', size=15, anchor='middle', color=MUTED)
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
    d.note(853, 'Parent header layout is pending', [
        '*Parent uses GPIO11 / 12 / 7 in config; verify those pins on its specific development-board headers.',
    ], ORANGE)
    d.save('rs485-network', png)


def power_wiring(png):
    d = Diagram('Power distribution', 'XIAO boards receive regulated 5 V in parallel. Parent input wiring remains provisional until its board layout is confirmed.', 1040)
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
    d.note(730, 'Full-size ESP32-C5-WROOM-1 parent: power connector / header positions TBD', [
        'Use the parent board’s documented USB power input for now. No parent VIN / 5V pin is assigned here.',
        'For this non-isolated RS-485 bus, connect parent GND to the same common ground.',
    ], ORANGE)
    d.note(868, 'Keep the voltage rails separate', [
        'Do not feed external power into XIAO 3V3_OUT. Do not connect different boards’ 3V3 outputs together.',
        'Do not attach the LCD backpack or HW-125 to a rail until its actual voltage requirements are confirmed.',
    ], RED)
    d.save('power-distribution', png)


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
    OUT.mkdir(parents=True, exist_ok=True)
    node_wiring(args.png)
    bus_wiring(args.png)
    power_wiring(args.png)


if __name__ == '__main__':
    main()
