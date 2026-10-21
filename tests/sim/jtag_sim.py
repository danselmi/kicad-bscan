#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""JTAG chain simulator for OpenOCD's remote_bitbang adapter.

    python3 tests/sim/jtag_sim.py [port] [--short] [--open]   (default port 5555)

    --short   the two nets below are shorted (wired AND)
    --open    OTHER.IN1 is not soldered (reads 1, its pull-up)

Chain, seen from TDO (= order of OpenOCD's 'jtag newtap' declarations):

  pos 0  SIMCHIP   IR 4, IDCODE 0x1B5C30EF, boundary register of 8 cells
                   (matches tests/sim/simchip.bsd)
  pos 1  OTHER     IR 5, IDCODE 0x4BA00477, the same 8 cells
                   (matches tests/sim/simchip2.bsd)

Simulated pin behaviour of both chips (time t in seconds):
  IN0   input, toggles every second
  IN1   input, wired to IO0 of the other chip (a board net, with pull-up)
  OUT0  output, enabled, core drives (t // 2) % 2
  IO0   bidir, disabled (control = 0), external pull-up -> reads 1
  A1    input, constant 0

The two nets NET_A = SIMCHIP.IO0 - OTHER.IN1 and NET_B = OTHER.IO0 -
SIMCHIP.IN1 allow interconnect tests: drive IO0 of one chip in EXTEST, read
IN1 of the other. --short and --open add faults to them.

Only what boundary-scan tests need is modelled: IR/DR shifting, BYPASS,
IDCODE, SAMPLE/PRELOAD and EXTEST (update register drives the pins).
"""
import socket
import sys
import time

# TAP controller states
(RESET, IDLE, SEL_DR, CAP_DR, SH_DR, EX1_DR, PA_DR, EX2_DR, UPD_DR,
 SEL_IR, CAP_IR, SH_IR, EX1_IR, PA_IR, EX2_IR, UPD_IR) = range(16)

NEXT = {
    RESET:  (IDLE, RESET),     IDLE:   (IDLE, SEL_DR),
    SEL_DR: (CAP_DR, SEL_IR),  CAP_DR: (SH_DR, EX1_DR),
    SH_DR:  (SH_DR, EX1_DR),   EX1_DR: (PA_DR, UPD_DR),
    PA_DR:  (PA_DR, EX2_DR),   EX2_DR: (SH_DR, UPD_DR),
    UPD_DR: (IDLE, SEL_DR),    SEL_IR: (CAP_IR, RESET),
    CAP_IR: (SH_IR, EX1_IR),   SH_IR:  (SH_IR, EX1_IR),
    EX1_IR: (PA_IR, UPD_IR),   PA_IR:  (PA_IR, EX2_IR),
    EX2_IR: (SH_IR, UPD_IR),   UPD_IR: (IDLE, SEL_DR),
}


class Tap:
    def __init__(self, name, irlen, idcode, ops, boundary_len=0):
        self.link = None                  # chip whose IO0 drives our IN1
        self.name = name
        self.irlen = irlen
        self.idcode = idcode
        self.ops = ops                    # opcode -> register name
        self.blen = boundary_len
        self.update_reg = [0] * boundary_len
        self.reset()

    def reset(self):
        self.ir = self.opcode("IDCODE")
        self.shift = []                   # bit list, index 0 = next out (TDO side)

    def opcode(self, name):
        for k, v in self.ops.items():
            if v == name:
                return k
        raise KeyError(name)

    def selected(self):
        return self.ops.get(self.ir, "BYPASS")

    def io0_drive(self):
        """Value driven on IO0, None when not driven."""
        if self.selected() == "EXTEST" and self.update_reg[4]:
            return self.update_reg[5]
        return None                             # core keeps IO0 disabled

    def io0_pin(self):
        """Level of the net on our IO0 pin."""
        return net_level(self)

    def in1_pin(self):
        """Level at our IN1 pin: the net of the other chip's IO0."""
        if self.link is None:
            return 1
        if FAULTS["open"] and self.name == "OTHER":
            return 1                            # not soldered: own pull-up
        return net_level(self.link)

    # --- boundary register content on Capture-DR ---
    def capture_boundary(self):
        t = time.monotonic()
        core_out0 = int(t // 2) % 2
        extest = self.selected() == "EXTEST"
        cells = [0] * self.blen
        if extest:
            # outputs driven from the update register
            oe0, out0 = self.update_reg[2], self.update_reg[3]
            oe_io, out_io = self.update_reg[4], self.update_reg[5]
        else:
            oe0, out0 = 1, core_out0
            oe_io, out_io = 0, 0
        pin_out0 = out0 if oe0 else 1          # pulled up when not driven
        pin_io0 = self.io0_pin() if self.link else (out_io if oe_io else 1)
        cells[0] = int(t) % 2                   # IN0
        cells[1] = self.in1_pin()               # IN1
        cells[2] = oe0                          # control of OUT0
        cells[3] = out0 if not extest else pin_out0
        cells[4] = oe_io                        # control of IO0
        cells[5] = pin_io0                      # BC_7 captures the pin
        cells[6] = 0                            # port A1
        cells[7] = 0                            # internal
        return cells

    def capture_dr(self):
        reg = self.selected()
        if reg == "IDCODE":
            self.shift = [(self.idcode >> i) & 1 for i in range(32)]
        elif reg in ("SAMPLE", "EXTEST"):
            self.shift = self.capture_boundary()
        else:
            self.shift = [0]

    def update_dr(self):
        if self.selected() in ("SAMPLE", "EXTEST"):
            self.update_reg = list(self.shift)

    def capture_ir(self):
        # IEEE 1149.1: the two LSBs capture "01"
        self.shift = [1, 0] + [0] * (self.irlen - 2)

    def update_ir(self):
        self.ir = sum(b << i for i, b in enumerate(self.shift))

    def shift_bit(self, tdi):
        out = self.shift[0]
        self.shift = self.shift[1:] + [tdi]
        return out


CHIP = Tap("SIMCHIP", 4, 0x1B5C30EF,
           {0b0000: "EXTEST", 0b0001: "SAMPLE", 0b0010: "IDCODE", 0b1111: "BYPASS"},
           boundary_len=8)
OTHER = Tap("OTHER", 5, 0x4BA00477,
            {0b00000: "EXTEST", 0b00001: "SAMPLE", 0b01110: "IDCODE",
             0b11111: "BYPASS"},
            boundary_len=8)
CHIP.link, OTHER.link = OTHER, CHIP

FAULTS = {"short": False, "open": False}


def net_level(tap):
    """Level of the net on tap's IO0: wired AND of the drivers, pull-up."""
    drivers = [tap.io0_drive()]
    if FAULTS["short"]:
        drivers.append(tap.link.io0_drive())
    level = 1
    for d in drivers:
        if d is not None:
            level &= d
    return level

# Index 0 is closest to TDO; TDI enters at the last TAP.
CHAIN = [CHIP, OTHER]


class Sim:
    def __init__(self):
        self.state = RESET
        self.tck = 0
        self.tdo = 0
        self.tms = 0
        self.tdi = 0
        self.update_tdo()

    def update_tdo(self):
        if self.state in (SH_DR, SH_IR):
            self.tdo = CHAIN[0].shift[0]
        else:
            self.tdo = 1

    def rising_edge(self):
        st = self.state
        if st in (SH_DR, SH_IR):
            bit = self.tdi
            for tap in reversed(CHAIN):          # TDI side first
                bit = tap.shift_bit(bit)
        nxt = NEXT[st][self.tms]
        if nxt == CAP_DR:
            for tap in CHAIN:
                tap.capture_dr()
        elif nxt == CAP_IR:
            for tap in CHAIN:
                tap.capture_ir()
        elif nxt == UPD_DR:
            for tap in CHAIN:
                tap.update_dr()
        elif nxt == UPD_IR:
            for tap in CHAIN:
                tap.update_ir()
        elif nxt == RESET:
            for tap in CHAIN:
                tap.reset()
        self.state = nxt
        self.update_tdo()

    def write(self, tck, tms, tdi):
        self.tms, self.tdi = tms, tdi
        if tck and not self.tck:
            self.rising_edge()
        self.tck = tck

    def trst(self):
        self.state = RESET
        for tap in CHAIN:
            tap.reset()
        self.update_tdo()


def serve(port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print(f"jtag_sim listening on 127.0.0.1:{port}", flush=True)
    while True:
        conn, _ = srv.accept()
        sim = Sim()
        with conn:
            while True:
                data = conn.recv(65536)
                if not data:
                    break
                out = bytearray()
                for c in data:
                    ch = chr(c)
                    if "0" <= ch <= "7":
                        v = c - ord("0")
                        sim.write((v >> 2) & 1, (v >> 1) & 1, v & 1)
                    elif ch == "R":
                        out += b"1" if sim.tdo else b"0"
                    elif ch in "tu":            # TRST asserted (t: srst 0, u: srst 1)
                        sim.trst()
                    elif ch == "Q":
                        break
                    # B/b (blink), r/s (trst deasserted) and others: ignore
                if out:
                    conn.sendall(bytes(out))


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    FAULTS["short"] = "--short" in sys.argv
    FAULTS["open"] = "--open" in sys.argv
    serve(int(args[0]) if args else 5555)
