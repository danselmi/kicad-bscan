# TODO

## Open
- [ ] Windows Package size OK for now: 30 MB (DLLs 28 MB, mostly Abseil/protobuf;
  `.exe` about 2 MB). If a ready-made Windows package is offered one day:
  link statically (only the used code, no DLLs).

- [ ] **Docs**
  - Update `doc/DEVELOPMENT.md` once `jtag scan` / `jtag cur_ir` are merged
    in OpenOCD.

- [ ] EXTEST setup dialog with large chips: are the filters (net, changing
  pins, search) and sorting enough with 150+ output pins?

## Ideas (on request)

Not planned; for when someone needs them. Mostly towards production tests.

- **SVF export** of the interconnect test (+ map file from failing bits to
  nets), for a test fixture with OpenOCD or another SVF player, without
  KiCad.
- **Resistors in the interconnect test:**
  - series resistors (e.g. matching): treat the nets on both sides as one
    net;
  - pull-ups / pull-downs: release the net (all drivers Z) and check that
    the receivers read the pulled level;
  - resistors between two nets (e.g. termination of a differential pair):
    check that driving one net does not force the other.
- **Connectors with a loopback plug:** pairs of connector pins connected by
  the plug (set in the project), so nets that end at a connector can be
  tested.
- **Clusters:** non-boundary-scan parts with known behaviour, e.g. buffers
  and level shifters (pass-through with direction), memories (write and
  read back through the bus), I2C/SPI devices (read an ID by bit-banging
  their bus through boundary-scan pins).
- **Operator steps:** LEDs and buttons checked by a person ("does LED1
  light?", "press BTN"), as part of a test sequence.
- **Coverage report:** which nets and pins a test covers and why the others
  are not covered (the interconnect tab shows this already, but not as a
  document).
- **Test log per board** (serial number, date, results), for production.



## Won't do

- Not planned: a "park" extension in OpenOCD (EXTEST together with IPDBG or
  target polling) – too complex for a corner case; the interruption warning
  covers it.

