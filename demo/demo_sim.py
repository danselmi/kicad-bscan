#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""JTAG simulator of the kicad-bscan demo board, for OpenOCD's remote_bitbang.

    python3 demo/demo_sim.py [--port 5555] [--short NET1,NET2] [--open REF.PIN]
    openocd -f demo/openocd-demo.cfg

Reads demo/sim/board.json (written by demo/generate.py): the chips on the
JTAG chain with their boundary registers, and the board's nets between their
pins.

    --short NET1,NET2   the two nets are shorted (wired AND); repeatable
    --open REF.PIN      the pin is not soldered (it reads 1, the net does not
                        see it); repeatable, e.g. --open U2.5

Modelled: TAP controller, IR, BYPASS, IDCODE, SAMPLE/PRELOAD, EXTEST, CLAMP
and HIGHZ. While a chip is not in EXTEST/CLAMP/HIGHZ its own logic drives its
pins (see 'core' in generate.py: counters on the data bus and the LEDs, a
clock, blinking and toggling pins). A net nobody drives reads 1 (pull-ups);
several drivers on a net give the wired AND. The button on J1 (net BTN) is
pressed for half a second every four seconds.
"""

import argparse
import json
import os
import socket
import time

HERE = os.path.dirname(os.path.abspath(__file__))
T0 = time.monotonic()

# TAP controller states
(
    RESET,
    IDLE,
    SEL_DR,
    CAP_DR,
    SH_DR,
    EX1_DR,
    PA_DR,
    EX2_DR,
    UPD_DR,
    SEL_IR,
    CAP_IR,
    SH_IR,
    EX1_IR,
    PA_IR,
    EX2_IR,
    UPD_IR,
) = range(16)

NEXT = {
    RESET: (IDLE, RESET),
    IDLE: (IDLE, SEL_DR),
    SEL_DR: (CAP_DR, SEL_IR),
    CAP_DR: (SH_DR, EX1_DR),
    SH_DR: (SH_DR, EX1_DR),
    EX1_DR: (PA_DR, UPD_DR),
    PA_DR: (PA_DR, EX2_DR),
    EX2_DR: (SH_DR, UPD_DR),
    UPD_DR: (IDLE, SEL_DR),
    SEL_IR: (CAP_IR, RESET),
    CAP_IR: (SH_IR, EX1_IR),
    SH_IR: (SH_IR, EX1_IR),
    EX1_IR: (PA_IR, UPD_IR),
    PA_IR: (PA_IR, EX2_IR),
    EX2_IR: (SH_IR, UPD_IR),
    UPD_IR: (IDLE, SEL_DR),
}


def core_value(core, t):
    """What the chip's own logic drives, None if the output is off."""
    if not core:
        return None
    kind = core[0]
    if kind == "drive":
        return core[1]
    if kind == "toggle":
        return int(t / core[1]) % 2
    if kind == "counter":
        return (int(t / core[2]) >> core[1]) & 1
    if kind == "pulse":
        return core[3] if t % core[1] < core[2] else None
    raise ValueError(core)


class Chip:
    def __init__(self, desc, board):
        self.board = board
        self.ref = desc["ref"]
        self.irlen = desc["irlen"]
        self.idcode = desc["idcode"]
        self.ops = {int(bits, 2): name for bits, name in desc["opcodes"].items()}
        self.cells = desc["cells"]
        self.ports = desc["ports"]
        self.update_reg = [0] * len(self.cells)
        # port -> (data cell, control cell or None, disable value)
        self.out_cells = {}
        self.in_cells = {}
        for c in self.cells:
            if c["func"] in ("output2", "output3", "bidir"):
                self.out_cells[c["port"]] = (c["num"], c.get("ctrl"), c.get("dis", 0))
            if c["func"] in ("input", "bidir"):
                self.in_cells[c["port"]] = c["num"]
        self.reset()

    def reset(self):
        self.ir = self.opcode("IDCODE")
        self.shift = []  # index 0 = next bit out (TDO side)

    def opcode(self, name):
        return next(k for k, v in self.ops.items() if v == name)

    def instruction(self):
        return self.ops.get(self.ir, "BYPASS")

    def drive(self, port, t):
        """Value this chip drives on port, None if not driven."""
        mode = self.instruction()
        if mode == "HIGHZ":
            return None
        if mode in ("EXTEST", "CLAMP"):
            if port not in self.out_cells:
                return None
            data, ctrl, dis = self.out_cells[port]
            if ctrl is not None and self.update_reg[ctrl] == dis:
                return None
            return self.update_reg[data]
        return core_value(self.ports[port]["core"], t)

    def capture_boundary(self, t):
        extest = self.instruction() == "EXTEST"
        bits = []
        for c in self.cells:
            n, func, port = c["num"], c["func"], c["port"]
            if func == "control":
                if extest:
                    bits.append(self.update_reg[n])
                else:
                    # the core's enable of the ports this cell controls
                    on = any(
                        core_value(self.ports[p]["core"], t) is not None
                        for p, (_, ctrl, _) in self.out_cells.items()
                        if ctrl == n
                    )
                    bits.append(1 - c.get("dis", 0) if on else c.get("dis", 0))
            elif func in ("input", "bidir"):
                bits.append(self.board.pin_level(self, port, t))
            else:  # output2 / output3: captures what the logic would drive
                if extest:
                    bits.append(self.update_reg[n])
                else:
                    v = core_value(self.ports[port]["core"], t)
                    bits.append(0 if v is None else v)
        return bits

    def capture_dr(self, t):
        reg = self.instruction()
        if reg == "IDCODE":
            self.shift = [(self.idcode >> i) & 1 for i in range(32)]
        elif reg in ("SAMPLE", "EXTEST"):
            self.shift = self.capture_boundary(t)
        else:
            self.shift = [0]

    def update_dr(self):
        if self.instruction() in ("SAMPLE", "EXTEST"):
            self.update_reg = list(self.shift)

    def capture_ir(self):
        self.shift = [1, 0] + [0] * (self.irlen - 2)

    def update_ir(self):
        self.ir = sum(b << i for i, b in enumerate(self.shift))

    def shift_bit(self, tdi):
        out = self.shift[0]
        self.shift = self.shift[1:] + [tdi]
        return out


class Board:
    def __init__(self, desc, shorts, opens):
        self.chain = [Chip(c, self) for c in desc["chain"]]
        by_ref = {c.ref: c for c in self.chain}
        self.opens = set(opens)

        # nets (merged by shorts) -> list of (chip, port, "REF.PIN")
        group = {n: n for n in desc["nets"]}
        for a, b in shorts:
            for n in (a, b):
                if n not in group:
                    raise SystemExit(f"--short: unknown net {n} (see demo/sim/board.json)")
            old, new = group[b], group[a]
            for n, g in group.items():
                if g == old:
                    group[n] = new

        # drivers outside the chips (the button), by group
        self.external = {group[n]: spec for n, spec in desc.get("external", {}).items()}

        self.net_of = {}  # (ref, port) -> group
        self.members = {}  # group -> [(chip, port)]
        for name, pins in desc["nets"].items():
            for key in pins:
                ref, pin = key.split(".")
                chip = by_ref[ref]
                port = next(p for p, d in chip.ports.items() if str(d["pin"]) == pin)
                if key in self.opens:
                    continue
                self.net_of[(ref, port)] = group[name]
                self.members.setdefault(group[name], []).append((chip, port))

        for key in self.opens:
            ref, pin = key.split(".")
            if ref not in by_ref or not any(
                str(d["pin"]) == pin for d in by_ref[ref].ports.values()
            ):
                raise SystemExit(f"--open: unknown pin {key}")

    def pin_level(self, chip, port, t):
        g = self.net_of.get((chip.ref, port))
        if g is None:
            # not connected (or not soldered): only the chip itself, pull-up
            v = (
                chip.drive(port, t)
                if f"{chip.ref}.{chip.ports[port]['pin']}" not in self.opens
                else None
            )
            return 1 if v is None else v
        level = 1
        v = core_value(self.external.get(g), t)
        if v is not None:
            level &= v
        for c, p in self.members[g]:
            v = c.drive(p, t)
            if v is not None:
                level &= v
        return level


class Sim:
    def __init__(self, board):
        self.chain = board.chain  # index 0 is closest to TDO
        self.state = RESET
        self.tck = self.tms = self.tdi = 0
        self.update_tdo()

    def update_tdo(self):
        self.tdo = self.chain[0].shift[0] if self.state in (SH_DR, SH_IR) else 1

    def rising_edge(self):
        st = self.state
        if st in (SH_DR, SH_IR):
            bit = self.tdi
            for chip in reversed(self.chain):  # TDI side first
                bit = chip.shift_bit(bit)
        nxt = NEXT[st][self.tms]
        t = time.monotonic() - T0
        for chip in self.chain:
            if nxt == CAP_DR:
                chip.capture_dr(t)
            elif nxt == CAP_IR:
                chip.capture_ir()
            elif nxt == UPD_DR:
                chip.update_dr()
            elif nxt == UPD_IR:
                chip.update_ir()
            elif nxt == RESET:
                chip.reset()
        self.state = nxt
        self.update_tdo()

    def write(self, tck, tms, tdi):
        self.tms, self.tdi = tms, tdi
        if tck and not self.tck:
            self.rising_edge()
        self.tck = tck

    def trst(self):
        self.state = RESET
        for chip in self.chain:
            chip.reset()
        self.update_tdo()


def serve(port, board):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print(f"demo_sim listening on 127.0.0.1:{port}", flush=True)
    while True:
        conn, _ = srv.accept()
        sim = Sim(board)
        with conn:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                out = bytearray()
                for c in data:
                    if 48 <= c <= 55:  # '0'..'7'
                        v = c - 48
                        sim.write((v >> 2) & 1, (v >> 1) & 1, v & 1)
                    elif c == 82:  # 'R'
                        out += b"1" if sim.tdo else b"0"
                    elif c in (116, 117):  # 't', 'u': TRST asserted
                        sim.trst()
                    elif c == 81:  # 'Q'
                        break
                if out:
                    conn.sendall(bytes(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", type=int, default=5555)
    ap.add_argument("--short", action="append", default=[], metavar="NET1,NET2")
    ap.add_argument("--open", action="append", default=[], metavar="REF.PIN")
    ap.add_argument("--board", default=os.path.join(HERE, "sim", "board.json"))
    args = ap.parse_args()

    shorts = []
    for s in args.short:
        parts = s.split(",")
        if len(parts) != 2:
            ap.error(f"--short {s}: expected NET1,NET2")
        shorts.append(parts)

    with open(args.board) as f:
        board = Board(json.load(f), shorts, args.open)
    serve(args.port, board)


if __name__ == "__main__":
    main()
