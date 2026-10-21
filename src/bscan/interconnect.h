// SPDX-License-Identifier: GPL-3.0-only
// Interconnect test: shorts and opens between boundary-scan pins.
//
// Every tested net has one driver (a boundary-scan pin that can drive) and
// one or more receivers (boundary-scan pins with an input cell). Each tested
// net gets a code word; pattern p drives bit p of the code on every net at
// the same time. The true/complement counting sequence (counting codes,
// then their complement) needs 2 * (ceil(log2(N + 1)) + 1) patterns for N
// nets. The codes are 1 .. 2^(w-1) - 1: never all-0 or all-1 (stuck-at
// faults are visible) and never the complement of another code (a short
// cannot look like a stuck net).
//
// Receivers that read another code than their net's show the fault:
//   - wired AND / OR of the net's code with another net's code: short,
//   - constant 0 or 1: stuck (open with a pull, or short to a supply),
//   - only some receivers of a net wrong: open at those pins.
//
// Nets that are not tested are reported with the reason (fewer than two
// boundary-scan pins, other parts on the net and not included, fixed by a
// starting state, no pin that can drive, shared output enable).

#pragma once

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "bscan/bits.h"
#include "bscan/bsdl_device.h"
#include "bscan/extest.h"

namespace bscan
{

// A JTAG device taking part in the test
struct IcDevice
{
    std::string                       tap;
    std::string                       reference;   // KiCad reference designator
    std::shared_ptr<const BsdlDevice> device;
    std::string                       pinMap;      // empty: BSDL default

    // Pins that keep their state; their nets are not tested: the starting
    // states set by the user (EXTEST setup, only explicit settings, not
    // Safe), and for a device already in EXTEST the pins driven otherwise
    // than their starting state (set by hand), listed in currentState too.
    std::map<std::string, PinDrive> constraints;

    // Image for everything outside the tested nets (constraints applied)
    Bits baseImage;

    std::set<std::string> currentState;   // see constraints
};

// Pads of the board: reference -> pad number -> net name ("" = no net)
using PadNets = std::map<std::string, std::map<std::string, std::string>>;

struct NetPin
{
    std::string reference;
    std::string pad;
    int         device = -1;   // index into InterconnectPlan::devices
    PinCells    cells;
};

struct TestNet
{
    enum class Status
    {
        Tested,
        FewPins,        // fewer than two boundary-scan pins
        OtherParts,     // other parts on the net, not included
        Fixed,          // pins have a starting state (constraint): fixedBy
        NoDriver,       // no pin can drive, or no pin can receive
        SharedEnable    // the driver's output enable is shared with another pin
    };

    std::string              name;
    std::vector<NetPin>      bsPins;        // boundary-scan pins of the test devices
    std::vector<std::string> otherParts;    // references of other parts, sorted
    Status                   status = Status::FewPins;
    std::string              reason;        // for status != Tested
    std::vector<size_t>      fixedBy;       // Fixed: bsPins with a starting state
    int                      driver = -1;   // index into bsPins
    unsigned long long       code   = 0;    // Tested: the net's code word

    bool Mixed() const { return !otherParts.empty(); }
};

struct InterconnectPlan
{
    std::vector<IcDevice> devices;
    std::vector<TestNet>  nets;              // all nets with a boundary-scan pin, by name
    int                   width = 0;         // code bits; patterns = 2 * width

    // [pattern][device]: images to shift (devices without a tested net keep
    // their base image)
    std::vector<std::vector<Bits>> patterns;

    size_t TestedCount() const;
};

// aIncluded: nets with other parts that the user included.
InterconnectPlan PlanInterconnect( std::vector<IcDevice> aDevices, const PadNets& aPadNets,
                                   const std::set<std::string>& aIncluded );

struct NetResult
{
    enum class Kind
    {
        Ok,
        Short,     // with the nets in shortWith
        Open,      // at the pins in pins
        Stuck0,    // all receivers read 0: open at the driver or short to GND
        Stuck1,    // all receivers read 1: open at the driver or short to a supply
        Wrong      // something else (pins: receivers that read wrong values)
    };

    std::string              net;
    Kind                     kind = Kind::Ok;
    std::vector<std::string> shortWith;   // net names
    std::vector<std::string> pins;        // "U3 pin 12"
    std::string              text;        // human-readable result
};

// aCaptured[pattern][device]: the boundary register captured while pattern
// was applied (i.e. by the scan after the one that shifted it). Results for
// the tested nets, in plan order.
std::vector<NetResult> EvaluateInterconnect( const InterconnectPlan& aPlan,
                                             const std::vector<std::vector<Bits>>& aCaptured );

// What the interconnect test does with one boundary-scan pin of a device
struct PinTestRole
{
    enum class Kind
    {
        Tested,      // its net is tested
        FixedHere,   // its starting state keeps its net out of the test
        NotTested    // its net is not tested for another reason (see reason)
    };

    std::string net;
    Kind        kind = Kind::NotTested;
    bool        currentState = false;   // FixedHere: by its current EXTEST state, not a starting state
    std::string reason;                 // the net's reason (TestNet::reason)
};

// Roles of the boundary-scan pins of aPlan.devices[aDevice], by pad
std::map<std::string, PinTestRole> PinTestRoles( const InterconnectPlan& aPlan, int aDevice );

const char* ToString( TestNet::Status aStatus );

} // namespace bscan
