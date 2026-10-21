// SPDX-License-Identifier: GPL-3.0-only
// What LiveSampler takes and reports. Also used by its jobs in the sampler
// thread (sample_job.h, extest_job.h, interconnect_job.h).

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "bscan/chain.h"

namespace bscan
{

// A pin to sample
struct SampleTarget
{
    std::string                       key;    // caller's id, returned in results
    TapInfo                           tap;
    std::shared_ptr<const BsdlDevice> device;
    std::string                       pin;    // physical pin, for display
    PinCells                          cells;
};

struct ChainUpdate
{
    bool                 connected = false;
    bool                 jtagScan  = false;   // several TAPs in EXTEST possible
    std::vector<TapInfo> taps;
    std::string          error;      // set if !connected

    bool operator==( const ChainUpdate& ) const = default;
};

struct PinResult
{
    std::string key;
    std::string tap;
    bool        ok = false;
    PinValue    value;
    std::string error;       // set if !ok

    bool operator==( const PinResult& a ) const
    {
        return key == a.key && tap == a.tap && ok == a.ok && error == a.error
               && value.input == a.value.input && value.output == a.value.output;
    }
};

// Results for all targets, in the order of LiveSampler::SetTargets()
struct SampleUpdate
{
    unsigned               targetsVersion = 0;
    std::vector<PinResult> pins;

    bool operator==( const SampleUpdate& ) const = default;
};

struct ExtestUpdate
{
    enum class State
    {
        Active,       // EXTEST entered (or image applied)
        Left,         // back in SAMPLE
        Failed,       // entering or leaving failed; see message
        Interrupted   // something else changed the instruction; EXTEST is
                      // being restored. message: the instruction found
    };

    std::string tap;
    State       state = State::Failed;
    std::string message;
    int         interruptions = 0;   // Interrupted: count since entering
};

// What LiveSampler::RequestSnapshot() delivers
struct Snapshot
{
    std::vector<Bits> samples;
    bool              backgroundPolling = false;   // see bscan::BackgroundPolling
};

// One TAP of LiveSampler::TakeOverExtest( vector )
struct TakeOver
{
    TapInfo                           tap;
    std::shared_ptr<const BsdlDevice> device;
    Bits                              image;
};

// Patterns to apply (see LiveSampler::RunInterconnect)
struct InterconnectRequest
{
    std::vector<TapInfo>                           taps;      // per device
    std::vector<std::shared_ptr<const BsdlDevice>> devices;
    std::vector<Bits>                              base;      // per device
    std::vector<std::vector<Bits>>                 patterns;  // [pattern][device]
    bool                                           repeat = false;
};

// [pattern][device]: captured while the pattern was applied
using Captures = std::vector<std::vector<Bits>>;

using ChainCallback        = std::function<void( const ChainUpdate& )>;
using SampleCallback       = std::function<void( const SampleUpdate& )>;
using ExtestCallback       = std::function<void( const ExtestUpdate& )>;
using SnapshotCallback     = std::function<void( const Result<Snapshot>& )>;
using InterconnectCallback = std::function<void( const Result<Captures>& )>;

} // namespace bscan
