// SPDX-License-Identifier: GPL-3.0-only
// The status of one TAP as the chain table shows it: the check result (BSDL
// file, IDCODE, reference, EXTEST) and what can be done (EXTEST, take over,
// look for a BSDL file). Decided from facts the window collects.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "bscan/bsdl_device.h"
#include "bscan/chain.h"

namespace bscan
{

struct TapFacts
{
    bool present   = true;    // in OpenOCD's chain (false: only in the settings)
    bool connected = false;   // OpenOCD is connected
    bool assigned  = false;   // a BSDL file is assigned

    const BsdlDevice* device = nullptr;   // the assigned file, read (nullptr: none or error)
    uint32_t assignedIdcode  = 0;         // IDCODE when the file was assigned (0: unknown)

    bool referenceUnknown = false;   // a reference is set, but it is not on the board
    bool inExtest         = false;   // this window: in EXTEST or entering it
    bool held             = false;   // in EXTEST (or CLAMP, ...) without this window
    std::optional<std::string> heldInstruction;   // held: as OpenOCD reports it
    bool        canTakeOver = false;
    std::string elsewhere;   // another TAP in EXTEST that blocks this one, empty if none
};

struct TapStatus
{
    enum class Check
    {
        Bypass,             // no BSDL file
        BsdlError,          // the file cannot be read
        NotInChain,         // connected, but OpenOCD does not have this TAP
        Offline,            // not connected
        Mismatch,           // IR length / IDCODE do not fit the file
        IdcodeChanged,      // other IDCODE than when the file was assigned
        UnknownReference,   // the reference is not on the board
        Ok,
        Extest,             // in EXTEST by this window
        Held                // held: see TapFacts::heldInstruction
    };

    Check check         = Check::Bypass;
    bool  problem       = false;   // shown as a problem
    bool  extestAllowed = false;   // the EXTEST button works
    bool  takeOver      = false;   // ... and takes the held TAP over
    bool  blocked       = false;   // EXTEST impossible because of TapFacts::elsewhere
    bool  lookForBsdl   = false;   // matching BSDL files are worth offering
};

TapStatus EvaluateTap( const TapInfo& aTap, const TapFacts& aFacts );

} // namespace bscan
