// SPDX-License-Identifier: GPL-3.0-only
// JTAG chain as configured in OpenOCD, and boundary-scan operations on it.

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bscan/bits.h"
#include "bscan/bsdl_device.h"
#include "openocd/tcl_client.h"

namespace bscan
{

using openocd::Result;

// One TAP as reported by OpenOCD's 'scan_chain', in chain order (TDO first).
struct TapInfo
{
    int         index = 0;
    std::string name;          // e.g. "stm32f4x.bs"
    bool        enabled = true;
    uint32_t    idcode = 0;    // 0: device has no IDCODE / was not read
    int         irLength = 0;

    // All -expected-id values of the OpenOCD configuration (may be empty)
    std::vector<uint32_t> expectedIds;
    bool                  ignoreVersion = false;   // -ignore-version: bits 31..28 ignored

    // Instruction register as last shifted by OpenOCD ('jtag cur_ir', only in
    // OpenOCD versions that have it; nullopt otherwise or if > 64 bits)
    std::optional<uint64_t> curIr;

    // True if no expected IDs are configured or aIdcode matches one of them
    bool MatchesExpected( uint32_t aIdcode ) const;

    bool operator==( const TapInfo& ) const = default;
};

// Parses the text table printed by 'scan_chain'.
std::optional<std::vector<TapInfo>> ParseScanChain( const std::string& aText );

// 'scan_chain', plus 'jtag cur_ir' per TAP if OpenOCD has that command
Result<std::vector<TapInfo>> ReadChain( openocd::TclClient& aClient );

// Instruction OpenOCD last shifted into aTap ('jtag cur_ir'). OpenOCD also
// records the BYPASS it shifts into a TAP while scanning another one, so this
// shows any JTAG activity on the TAP since our last scan. nullopt if OpenOCD
// does not have the command; a command error (e.g. disabled TAP) or a reply
// without 0x prefix is an error.
Result<std::optional<uint64_t>> ReadCurIr( openocd::TclClient& aClient, const std::string& aTap );

// True if OpenOCD polls targets in the background ('poll' is on and there is
// at least one target). Every poll scans the target's TAP and puts all other
// TAPs into BYPASS.
// Calls 'poll', which also polls the current target once: never use it while
// a device is in EXTEST. Without any target it is not called ('poll' without
// a target makes OpenOCD exit).
Result<bool> BackgroundPolling( openocd::TclClient& aClient );

// Result of comparing a TAP with a BSDL file
struct TapCheck
{
    bool        ok = true;               // false if anything does not match
    std::vector<std::string> messages;   // human-readable findings
};

TapCheck CheckTap( const TapInfo& aTap, const BsdlDevice& aDevice );

// One TAP of a combined scan: instruction and data register content
struct ScanTap
{
    std::string tap;
    uint64_t    opcode = 0;
    Bits        data;
};

// Loads SAMPLE into aTap (all other TAPs go to BYPASS) and shifts the
// boundary register. The safe values from the BSDL file are shifted in;
// under SAMPLE they only reach the update latches, not the pins.
// The instruction is set before every scan, so the scan stays correct even if
// another client changed the IR in between.
//
// aAlong (all functions below): other TAPs scanned in the same 'jtag scan',
// e.g. devices kept in EXTEST with their image; without it they would be put
// into BYPASS. Needs 'jtag scan' (error otherwise); their captured data is
// discarded.
Result<Bits> Sample( openocd::TclClient& aClient, const TapInfo& aTap, const BsdlDevice& aDevice,
                     const std::vector<ScanTap>& aAlong = {} );

// --- EXTEST (the boundary register drives the pins) ---

// PRELOAD (or SAMPLE) with aImage, then EXTEST, in one Tcl command.
// Returns the captured pin values.
Result<Bits> EnterExtest( openocd::TclClient& aClient, const TapInfo& aTap,
                          const BsdlDevice& aDevice, const Bits& aImage,
                          const std::vector<ScanTap>& aAlong = {} );

// While in EXTEST: drive aImage and capture the pins
Result<Bits> ExtestScan( openocd::TclClient& aClient, const TapInfo& aTap,
                         const BsdlDevice& aDevice, const Bits& aImage,
                         const std::vector<ScanTap>& aAlong = {} );

// Back to the device's own logic (loads SAMPLE)
Result<void> LeaveExtest( openocd::TclClient& aClient, const TapInfo& aTap,
                          const BsdlDevice& aDevice, const std::vector<ScanTap>& aAlong = {} );

// --- Several TAPs in one scan ('jtag scan' only) ---


// SAMPLE (safe values shifted in) / EXTEST with aImage, for ScanTogether()
// or as aAlong; checked like Sample() / ExtestScan()
Result<ScanTap> SampleScanTap( const TapInfo& aTap, const BsdlDevice& aDevice );
Result<ScanTap> ExtestScanTap( const TapInfo& aTap, const BsdlDevice& aDevice, const Bits& aImage );

// All aTaps in one 'jtag scan' (the instruction is always shifted); TAPs
// not listed go to BYPASS. Returns the captured data, in the order of aTaps.
Result<std::vector<Bits>> ScanTogether( openocd::TclClient& aClient,
                                        const std::vector<ScanTap>& aTaps );

// State of one pin taken from a captured boundary register
struct PinValue
{
    enum class Output
    {
        None,      // pin has no output cell
        HighZ,     // output disabled by its control cell
        Drive0,
        Drive1
    };

    std::optional<bool> input;   // value at the input cell, if any
    Output              output = Output::None;
};

PinValue DecodePin( const Bits& aBoundary, const PinCells& aCells );

const char* ToString( PinValue::Output aOutput );

} // namespace bscan
