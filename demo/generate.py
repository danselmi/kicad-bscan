#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Generates the kicad-bscan demo project from one description of the board.

    python3 demo/generate.py

Writes (all generated, do not edit by hand):

    demo/bscan-demo/bscan-demo.kicad_pcb     the board (KiCad 9 format)
    demo/bscan-demo/bscan-demo.kicad_pro     the KiCad project
    demo/bscan-demo/bscan-demo.bscan.json    the plugin's settings for it
    demo/bscan-demo/bsdl/*.bsd               BSDL files of the two chips
    demo/sim/board.json                      chips and nets for demo_sim.py

The two chips are fictional ("BSDEMO_M32", a small microcontroller, and
"BSDEMO_F48", a small FPGA); demo_sim.py simulates them on the JTAG chain
and the board's nets, so the plugin can be tried without hardware.
"""

import json
import os
import uuid
from itertools import pairwise

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT = os.path.join(HERE, "bscan-demo")
NAME = "bscan-demo"

# =============================================================================
# The chips
# =============================================================================
#
# Port kinds: in (input cell), out2 (2-state output), out3 (3-state output,
# own control cell unless shared), bidir (BC_7 with own control cell unless
# shared), tap (TCK/TMS/TDI/TDO), linkage (power).
#
# core: what the chip's own logic does with the pin while it is not in
# EXTEST (for the simulator): None (output off), ["drive", v],
# ["toggle", period_s], ["counter", bit, period_s] (bit of a counter that
# counts every period_s).


class Chip:
    def __init__(self, ref, entity, package, tap, irlen, idcode, opcodes, pins, shared=()):
        self.ref = ref
        self.entity = entity
        self.package = package
        self.tap = tap
        self.irlen = irlen
        self.idcode = idcode
        self.opcodes = opcodes  # name -> bit string
        self.pins = pins  # pin number -> (port, kind, core)
        self.shared = shared  # tuples of ports sharing one control cell
        self.cells = self._cells()

    def _cells(self):
        """Boundary register: list of dicts, index = cell number."""
        cells = []
        control_of = {}  # port -> control cell number

        def control_for(port):
            for group in self.shared:
                if port in group:
                    for other in group:
                        if other in control_of:
                            return control_of[other]
            n = len(cells)
            cells.append({"num": n, "type": "BC_1", "port": "*", "func": "control", "safe": "0"})
            return n

        for pin in sorted(self.pins):
            port, kind, _ = self.pins[pin]

            if kind == "in":
                cells.append(
                    {"num": len(cells), "type": "BC_1", "port": port, "func": "input", "safe": "X"}
                )
            elif kind == "out2":
                cells.append(
                    {
                        "num": len(cells),
                        "type": "BC_1",
                        "port": port,
                        "func": "output2",
                        "safe": "1",
                    }
                )
            elif kind in ("out3", "bidir"):
                ctrl = control_for(port)
                control_of[port] = ctrl
                cells.append(
                    {
                        "num": len(cells),
                        "type": "BC_7" if kind == "bidir" else "BC_1",
                        "port": port,
                        "func": "bidir" if kind == "bidir" else "output3",
                        "safe": "X",
                        "ctrl": ctrl,
                        "dis": 0,
                    }
                )
        return cells

    def ports(self):
        """Port name -> list of pins (several for linkage ports)."""
        out = {}
        for pin in sorted(self.pins):
            port = self.pins[pin][0]
            out.setdefault(port, []).append(pin)
        return out


def idcode(version, part, manufacturer):
    return (version << 28) | (part << 12) | (manufacturer << 1) | 1


IO, IN, O2, O3 = "bidir", "in", "out2", "out3"

# --- U1: microcontroller, QFN-32 (pins counter-clockwise from the top left) ---
MCU = Chip(
    "U1",
    "BSDEMO_M32",
    "QFN32",
    "mcu.tap",
    4,
    idcode(1, 0xD3C1, 0x077),
    {"EXTEST": "0000", "SAMPLE": "0010", "PRELOAD": "0010", "IDCODE": "1110", "BYPASS": "1111"},
    {
        # left side, top to bottom: to the connector J1
        1: ("VDD", "linkage", None),
        2: ("NRST", IN, None),
        3: ("PD0", IO, None),
        4: ("PD1", IO, None),
        5: ("PD2", IO, None),
        6: ("PC3", IN, None),
        7: ("PC6", IO, ["toggle", 1.0]),  # status LED blinks
        8: ("VSS", "linkage", None),
        # bottom side, left to right: control lines to the FPGA
        9: ("TCK", "tap", None),
        10: ("TMS", "tap", None),
        11: ("PB4", O3, ["drive", 1]),  # nPROG: keep the FPGA configured
        12: ("PB3", O2, ["toggle", 0.5]),  # CLK
        13: ("PB2", O3, ["drive", 1]),  # nWR
        14: ("PB1", O3, ["drive", 1]),  # nRD
        15: ("PB0", O3, ["drive", 1]),  # nCS
        16: ("VDD", "linkage", None),
        # right side, bottom to top: data bus D7 .. D0
        17: ("PA7", IO, ["counter", 7, 0.25]),
        18: ("PA6", IO, ["counter", 6, 0.25]),
        19: ("PA5", IO, ["counter", 5, 0.25]),
        20: ("PA4", IO, ["counter", 4, 0.25]),
        21: ("PA3", IO, ["counter", 3, 0.25]),
        22: ("PA2", IO, ["counter", 2, 0.25]),
        23: ("PA1", IO, ["counter", 1, 0.25]),
        24: ("PA0", IO, ["counter", 0, 0.25]),
        # top side, right to left: IRQ, timer input, I2C
        25: ("PC0", IN, None),
        26: ("PC1", IN, None),
        27: ("PC4", IO, None),
        28: ("PC5", IO, None),
        29: ("TDI", "tap", None),
        30: ("TDO", "tap", None),
        31: ("VSS", "linkage", None),
        32: ("VDD", "linkage", None),
    },
)

# --- U2: FPGA, TQFP-48 ---
FPGA = Chip(
    "U2",
    "BSDEMO_F48",
    "TQFP48",
    "fpga.tap",
    6,
    idcode(0, 0x3A42, 0x077),
    {
        "EXTEST": "100110",
        "SAMPLE": "000001",
        "PRELOAD": "000001",
        "IDCODE": "001001",
        "HIGHZ": "001010",
        "CLAMP": "001111",
        "BYPASS": "111111",
    },
    {
        # left side, top to bottom: data bus D0 .. D7
        1: ("VCCO", "linkage", None),
        2: ("GND", "linkage", None),
        3: ("IO_L1", IO, None),
        4: ("IO_L2", IO, None),
        5: ("IO_L3", IO, None),
        6: ("IO_L4", IO, None),
        7: ("IO_L5", IO, None),
        8: ("IO_L6", IO, None),
        9: ("IO_L7", IO, None),
        10: ("IO_L8", IO, None),
        11: ("GND", "linkage", None),
        12: ("VCCO", "linkage", None),
        # bottom side, left to right
        13: ("IO_B1", IO, None),  # nCS
        14: ("IO_B2", IO, None),  # nRD
        15: ("IO_B3", IO, None),  # nWR
        16: ("GCLK", IN, None),  # CLK
        17: ("PROG_B", IN, None),  # nPROG
        18: ("VCCINT", "linkage", None),
        19: ("GND", "linkage", None),
        20: ("TCK", "tap", None),
        21: ("TMS", "tap", None),
        22: ("TDI", "tap", None),
        23: ("TDO", "tap", None),
        24: ("GND", "linkage", None),
        # right side, bottom to top: LEDs and the SPI connector J2
        25: ("VCCO", "linkage", None),
        26: ("IO_R1", IO, ["counter", 3, 0.5]),  # LED4
        27: ("IO_R2", IO, ["counter", 2, 0.5]),  # LED3
        28: ("IO_R3", IO, ["counter", 1, 0.5]),  # LED2
        29: ("IO_R4", IO, ["counter", 0, 0.5]),  # LED1
        30: ("IO_R5", IO, ["toggle", 0.5]),  # PWM1 (shares its enable with PWM0)
        31: ("IO_R6", IO, None),  # SPI_SCK  } one output enable
        32: ("IO_R7", IO, None),  # SPI_MOSI}
        33: ("IO_R8", IN, None),  # SPI_MISO
        34: ("IO_R9", IO, None),  # AUX0
        35: ("IO_R10", IO, None),
        36: ("GND", "linkage", None),
        # top side, right to left
        37: ("VCCO", "linkage", None),
        38: ("GND", "linkage", None),
        39: ("IO_T1", IO, None),
        40: ("IO_T2", IO, None),
        41: ("IO_T3", IO, None),
        42: ("IO_T4", IO, None),
        43: ("IO_T5", IO, None),
        44: ("IO_T6", IO, None),
        45: ("IO_T7", IO, None),  # SCL
        46: ("IO_T8", IO, None),  # SDA
        47: ("IO_T9", IO, ["toggle", 0.25]),  # PWM0
        48: ("IO_T10", IO, ["drive", 1]),  # IRQ (inactive high)
    },
    shared=(("IO_T9", "IO_R5"), ("IO_R6", "IO_R7")),
)

CHIPS = [MCU, FPGA]

# Chain seen from TDO (= order of OpenOCD's 'jtag newtap'): TDI -> U1 -> U2 -> TDO
CHAIN = [FPGA, MCU]

# =============================================================================
# The board: nets as (reference, pad) lists
# =============================================================================

NETS = {}


def net(name, *pads):
    NETS.setdefault(name, []).extend(pads)


for i in range(8):
    net(f"D{i}", ("U1", 24 - i), ("U2", 3 + i))

net("nCS", ("U1", 15), ("U2", 13))
net("nRD", ("U1", 14), ("U2", 14))
net("nWR", ("U1", 13), ("U2", 15))
net("CLK", ("U1", 12), ("U2", 16))
net("nPROG", ("U1", 11), ("U2", 17))
net("IRQ", ("U1", 25), ("U2", 48))
net("PWM0", ("U1", 26), ("U2", 47))
net("SDA", ("U1", 27), ("U2", 46), ("R7", 1))
net("SCL", ("U1", 28), ("U2", 45), ("R8", 1))

# J1: reset, GPIOs, button and status LED
for pin, name, header in (
    (2, "nRESET", 1),
    (3, "GPIO0", 2),
    (4, "GPIO1", 3),
    (5, "GPIO2", 4),
    (6, "BTN", 5),
    (7, "STAT", 6),
):
    net(name, ("U1", pin), ("J1", header))

# J2: SPI and spare signals of the FPGA
for pin, name, header in (
    (34, "AUX0", 1),
    (33, "SPI_MISO", 2),
    (32, "SPI_MOSI", 3),
    (31, "SPI_SCK", 4),
    (30, "PWM1", 5),
):
    net(name, ("U2", pin), ("J2", header))

# LEDs: FPGA pin - resistor - LED
for i, pin in enumerate((29, 28, 27, 26)):
    net(f"LED{i + 1}", ("U2", pin), (f"R{i + 1}", 1))
    net(f"LED{i + 1}_A", (f"R{i + 1}", 2), (f"D{i + 1}", 2))
    net("GND", (f"D{i + 1}", 1))

# pull-ups of the I2C lines
net("+3V3", ("R7", 2), ("R8", 2))

# What drives nets from outside the chips (for the simulator), like 'core':
# ["pulse", period_s, width_s, value] drives value for width_s every period_s.
EXTERNAL = {
    "BTN": ["pulse", 4.0, 0.5, 0],  # the button on J1 is pressed now and then
}

# =============================================================================
# BSDL
# =============================================================================


def bsdl(chip):
    ports = chip.ports()
    lines = []
    w = lines.append

    w(
        f"-- {chip.entity}: fictional {'microcontroller' if chip is MCU else 'FPGA'} of the "
        "kicad-bscan demo board."
    )
    w("-- Generated by demo/generate.py; simulated by demo/demo_sim.py.")
    w("")
    w(f"entity {chip.entity} is")
    w("")
    w(f'  generic (PHYSICAL_PIN_MAP : string := "{chip.package}");')
    w("")
    w("  port (")

    decls = []
    for port, pins in ports.items():
        kind = chip.pins[pins[0]][1]
        mode = {
            "in": "in",
            "out2": "out",
            "out3": "out",
            "bidir": "inout",
            "linkage": "linkage",
        }.get(kind)
        if kind == "tap":
            mode = "out" if port == "TDO" else "in"
        if len(pins) > 1:
            decls.append(f"    {port:<7}: {mode:<7} bit_vector (0 to {len(pins) - 1})")
        else:
            decls.append(f"    {port:<7}: {mode:<7} bit")
    w(";\n".join(decls))
    w("  );")
    w("")
    w("  use STD_1149_1_2001.all;")
    w("")
    w(f'  attribute COMPONENT_CONFORMANCE of {chip.entity} : entity is "STD_1149_1_2001";')
    w("")
    w(f"  attribute PIN_MAP of {chip.entity} : entity is PHYSICAL_PIN_MAP;")
    w("")
    w(f"  constant {chip.package} : PIN_MAP_STRING :=")
    entries = []
    for port, pins in ports.items():
        pinlist = ", ".join(str(p) for p in pins)
        entries.append(f"{port} : ({pinlist})" if len(pins) > 1 else f"{port} : {pinlist}")
    for i, e in enumerate(entries):
        sep = "," if i + 1 < len(entries) else ""
        end = " &" if i + 1 < len(entries) else ";"
        w(f'    "{e}{sep}"{end}')
    w("")
    w("  attribute TAP_SCAN_IN    of TDI : signal is true;")
    w("  attribute TAP_SCAN_MODE  of TMS : signal is true;")
    w("  attribute TAP_SCAN_OUT   of TDO : signal is true;")
    w("  attribute TAP_SCAN_CLOCK of TCK : signal is (10.0e6, BOTH);")
    w("")
    w(f"  attribute INSTRUCTION_LENGTH of {chip.entity} : entity is {chip.irlen};")
    w("")
    w(f"  attribute INSTRUCTION_OPCODE of {chip.entity} : entity is")
    ops = list(chip.opcodes.items())
    for i, (name, bits) in enumerate(ops):
        sep = "," if i + 1 < len(ops) else ""
        end = " &" if i + 1 < len(ops) else ";"
        w(f'    "{name:<8} ({bits}){sep}"{end}')
    w("")
    w(f'  attribute INSTRUCTION_CAPTURE of {chip.entity} : entity is "{"0" * (chip.irlen - 2)}01";')
    w("")
    bits = f"{chip.idcode:032b}"
    w(f"  attribute IDCODE_REGISTER of {chip.entity} : entity is")
    w(f'    "{bits[0:4]}"             &  -- version')
    w(f'    "{bits[4:20]}" &  -- part number')
    w(f'    "{bits[20:31]}"      &  -- manufacturer')
    w('    "1";')
    w("")
    others = [
        n for n in chip.opcodes if n not in ("EXTEST", "SAMPLE", "PRELOAD", "IDCODE", "BYPASS")
    ]
    w(f"  attribute REGISTER_ACCESS of {chip.entity} : entity is")
    w('    "BOUNDARY (EXTEST, SAMPLE, PRELOAD),"  &')
    w('    "DEVICE_ID (IDCODE),"  &')
    w(f'    "BYPASS (BYPASS{"".join(", " + n for n in others)})";')
    w("")
    w(f"  attribute BOUNDARY_LENGTH of {chip.entity} : entity is {len(chip.cells)};")
    w("")
    w(f"  attribute BOUNDARY_REGISTER of {chip.entity} : entity is")
    w("  --  num  cell   port     function  safe ccell disval rslt")
    for i, c in enumerate(chip.cells):
        body = f"{c['num']:>3}  ({c['type']}, {c['port']:<7}, {c['func']:<8}, {c['safe']}"
        if "ctrl" in c:
            body += f",   {c['ctrl']:>2},    {c['dis']},     Z"
        body += ")"
        sep = "," if i + 1 < len(chip.cells) else ""
        end = " &" if i + 1 < len(chip.cells) else ";"
        w(f'    "{body}{sep}"{end}')
    w("")
    w(f"end {chip.entity};")
    return "\n".join(lines) + "\n"


# =============================================================================
# Simulator description
# =============================================================================


def sim_json():
    chips = []
    for chip in CHAIN:
        ports = {}
        for pin, (port, kind, core) in chip.pins.items():
            if kind in ("in", "out2", "out3", "bidir"):
                ports[port] = {"pin": pin, "kind": kind, "core": core}
        chips.append(
            {
                "ref": chip.ref,
                "tap": chip.tap,
                "irlen": chip.irlen,
                "idcode": chip.idcode,
                "opcodes": {bits: name for name, bits in chip.opcodes.items() if name != "PRELOAD"},
                "cells": chip.cells,
                "ports": ports,
            }
        )

    bs_refs = {c.ref for c in CHIPS}
    nets = {name: [f"{r}.{p}" for r, p in pads if r in bs_refs] for name, pads in NETS.items()}
    return {
        "chain": chips,
        "nets": {n: p for n, p in nets.items() if p},
        "external": EXTERNAL,
    }


# =============================================================================
# PCB
# =============================================================================

OX, OY = 108.0, 108.0  # board origin (top left)
BW, BH = 68.0, 34.0
MCU_AT = (128.0, 125.0)
FPGA_AT = (150.0, 125.0)

TRACK = 0.2
VIA_SIZE, VIA_DRILL = 0.6, 0.3


def uid(*parts):
    return str(uuid.uuid5(uuid.NAMESPACE_URL, "kicad-bscan-demo/" + "/".join(map(str, parts))))


def fmt(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s == "-0" else s


class Footprint:
    def __init__(self, ref, name, value, at, pads, body, kind="smd"):
        self.ref, self.name, self.value, self.at = ref, name, value, at
        self.pads = pads  # list of (number, x, y, w, h, shape, drill)
        self.body = body  # (w, h) of the silkscreen outline
        self.kind = kind

    def pad_pos(self, number):
        for p in self.pads:
            if p[0] == number:
                return (self.at[0] + p[1], self.at[1] + p[2])
        raise KeyError((self.ref, number))


def qfn(n_side, pitch, center, pad_len, pad_w):
    """Pads of a quad package, numbered counter-clockwise from the top left."""
    pads = []
    span = (n_side - 1) * pitch / 2
    num = 1
    for side in range(4):
        for i in range(n_side):
            off = -span + i * pitch
            if side == 0:
                pads.append((num, -center, off, pad_len, pad_w))
            elif side == 1:
                pads.append((num, off, center, pad_w, pad_len))
            elif side == 2:
                pads.append((num, center, -off, pad_len, pad_w))
            else:
                pads.append((num, -off, -center, pad_w, pad_len))
            num += 1
    return [(n, x, y, w, h, "roundrect", None) for n, x, y, w, h in pads]


def two_pad(vertical=False):
    if vertical:
        return [
            (1, 0, 0.8, 0.95, 0.8, "roundrect", None),
            (2, 0, -0.8, 0.95, 0.8, "roundrect", None),
        ]
    return [(1, -0.8, 0, 0.8, 0.95, "roundrect", None), (2, 0.8, 0, 0.8, 0.95, "roundrect", None)]


def led_pads():
    """0603 LED turned by 180 degrees: anode (2) left, cathode (1) right"""
    return [(1, 0.8, 0, 0.8, 0.95, "roundrect", None), (2, -0.8, 0, 0.8, 0.95, "roundrect", None)]


def header(n):
    return [(i + 1, 0, i * 2.54, 1.7, 1.7, "rect" if i == 0 else "oval", 1.0) for i in range(n)]


FOOTPRINTS = []
SEGMENTS = []  # (x1, y1, x2, y2, layer, net)
VIAS = []  # (x, y, net)


def add_fp(*args, **kw):
    fp = Footprint(*args, **kw)
    FOOTPRINTS.append(fp)
    return fp


U1 = add_fp(
    "U1",
    "kicad-bscan-demo:QFN-32_5x5mm_P0.5mm",
    MCU.entity,
    MCU_AT,
    qfn(8, 0.5, 2.45, 0.8, 0.25),
    (5.0, 5.0),
)
U2 = add_fp(
    "U2",
    "kicad-bscan-demo:TQFP-48_7x7mm_P0.5mm",
    FPGA.entity,
    FPGA_AT,
    qfn(12, 0.5, 4.2, 1.5, 0.3),
    (7.0, 7.0),
)

J1_AT = (MCU_AT[0] - 13.0, MCU_AT[1] - 2.5 * 2.54)
J2_AT = (FPGA_AT[0] + 17.0, FPGA_AT[1] - 10.0)
J1 = add_fp(
    "J1",
    "kicad-bscan-demo:PinHeader_1x06_P2.54mm",
    "EXT",
    J1_AT,
    header(6),
    (2.54, 6 * 2.54),
    kind="tht",
)
J2 = add_fp(
    "J2",
    "kicad-bscan-demo:PinHeader_1x05_P2.54mm",
    "SPI",
    J2_AT,
    header(5),
    (2.54, 5 * 2.54),
    kind="tht",
)

LED_ROWS = [3.0, 5.5, 8.0, 10.5]  # room for the references between the rows
for i, dy in enumerate(LED_ROWS):
    add_fp(
        f"R{i + 1}",
        "kicad-bscan-demo:R_0603_1608Metric",
        "330",
        (FPGA_AT[0] + 9.0, FPGA_AT[1] + dy),
        two_pad(),
        (1.6, 0.8),
    )
    add_fp(
        f"D{i + 1}",
        "kicad-bscan-demo:LED_0603_1608Metric",
        "LED",
        (FPGA_AT[0] + 12.0, FPGA_AT[1] + dy),
        led_pads(),
        (1.6, 0.8),
    )

LANE0, LANE_STEP = 6.0, 0.6
SCL_LANE_Y = MCU_AT[1] - LANE0 - 3 * LANE_STEP
R7 = add_fp(
    "R7",
    "kicad-bscan-demo:R_0603_1608Metric",
    "4k7",
    (MCU_AT[0] + 5.0, SCL_LANE_Y - 3.0),
    two_pad(vertical=True),
    (0.8, 1.6),
)
R8 = add_fp(
    "R8",
    "kicad-bscan-demo:R_0603_1608Metric",
    "4k7",
    (MCU_AT[0] + 8.0, SCL_LANE_Y - 3.0),
    two_pad(vertical=True),
    (0.8, 1.6),
)

FP_BY_REF = {fp.ref: fp for fp in FOOTPRINTS}


def pos(ref, pad):
    return FP_BY_REF[ref].pad_pos(pad)


def net_of(ref, pad):
    for name, pads in NETS.items():
        if (ref, pad) in pads:
            return name
    return None


def track(points, netname, layer="F.Cu"):
    for (x1, y1), (x2, y2) in pairwise(points):
        if abs(x1 - x2) > 1e-6 or abs(y1 - y2) > 1e-6:
            SEGMENTS.append((x1, y1, x2, y2, layer, netname))


# --- data bus: straight ---
for i in range(8):
    track([pos("U1", 24 - i), pos("U2", 3 + i)], f"D{i}")

# --- control lines below, nested lanes (closest pins on the innermost lane) ---
for k, (name, mpin, fpin) in enumerate(
    [("nCS", 15, 13), ("nRD", 14, 14), ("nWR", 13, 15), ("CLK", 12, 16), ("nPROG", 11, 17)]
):
    y = MCU_AT[1] + LANE0 + k * LANE_STEP
    (mx, my), (fx, fy) = pos("U1", mpin), pos("U2", fpin)
    track([(mx, my), (mx, y), (fx, y), (fx, fy)], name)

# --- IRQ, timer and I2C above; pull-ups for SDA / SCL ---
for k, (name, mpin, fpin) in enumerate(
    [("IRQ", 25, 48), ("PWM0", 26, 47), ("SDA", 27, 46), ("SCL", 28, 45)]
):
    y = MCU_AT[1] - LANE0 - k * LANE_STEP
    (mx, my), (fx, fy) = pos("U1", mpin), pos("U2", fpin)
    tap_x = {"SDA": R7.at[0], "SCL": R8.at[0]}.get(name)
    if tap_x is None:
        track([(mx, my), (mx, y), (fx, y), (fx, fy)], name)
    else:
        track([(mx, my), (mx, y), (tap_x, y), (fx, y), (fx, fy)], name)

# SCL: straight up to its pull-up (outermost lane)
rx, ry = pos("R8", 1)
track([(rx, SCL_LANE_Y), (rx, ry)], "SCL")
# SDA: under the SCL lane on the back side
rx, ry = pos("R7", 1)
sda_y = SCL_LANE_Y + LANE_STEP
via_top = SCL_LANE_Y - 1.0
VIAS.append((rx, sda_y, "SDA"))
VIAS.append((rx, via_top, "SDA"))
track([(rx, sda_y), (rx, via_top)], "SDA", "B.Cu")
track([(rx, via_top), (rx, ry)], "SDA")


def fan(pins_targets, x0, step):
    """Nested L-shaped fan-out from one side of a chip: pad -> horizontal to
    its own x -> vertical to the target's y -> target pad. Of the pins going
    up the topmost turns first (at x0), of those going down the bottommost;
    step < 0 for the left side."""
    up = sorted([p for p in pins_targets if p[2][1] < p[1][1]], key=lambda p: p[1][1])
    down = sorted([p for p in pins_targets if p[2][1] >= p[1][1]], key=lambda p: -p[1][1])
    for group in (up, down):
        for k, (name, (px, py), (tx, ty)) in enumerate(group):
            x = x0 + k * step
            track([(px, py), (x, py), (x, ty), (tx, ty)], name)


# --- MCU left side to J1 ---
fan(
    [
        (net_of("U1", p), pos("U1", p), pos("J1", h))
        for p, h in ((2, 1), (3, 2), (4, 3), (5, 4), (6, 5), (7, 6))
    ],
    MCU_AT[0] - 4.5,
    -0.6,
)

# --- FPGA right side to J2 and the LED resistors ---
fan(
    [
        (net_of("U2", p), pos("U2", p), pos("J2", h))
        for p, h in ((34, 1), (33, 2), (32, 3), (31, 4), (30, 5))
    ]
    + [(f"LED{i + 1}", pos("U2", p), pos(f"R{i + 1}", 1)) for i, p in enumerate((29, 28, 27, 26))],
    FPGA_AT[0] + 6.0,
    0.6,
)

for i in range(4):
    track([pos(f"R{i + 1}", 2), pos(f"D{i + 1}", 2)], f"LED{i + 1}_A")

# LED cathodes to GND, pull-ups to +3V3 (not boundary-scan nets)
track([pos(f"D{i + 1}", 1) for i in range(4)], "GND")
track([pos("R7", 2), pos("R8", 2)], "+3V3")

# --- writing the board file ---

NET_NAMES = [""] + sorted(NETS)
NET_NUM = {n: i for i, n in enumerate(NET_NAMES)}

LAYERS = """\t(layers
\t\t(0 "F.Cu" signal "top_layer")
\t\t(2 "B.Cu" signal "bottom_layer")
\t\t(9 "F.Adhes" user "F.Adhesive")
\t\t(11 "B.Adhes" user "B.Adhesive")
\t\t(13 "F.Paste" user)
\t\t(15 "B.Paste" user)
\t\t(5 "F.SilkS" user "F.Silkscreen")
\t\t(7 "B.SilkS" user "B.Silkscreen")
\t\t(1 "F.Mask" user)
\t\t(3 "B.Mask" user)
\t\t(17 "Dwgs.User" user "User.Drawings")
\t\t(19 "Cmts.User" user "User.Comments")
\t\t(21 "Eco1.User" user "User.Eco1")
\t\t(23 "Eco2.User" user "User.Eco2")
\t\t(25 "Edge.Cuts" user)
\t\t(27 "Margin" user)
\t\t(31 "F.CrtYd" user "F.Courtyard")
\t\t(29 "B.CrtYd" user "B.Courtyard")
\t\t(35 "F.Fab" user)
\t\t(33 "B.Fab" user)
\t)
"""


SETUP = """\t(setup
\t\t(stackup
\t\t\t(layer "F.SilkS"
\t\t\t\t(type "Top Silk Screen")
\t\t\t\t(color "White")
\t\t\t)
\t\t\t(layer "F.Paste"
\t\t\t\t(type "Top Solder Paste")
\t\t\t)
\t\t\t(layer "F.Mask"
\t\t\t\t(type "Top Solder Mask")
\t\t\t\t(color "Green")
\t\t\t\t(thickness 0.01)
\t\t\t)
\t\t\t(layer "F.Cu"
\t\t\t\t(type "copper")
\t\t\t\t(thickness 0.035)
\t\t\t)
\t\t\t(layer "dielectric 1"
\t\t\t\t(type "core")
\t\t\t\t(thickness 1.51)
\t\t\t\t(material "FR4")
\t\t\t\t(epsilon_r 4.5)
\t\t\t\t(loss_tangent 0.02)
\t\t\t)
\t\t\t(layer "B.Cu"
\t\t\t\t(type "copper")
\t\t\t\t(thickness 0.035)
\t\t\t)
\t\t\t(layer "B.Mask"
\t\t\t\t(type "Bottom Solder Mask")
\t\t\t\t(color "Green")
\t\t\t\t(thickness 0.01)
\t\t\t)
\t\t\t(layer "B.Paste"
\t\t\t\t(type "Bottom Solder Paste")
\t\t\t)
\t\t\t(layer "B.SilkS"
\t\t\t\t(type "Bottom Silk Screen")
\t\t\t\t(color "White")
\t\t\t)
\t\t\t(copper_finish "None")
\t\t\t(dielectric_constraints no)
\t\t)
\t\t(pad_to_mask_clearance 0)
\t\t(allow_soldermask_bridges_in_footprints no)
\t\t(tenting front back)
\t)
"""


def text_effects(size, thickness, indent):
    t = "\t" * indent
    return (
        f"{t}(effects\n{t}\t(font\n{t}\t\t(size {fmt(size)} {fmt(size)})\n"
        f"{t}\t\t(thickness {fmt(thickness)})\n{t}\t)\n{t})\n"
    )


def fp_line(x1, y1, x2, y2, layer, width, key):
    return (
        f"\t\t(fp_line\n\t\t\t(start {fmt(x1)} {fmt(y1)})\n\t\t\t(end {fmt(x2)} {fmt(y2)})\n"
        f"\t\t\t(stroke\n\t\t\t\t(width {fmt(width)})\n\t\t\t\t(type solid)\n\t\t\t)\n"
        f'\t\t\t(layer "{layer}")\n\t\t\t(uuid "{uid(key)}")\n\t\t)\n'
    )


def rect_lines(w, h, layer, width, key):
    x, y = w / 2, h / 2
    pts = [(-x, -y), (x, -y), (x, y), (-x, y), (-x, -y)]
    return "".join(
        fp_line(*a, *b, layer, width, f"{key}/{i}") for i, (a, b) in enumerate(pairwise(pts))
    )


def footprint_text(fp):
    out = []
    w = out.append
    w(f'\t(footprint "{fp.name}"\n')
    w('\t\t(layer "F.Cu")\n')
    w(f'\t\t(uuid "{uid(fp.ref)}")\n')
    w(f"\t\t(at {fmt(fp.at[0])} {fmt(fp.at[1])})\n")
    bw, bh = fp.body
    # References: inside the chips, above the other parts. Values are hidden
    # (they would crowd the small board).
    if fp.ref.startswith("U"):
        ref_y, ref_size = 0, 1.2
    elif fp.kind == "tht":
        ref_y, ref_size = -2.2, 1.0
    else:
        ref_y, ref_size = -bh / 2 - 0.75, 0.7
    for prop, val, y, layer, size, hide in (
        ("Reference", fp.ref, ref_y, "F.SilkS", ref_size, False),
        ("Value", fp.value, bh / 2 + 1.2, "F.Fab", 1.0, True),
    ):
        w(f'\t\t(property "{prop}" "{val}"\n')
        w(f"\t\t\t(at 0 {fmt(y)} 0)\n")
        w(f'\t\t\t(layer "{layer}")\n')
        if hide:
            w("\t\t\t(hide yes)\n")
        w(f'\t\t\t(uuid "{uid(fp.ref, prop)}")\n')
        w(text_effects(size, 0.15 * size, 3))
        w("\t\t)\n")
    w(f"\t\t(attr {'through_hole' if fp.kind == 'tht' else 'smd'})\n")

    # outline
    if fp.kind == "tht":
        n = len(fp.pads)
        x0, y0, x1, y1 = -1.33, -1.33, 1.33, (n - 1) * 2.54 + 1.33
        pts = [(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)]
        w(
            "".join(
                fp_line(*a, *b, "F.SilkS", 0.12, f"{fp.ref}/silk/{i}")
                for i, (a, b) in enumerate(pairwise(pts))
            )
        )
    else:
        w(rect_lines(bw, bh, "F.Fab", 0.1, f"{fp.ref}/fab"))
        if fp.ref.startswith("U"):
            s = bw / 2 + 0.1
            w(fp_line(-s, -s, -s + 0.8, -s, "F.SilkS", 0.12, f"{fp.ref}/s1"))
            w(fp_line(-s, -s, -s, -s + 0.8, "F.SilkS", 0.12, f"{fp.ref}/s2"))
            w(fp_line(s, s, s - 0.8, s, "F.SilkS", 0.12, f"{fp.ref}/s3"))
            w(fp_line(s, s, s, s - 0.8, "F.SilkS", 0.12, f"{fp.ref}/s4"))
            w(fp_line(s, -s, s - 0.8, -s, "F.SilkS", 0.12, f"{fp.ref}/s5"))
            w(fp_line(s, -s, s, -s + 0.8, "F.SilkS", 0.12, f"{fp.ref}/s6"))
            w(fp_line(-s, s, -s + 0.8, s, "F.SilkS", 0.12, f"{fp.ref}/s7"))
            w(fp_line(-s, s, -s, s - 0.8, "F.SilkS", 0.12, f"{fp.ref}/s8"))
        if fp.ref.startswith("D"):
            w(fp_line(1.5, -0.6, 1.5, 0.6, "F.SilkS", 0.12, f"{fp.ref}/k"))  # cathode

    for num, x, y, pw, ph, shape, drill in fp.pads:
        name = net_of(fp.ref, num)
        if drill:
            w(f'\t\t(pad "{num}" thru_hole {shape}\n')
            w(f"\t\t\t(at {fmt(x)} {fmt(y)})\n")
            w(f"\t\t\t(size {fmt(pw)} {fmt(ph)})\n")
            w(f"\t\t\t(drill {fmt(drill)})\n")
            w('\t\t\t(layers "*.Cu" "*.Mask")\n')
            w("\t\t\t(remove_unused_layers no)\n")
        else:
            w(f'\t\t(pad "{num}" smd {shape}\n')
            w(f"\t\t\t(at {fmt(x)} {fmt(y)})\n")
            w(f"\t\t\t(size {fmt(pw)} {fmt(ph)})\n")
            w('\t\t\t(layers "F.Cu" "F.Mask" "F.Paste")\n')
            w("\t\t\t(roundrect_rratio 0.25)\n")
        if name:
            w(f'\t\t\t(net {NET_NUM[name]} "{name}")\n')
        w(f'\t\t\t(uuid "{uid(fp.ref, "pad", num)}")\n')
        w("\t\t)\n")
    w("\t\t(embedded_fonts no)\n")
    w("\t)\n")
    return "".join(out)


def board_text():
    out = []
    w = out.append
    w("(kicad_pcb\n")
    w("\t(version 20241229)\n")
    w('\t(generator "pcbnew")\n')
    w('\t(generator_version "9.0")\n')
    w("\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)\n")
    w('\t(paper "A4")\n')
    w(
        '\t(title_block\n\t\t(title "kicad-bscan demo board")\n'
        '\t\t(comment 1 "Simulated by demo/demo_sim.py - generated by demo/generate.py")\n\t)\n'
    )
    w(LAYERS)
    w(SETUP)
    for i, n in enumerate(NET_NAMES):
        w(f'\t(net {i} "{n}")\n')
    for fp in FOOTPRINTS:
        w(footprint_text(fp))

    # outline and mounting holes
    pts = [(OX, OY), (OX + BW, OY), (OX + BW, OY + BH), (OX, OY + BH), (OX, OY)]
    for i, (a, b) in enumerate(pairwise(pts)):
        w(
            f"\t(gr_line\n\t\t(start {fmt(a[0])} {fmt(a[1])})\n\t\t(end {fmt(b[0])} {fmt(b[1])})\n"
            f"\t\t(stroke\n\t\t\t(width 0.1)\n\t\t\t(type solid)\n\t\t)\n"
            f'\t\t(layer "Edge.Cuts")\n\t\t(uuid "{uid("edge", i)}")\n\t)\n'
        )
    for i, (x, y) in enumerate(
        [(OX + 4, OY + 4), (OX + BW - 4, OY + 4), (OX + 4, OY + BH - 4), (OX + BW - 4, OY + BH - 4)]
    ):
        w(
            f"\t(gr_circle\n\t\t(center {fmt(x)} {fmt(y)})\n\t\t(end {fmt(x + 1.6)} {fmt(y)})\n"
            f"\t\t(stroke\n\t\t\t(width 0.1)\n\t\t\t(type solid)\n\t\t)\n\t\t(fill no)\n"
            f'\t\t(layer "Edge.Cuts")\n\t\t(uuid "{uid("hole", i)}")\n\t)\n'
        )
    for key, text, x, y, size in (
        ("title", "kicad-bscan demo board", OX + BW / 2, OY + BH - 4.5, 1.5),
        ("j1", "EXT", J1_AT[0], J1_AT[1] - 3.6, 1.0),
        ("j2", "SPI", J2_AT[0], J2_AT[1] - 3.6, 1.0),
    ):
        w(
            f'\t(gr_text "{text}"\n\t\t(at {fmt(x)} {fmt(y)} 0)\n\t\t(layer "F.SilkS")\n'
            f'\t\t(uuid "{uid("text", key)}")\n'
        )
        w(text_effects(size, 0.15 * size, 2))
        w("\t)\n")
    for i, (x1, y1, x2, y2, layer, name) in enumerate(SEGMENTS):
        w(
            f"\t(segment\n\t\t(start {fmt(x1)} {fmt(y1)})\n\t\t(end {fmt(x2)} {fmt(y2)})\n"
            f'\t\t(width {fmt(TRACK)})\n\t\t(layer "{layer}")\n\t\t(net {NET_NUM[name]})\n'
            f'\t\t(uuid "{uid("seg", i)}")\n\t)\n'
        )
    for i, (x, y, name) in enumerate(VIAS):
        w(
            f"\t(via\n\t\t(at {fmt(x)} {fmt(y)})\n\t\t(size {fmt(VIA_SIZE)})\n"
            f'\t\t(drill {fmt(VIA_DRILL)})\n\t\t(layers "F.Cu" "B.Cu")\n'
            f'\t\t(net {NET_NUM[name]})\n\t\t(uuid "{uid("via", i)}")\n\t)\n'
        )
    w("\t(embedded_fonts no)\n")
    w(")\n")
    return "".join(out)


def project_json():
    # KiCad fills in its defaults (and writes them when the project is saved)
    return {"meta": {"filename": f"{NAME}.kicad_pro", "version": 3}}


def bscan_json():
    def watch(ref, pad):
        return {"reference": ref, "pad": str(pad), "net": net_of(ref, pad)}

    return {
        "version": 1,
        "openocd": {"host": "127.0.0.1", "port": 6666},
        "taps": [
            {
                "tap": MCU.tap,
                "idcode": f"0x{MCU.idcode:08x}",
                "reference": "U1",
                "bsdl": "bsdl/bsdemo_m32.bsd",
                # keep the FPGA configured (nPROG high) whenever EXTEST runs
                "extest_start": {"11": "1"},
            },
            {
                "tap": FPGA.tap,
                "idcode": f"0x{FPGA.idcode:08x}",
                "reference": "U2",
                "bsdl": "bsdl/bsdemo_f48.bsd",
            },
        ],
        "watch": [
            watch("U2", 29),  # LED1: output
            watch("U1", 7),  # STAT: output, blinking
            watch("U2", 16),  # GCLK: input, the clock from U1
            watch("U1", 25),  # IRQ: input, from U2
            watch("U1", 2),  # NRST: input, from J1
            watch("U1", 6),  # BTN: input, pressed now and then
        ],
        "bsdl_dirs": ["bsdl"],
        "interconnect_include": [],
    }


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n") as f:
        f.write(text)
    print("wrote", os.path.relpath(path, os.path.dirname(HERE)))


def main():
    write(os.path.join(PROJECT, "bsdl", "bsdemo_m32.bsd"), bsdl(MCU))
    write(os.path.join(PROJECT, "bsdl", "bsdemo_f48.bsd"), bsdl(FPGA))
    write(os.path.join(PROJECT, f"{NAME}.kicad_pcb"), board_text())
    write(os.path.join(PROJECT, f"{NAME}.kicad_pro"), json.dumps(project_json(), indent=2) + "\n")
    write(os.path.join(PROJECT, f"{NAME}.bscan.json"), json.dumps(bscan_json(), indent=2) + "\n")
    write(os.path.join(HERE, "sim", "board.json"), json.dumps(sim_json(), indent=1) + "\n")


if __name__ == "__main__":
    main()
