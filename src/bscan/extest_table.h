// SPDX-License-Identifier: GPL-3.0-only
// The TAPs that LiveSampler holds in EXTEST (or is entering, taking over or
// leaving), by TAP name. Thread-safe: the GUI side requests changes, the
// sampler thread carries them out and records the outcome.

#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "bscan/chain.h"

namespace bscan
{

class ExtestTable
{
public:
    struct Entry
    {
        enum class Phase
        {
            PendingEnter,
            PendingTakeOver,
            Active,
            PendingLeave
        };

        TapInfo                           tap;
        std::shared_ptr<const BsdlDevice> device;
        Bits                              image;
        Phase                             phase = Phase::PendingEnter;
        bool                              dirty = false;   // image changed, not yet shifted
        bool                              lost  = false;   // connection lost while active
        int                               interruptions = 0;
    };

    using Entries = std::map<std::string, Entry>;
    using Phase   = Entry::Phase;

    // --- requests (GUI side) ---
    void RequestEnter( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                       const Bits& aImage );
    void RequestTakeOver( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                          const Bits& aImage );
    // Leaving a TAP this sampler did not enter
    void RequestForceLeave( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice );
    // False if the TAP is not (or no longer) held
    bool RequestImage( const std::string& aTap, const Bits& aImage );

    enum class LeaveRequest
    {
        Unknown,     // not held
        Cancelled,   // was only about to be entered: removed, never entered
        Pending      // will be left by the sampler thread
    };

    LeaveRequest RequestLeave( const std::string& aTap );

    // --- sampler thread ---
    Entries All() const;
    Entries InPhase( Phase aPhase ) const;
    Entries Active() const;   // Active and not lost: in EXTEST, scanned with their image

    // The first other TAP that is in EXTEST or being left; empty if none
    std::string OtherHeld( const std::string& aTap ) const;

    bool Contains( const std::string& aTap ) const;
    bool Empty() const;
    // Something to do soon: a pending phase or an image not yet shifted
    bool WorkPending() const;

    // Outcome of entering / taking over aTap (from aPending): Active, or
    // removed if it failed. False if the entry is gone (cancelled meanwhile).
    bool Settle( const std::string& aTap, Phase aPending, bool aOk );
    // The same, but only if aTap is still in aPending (otherwise unchanged)
    void SettleIfPending( const std::string& aTap, Phase aPending, bool aOk );

    // Registers aEntry as Active (TAPs entered by the interconnect test)
    void PutActive( Entry aEntry );
    void Erase( const std::string& aTap );
    void SetLost( const std::string& aTap );

    // Connection lost: all active entries are marked lost; after the
    // reconnect they are left (EXTEST is never resumed)
    void MarkActiveLost();
    void LeaveLost();

    // The image was shifted: not dirty any more, unless it changed meanwhile
    void Shifted( const std::string& aTap, const Bits& aImage );
    bool Dirty( const std::string& aTap ) const;

    // Something else changed the instruction: shift EXTEST and the image
    // again. Returns the interruption count, nullopt if not active.
    std::optional<int> Interrupted( const std::string& aTap );

    // Removes and returns all entries (sampler stopping)
    Entries TakeAll();

private:
    mutable std::mutex m_mutex;
    Entries            m_entries;
};

} // namespace bscan
