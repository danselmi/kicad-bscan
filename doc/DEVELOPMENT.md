# Development notes

## Overview

The plugin is a separate process started by KiCad through its IPC API
(`plugin.json`, `runtime.type = "exec"`). It talks to two servers:

- **KiCad** over NNG (REQ/REP, `ipc://` socket from `KICAD_API_SOCKET`, token
  from `KICAD_API_TOKEN`) with protobuf messages (`ApiRequest`/`ApiResponse`
  wrapping `google.protobuf.Any`).
- **OpenOCD** over its Tcl RPC port (default 6666): commands terminated by
  0x1a. The server does not report errors, so every command is wrapped as
  `format "%d:%s" [catch {...} r] $r`; `irscan` errors only go to OpenOCD's
  log, so scans are wrapped in `capture`.

## Source layout

```
src/openocd/tcl_client.*    TCP client for OpenOCD's Tcl RPC port
src/bscan/bits.*            bit vector (bit i = boundary cell i)
src/bscan/bsdl_device.*     C++ wrapper around libbsdl
src/bscan/chain.*           scan_chain parsing, SAMPLE, EXTEST scans, pin decoding
src/bscan/extest.*          EXTEST planning (starting states, conflicts), take over
src/bscan/bsdl_index.*      BSDL folder search, cache, IDCODE matching, ranking
src/bscan/interconnect.*    interconnect test: nets, patterns, diagnosis
src/bscan/project_config.*  <project>.bscan.json
src/bscan/live_sampler.*    worker thread: API, cycle (chain, jobs, sampling, wait)
src/bscan/sampler_types.h   what the sampler takes and reports
src/bscan/sampler_context.h what the jobs of one cycle share
src/bscan/extest_table.*    TAPs held in EXTEST (thread-safe bookkeeping)
src/bscan/extest_job.*      EXTEST requests: enter, take over, leave, interruptions
src/bscan/sample_job.*      reading pins: one cycle, snapshot before EXTEST
src/bscan/interconnect_job.* applies image lists, returns captures (interconnect test)
src/kicad/ipc_client.*      NNG client, error mapping
src/kicad/board_watcher.*   worker thread: selection, footprints, pad nets
src/bscan/interconnect_session.* interconnect test: plan, run, fault statistics (no GUI)
src/bscan/tap_status.*      status of a TAP in the chain table (check, EXTEST possible)
src/bscan/package_layout.*  chip view: package layout from the pin names (BGA, quad, dual)
src/gui/main_frame.*        window: connection line, tabs, workers, passes changes on
src/gui/session.*           what the parts share: board, settings, chain, devices, EXTEST state
src/gui/extest_controller.* EXTEST actions: setup, review, enter, take over, drive, leave
src/gui/extest_banner.*     the red EXTEST bar
src/gui/chain_page.*        tab "JTAG chain" (with chain_table.*, BSDL folders)
src/gui/pins_page.*         tab "Pins": selected pad, watch list, live values
src/gui/interconnect_page.* tab "Interconnect test"
src/gui/chip_view.*         chip view window: chooser, layout, sampling, selection, pin menu
src/gui/chip_canvas.*       its drawing: package, pin colours, legend, tooltips (ChipDrawing in)
src/gui/*_dialogs.*         EXTEST and BSDL dialogs
src/gui/gui_events.*        wx events between the parts of the window (layout, assignments)
tools/bscan_chain.cpp       developer tool: chain, BSDL check, SAMPLE, BSDL search
tools/kicad_probe.cpp       command-line test of the KiCad connection
art/*.svg                   icon sources (see "Icons")
tests/test_bscan.cpp        unit tests (ctest)
tests/extest_demo.cpp       EXTEST in the sampler against OpenOCD (manual)
tests/multi_extest_demo.cpp several TAPs in EXTEST against the simulator
tests/interconnect_demo.cpp interconnect test against the simulator
tests/fake_kicad.cpp        stand-in for KiCad's API server
tests/sim/                  JTAG simulator, test BSDL files, OpenOCD config
demo/                       demo board: generator, simulator, KiCad project (demo/README.md)
third_party/libbsdl/        BSDL parser (git submodule)
third_party/kicad-api/      KiCad API .proto files (10.0 branch, see SOURCE.txt)
```

## Building

```sh
cmake -S . -B build -DBSCAN_BUILD_TESTS=ON -DBSCAN_BUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build
```

The default build type is `Release` (no debug information; with GCC/MinGW it
would end up in the `.exe`). Options: `BSCAN_BUILD_GUI` (default ON), `BSCAN_BUILD_TOOLS` (OFF),
`BSCAN_BUILD_TESTS` (OFF), `KICAD_PLUGIN_DIR`, `BSCAN_PLUGIN_ID`,
`BSCAN_PLUGIN_NAME`; Linux: `BSCAN_INSTALL_DESKTOP_FILE` (ON); Windows:
`BSCAN_INSTALL_DLLS` (ON, needs CMake 3.21).

**Windows DLLs:** `cmake --install` copies the DLLs `kicad-bscan.exe` needs
next to it (`install(TARGETS ... RUNTIME_DEPENDENCIES)`). They are searched in
`PATH`, `<prefix>/bin` of every `CMAKE_PREFIX_PATH` entry, `wxWidgets_LIB_DIR`
and the compiler's folder (MinGW runtime); DLLs from the Windows folder are
left out. A DLL that is not found stops the install with its name – add its
folder to `CMAKE_PREFIX_PATH` or `PATH`.

Protobuf: both 3.x and newer versions with Abseil work; the C++ code is
generated from the `.proto` files at build time.

## Developer tools

Built with `-DBSCAN_BUILD_TOOLS=ON`, never installed. They use the same code
as the plugin (`bscan_core`, `bscan_kicad`) and only read – driving pins
(EXTEST) is only possible in the plugin window, with its review.

```sh
./build/bscan-chain list                                  # TAPs, IR, IDCODE, cur IR; jtag scan / cur_ir available?
./build/bscan-chain check  <tap> chip.bsd                 # does the BSDL file match?
./build/bscan-chain sample <tap> chip.bsd --watch 100 A12 # follow pin A12
./build/bscan-chain find-bsdl <tap|0xIDCODE> ~/bsdl       # matching BSDL files (same cache as the plugin)
./build/bscan-kicad-probe [--watch]                       # KiCad version, selected pad
```

`<tap>` is the OpenOCD TAP name or its position in the chain; `--host` and
`--port` select another OpenOCD.

## Testing without hardware

**JTAG simulator** – `tests/sim/jtag_sim.py` implements OpenOCD's
`remote_bitbang` protocol with a chain of two TAPs; `tests/sim/simchip.bsd`
and `tests/sim/simchip2.bsd` describe them (with pull-ups, so EXTEST effects
are visible; IO0 of each chip is wired to IN1 of the other).
OpenOCD must be built with `--enable-remote-bitbang`.

```sh
python3 tests/sim/jtag_sim.py &
openocd -f tests/sim/openocd-sim.cfg &
./build/bscan-chain sample simchip.tap tests/sim/simchip.bsd --watch 100
./build/extest-demo tests/sim/simchip.bsd    # restart OpenOCD while it waits
./build/multi-extest-demo tests/sim/simchip.bsd tests/sim/simchip2.bsd   # needs 'jtag scan'
./build/interconnect-demo tests/sim/simchip.bsd tests/sim/simchip2.bsd [ok|short|open]
                                             # jtag_sim.py with --short / --open injects faults
```

**Fake KiCad** – `fake-kicad` answers the API calls the plugin uses, with a
changing selection and occasional "busy" replies; `--kicad9` omits the pad's
parent footprint like KiCad 9.

```sh
./build/fake-kicad ipc:///tmp/fake.sock tok &
KICAD_API_SOCKET=ipc:///tmp/fake.sock KICAD_API_TOKEN=tok ./build/kicad-bscan
```

**KiCad connection** – `bscan-kicad-probe` prints the KiCad version and the
current selection (`--watch` for every change). Started by hand it uses the
default socket (`/tmp/kicad/api.sock`) and no token.

## Design notes

**Texts in `_( "..." )` are ASCII only.** `_()` converts with the current
locale, which is not UTF-8 on Windows: "–" shows up as "â€“". For other
characters (symbols such as "⚠" or "←") use `wxString::FromUTF8()` outside
`_()`.

**Events or callbacks.** Windows tell other parts of the window about
changes with wxWidgets events (`gui_events.h`, `EVT_CHAIN_*` in
`chain_table.h`), sent with `SendGuiEvent()` and processed at once. Callbacks
(`std::function`) remain where an event does not fit: the libraries without
wxWidgets and their worker threads (`LiveSampler`, `BoardWatcher`), classes
that are not windows (`ExtestController`, `Session`), and calls that need an
answer (`ChainTable::OnBsdlChosen`).

**KiCad 9:** pads carry no parent footprint (added in 10.0.6), so the board
watcher builds a pad → footprint table from `GetItems` (footprints with their
pads) once per board and reloads it when an unknown pad shows up. KiCad
10.0.7 reports the parent; pads, multi-selection and "+ Selection" work
there without the table.

**Unconnected pads:** KiCad names their net `unconnected-(...)` (9 and
10.0.7); such names count as "no net".

**Scans are not atomic with irscan + drscan.** OpenOCD runs due timer
callbacks (target polling, IPDBG) before every registered command
(`command.c`, before the handler call), so between our `irscan` and `drscan`
– even in one Tcl message – other JTAG activity can happen. With a single
chip and nothing else on the chain this does not occur. If OpenOCD has
`jtag scan` (IR + DR in one handler), the plugin uses it for every scan,
always with `-force-ir` (OpenOCD's record of the instruction is wrong after
a board reset or power cycle).

**Detecting OpenOCD commands:** `usage {jtag scan}` is empty if the command
does not exist. (Calling an unknown `jtag` subcommand does not say "invalid
command" but prints the usage of the whole `jtag` group.) The result is
cached per connection (`TclClient::HasCommand`).

**Several TAPs per scan:** OpenOCD's `irscan`/`drscan` handle one TAP and put
all others into BYPASS (`driver.c`). `jtag scan` takes several TAPs
(`(tap instruction (num_bits value)+)+ [-force-ir] [-endstate state]`,
reply: the captured fields in command-line order, separated by a space;
TAPs not listed go to BYPASS).

- With `jtag scan`, each sampling cycle is one `jtag scan`
  (`LiveSampler::sampleTogether`): every TAP in EXTEST with EXTEST and its
  image (also TAPs without sampled pins), every other sampled TAP with
  SAMPLE. Entering, leaving and taking over EXTEST and the snapshot before
  the review scan the other TAPs in EXTEST along (`aAlong` in `chain.h`), so
  several TAPs can be in EXTEST. The background-polling check is skipped
  while a TAP is in EXTEST (`poll` would interrupt it).
- Without `jtag scan`: while one TAP is in EXTEST (active, being entered),
  the sampler and the GUI do not scan any other TAP and refuse a second
  EXTEST.
- Either way: a TAP held in EXTEST from an earlier session (image unknown to
  the sampler) blocks scanning other TAPs until it is left or taken over.
  With `jtag scan`, all held TAPs are taken over together
  (`MainFrame::takeOverGroup`, `LiveSampler::TakeOverExtest( vector )`):
  `jtag cur_ir` must report EXTEST for every one, then one scan drives all
  images – taking them over one by one would put the others into BYPASS.

`tests/sim` has two chips with boundary registers whose IO0/IN1 are
cross-wired; `multi-extest-demo` checks two TAPs in EXTEST against it,
including a group take-over after a "restart" of the sampler.

**Interconnect test** (`interconnect.*`, `LiveSampler::RunInterconnect`,
`gui/interconnect_page.*`):

- Nets come from all pads of the board (`BoardState::padNets`). A tested
  net has one driver and receivers (pins with input cells), so every pin's
  joint is checked once. Driver choice avoids 2-state outputs on the net
  and output enables shared with pins that would then drive.
- Codes 1 .. 2^(w-1) - 1, true then complement: 2 * w patterns. The top
  code bit is always 0, so no code is the complement of another – otherwise
  a wired-AND short of two complementary codes reads all 0 and looks like
  stuck-at-0.
- In the sampler: scan k shifts pattern k and captures pattern k-1; the
  final scan (previous image / base image) captures the last pattern.
  Devices entered for the test are registered in `m_extest` meanwhile, so a
  lost connection leaves EXTEST as usual.
- Diagnosis per net: wired AND/OR with another net's code or the other code
  itself → short; all receivers constant → stuck (open at the driver or
  short to a supply/GND); some receivers constant → open there.

**EXTEST safety in the sampler** (`live_sampler.*`):

- PRELOAD + image + EXTEST + image are sent as one Tcl command.
- In EXTEST, output values come from the image being driven (an output cell's
  capture shows the chip's logic, not the pin); inputs come from the capture.
- Connection lost while in EXTEST: on reconnect EXTEST is left, never resumed.
- `Stop()` leaves EXTEST unless disabled (window closed with "keep").
- Take over: `jtag cur_ir` must report EXTEST right before; then EXTEST +
  image without PRELOAD/SAMPLE, so the pins never return to the chip's logic.
- Interruptions: before each cycle's scans, `jtag cur_ir` of every TAP in
  EXTEST is read. OpenOCD also records the BYPASS it shifts into a TAP while
  scanning another one (`driver.c`), so anything since our last scan shows
  up – target polling, IPDBG, other clients. The entry is marked dirty (the
  next scan restores EXTEST and the image, also for TAPs without sampled
  pins) and `ExtestUpdate::State::Interrupted` reports it with a count.
  Checked at least every 500 ms while in EXTEST.
- Background polling (warning in the review): `BackgroundPolling()` calls
  `poll` only if `target names` is not empty – `poll` without a target makes
  OpenOCD exit ("BUG: current_target out of bounds"). It also polls the
  current target once, so it runs only with the snapshot before the review,
  never while a TAP is in EXTEST (not for *Take over*).

**Kept in EXTEST across sessions:** the project file records the TAPs
(`extest_left`) and the driven image (`extest_images`). With `jtag cur_ir`
the actual instruction decides (records that OpenOCD contradicts are
dropped); without it the record does ("probably in EXTEST").

**Idle:** with nothing to sample, the chain is re-read every 3 s (notices
OpenOCD restarts and chain changes by other clients).

## Icons

The SVGs in `art/` are the sources; the PNGs are committed, so building
needs no SVG tools. After changing an SVG, export again (e.g. with
`rsvg-convert` from librsvg):

```sh
for n in 24 48; do
    rsvg-convert -w $n -h $n art/bscan.svg      -o plugin/icons/bscan-$n.png
    rsvg-convert -w $n -h $n art/bscan-dark.svg -o plugin/icons/bscan-dark-$n.png
done
for n in 16 24 32 48 64 128 256; do
    rsvg-convert -w $n -h $n art/kicad-bscan.svg -o src/gui/icons/kicad-bscan-$n.png
done
# Explorer icon of kicad-bscan.exe (Windows resource), from the PNGs above.
# ImageMagick stores the sizes below 256 as bitmaps; Explorer does not show
# PNG-compressed small sizes everywhere (e.g. details view).
# (ImageMagick 7; with ImageMagick 6 use "convert" instead of "magick")
magick src/gui/icons/kicad-bscan-{16,24,32,48,64,128,256}.png src/gui/icons/kicad-bscan.ico
```

- `bscan.svg`, `bscan-dark.svg`: KiCad toolbar button (light / dark theme).
- `kicad-bscan.svg`: window and taskbar icon (same motif on a light tile);
  the PNGs are embedded at build time (`app_icons.h`); on Windows
  `kicad-bscan.ico` is also the icon of the `.exe` in Explorer
  (`src/gui/kicad-bscan.rc`).

## Debugging

- The plugin's stderr appears in KiCad's status messages (bottom right), but
  only after the process ends.
- KiCad API log: `EnableAPILogging=1` in the `kicad_advanced` file of the
  KiCad configuration folder; the log goes to
  `~/.local/share/KiCad/<version>/logs/api.log`.
- KiCad trace: `KICAD_ENABLE_WXTRACE=1 WXTRACE=KICAD_API kicad`.
- OpenOCD with `-d3` logs every command the plugin sends.
