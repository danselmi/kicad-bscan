// SPDX-License-Identifier: GPL-3.0-only
// EXTEST planning: which value the boundary register gets for every cell.
//
// In EXTEST the boundary register drives ALL pins of the device. The image to
// load is built from
//   - the BSDL safe values (default for every cell),
//   - a starting state per output pin chosen by the user (Z / 0 / 1 / keep),
//   - one or more SAMPLE captures (for "keep" and to show what changes).
//
// Several pins can share one control (output enable) cell; settings that need
// different values in such a cell are reported as conflicts.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "bscan/bits.h"
#include "bscan/bsdl_device.h"
#include "bscan/chain.h"

namespace bscan
{

// Instructions that disconnect the device's own logic from its pins:
// EXTEST, EXTEST_PULSE, EXTEST_TRAIN, CLAMP, HIGHZ, INTEST (case-insensitive)
bool IsPinControlInstruction( const std::string& aName );

// Name of the pin-controlling instruction aCurIr selects on aDevice, or
// empty if it selects none (e.g. BYPASS, IDCODE, SAMPLE)
std::string PinControlInstruction( const BsdlDevice& aDevice, uint64_t aCurIr );

// "BYPASS (0x3f)", or just "0x3f" if the BSDL file has no name for it
std::string DescribeInstruction( const BsdlDevice& aDevice, uint64_t aValue );

// Starting state of an output pin, as chosen by the user
enum class PinDrive
{
    Safe,     // BSDL safe values of the pin's cells (default)
    HighZ,    // output off
    Low,
    High,
    Keep      // what the device drives at the moment of activation
};

const char*             ToString( PinDrive aDrive );       // "safe", "z", "0", "1", "keep"
std::optional<PinDrive> PinDriveFromString( const std::string& aText );

// State of an output pin, observed or planned
enum class PinState
{
    Unknown,
    HighZ,
    Low,
    High,
    Toggling   // differed between the samples
};

const char* ToString( PinState aState );

// All pins of a pin map that have an output cell, in pin map order
std::vector<OutputPin> OutputPins( const BsdlDevice& aDevice, const std::string& aPinMap );

struct PinPlan
{
    std::string pin;
    std::string port;
    PinDrive    drive = PinDrive::Safe;     // setting used
    bool        explicitSetting = false;    // set by the user (not the default)
    PinState    current = PinState::Unknown;
    PinState    planned = PinState::Unknown;

    bool Changes() const { return current == PinState::Toggling || current != planned; }
};

struct ExtestPlan
{
    Bits                     image;      // boundary register content to load
    std::vector<PinPlan>     pins;       // every output pin
    std::vector<std::string> conflicts;  // problems; activation should be refused
};

// aSamples: SAMPLE captures taken just before (may be empty: current unknown,
// "keep" impossible). aSettings: pin -> drive; missing pins use Safe.
ExtestPlan PlanExtest( const BsdlDevice& aDevice, const std::string& aPinMap,
                       const std::vector<Bits>& aSamples,
                       const std::map<std::string, PinDrive>& aSettings );

// Settings that refer to pins which are not output pins of this pin map
// (e.g. saved for another BSDL file or package). They are ignored by
// PlanExtest(); the caller should tell the user. Sorted.
std::vector<std::string> StaleSettings( const BsdlDevice& aDevice, const std::string& aPinMap,
                                        const std::map<std::string, PinDrive>& aSettings );

// State of one output pin as given by the cells of an image (what is driven)
PinState StateFromImage( const Bits& aImage, const PinCells& aCells );

// Changes one pin in an image that is already active. Returns the other pins
// whose output enable changes as a side effect (shared control cell).
// aDrive must be HighZ, Low or High. Returns nullopt if the pin cannot take
// the state (e.g. Z without a control cell).
struct PinChange
{
    Bits                     image;
    std::vector<std::string> alsoAffected;
};

std::optional<PinChange> SetPin( const BsdlDevice& aDevice, const std::string& aPinMap,
                                 const Bits& aImage, const std::string& aPin, PinDrive aDrive );

// Driving several pins at once (devices already in EXTEST): the new image
// per device, what cannot be done, and what changes as a side effect.
// Nothing is applied; the caller asks the user first.
struct DriveDevice
{
    const BsdlDevice* device = nullptr;
    std::string       pinMap;
    Bits              image;   // driven now
    std::string       label;   // for alsoAffected, e.g. the reference
};

struct DriveRequest
{
    std::string tap;    // device in EXTEST; empty: its device is not in EXTEST
    std::string pin;    // physical pin
    std::string name;   // for skipped, e.g. "U3 12"
};

struct DrivePlan
{
    enum class SkipReason
    {
        NotInExtest,
        NoOutputEnable,   // Z requested, the pin cannot be switched off
        NoOutput          // 0 / 1 requested, the pin cannot drive
    };

    struct Skipped
    {
        std::string name;
        SkipReason  reason;
    };

    std::map<std::string, Bits> images;    // by TAP: the new image (only TAPs with a pin set)
    size_t                      applied = 0;
    std::vector<Skipped>        skipped;
    // Pins not requested whose output state changes too (shared output
    // enable), "label pin", each once
    std::vector<std::string> alsoAffected;
};

DrivePlan PlanDrive( const std::map<std::string, DriveDevice>& aDevices,
                     const std::vector<DriveRequest>& aPins, PinDrive aDrive );

// Takes over a TAP that is already in EXTEST (left so by an earlier session):
// drives aImage without leaving EXTEST in between (no PRELOAD/SAMPLE, so the
// pins never return to the device's own logic). Refused unless OpenOCD
// reports EXTEST as the current instruction ('jtag cur_ir'), since entering
// EXTEST this way would first drive whatever the update latches hold.
// Returns the captured data like ExtestScan().
// aAlong: see Sample().
Result<Bits> TakeOverExtest( openocd::TclClient& aClient, const TapInfo& aTap,
                             const BsdlDevice& aDevice, const Bits& aImage,
                             const std::vector<ScanTap>& aAlong = {} );

// Decoding while EXTEST is active: input from the captured data, output from
// the image that is being driven (an output cell's capture shows the device's
// internal logic, not the pin).
PinValue DecodeExtest( const Bits& aCaptured, const Bits& aImage, const PinCells& aCells );

} // namespace bscan
