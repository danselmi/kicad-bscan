// SPDX-License-Identifier: GPL-3.0-only
// Carries out the EXTEST requests of LiveSampler in the sampler thread:
// enter, take over (one TAP or several in one scan), leave, notice
// interruptions, shift changed images. The outcome is recorded in the
// ExtestTable and reported through the context.
//
// Without 'jtag scan' only one TAP can be in EXTEST: every scan of one TAP
// puts all others into BYPASS (OpenOCD), which would end EXTEST there.

#pragma once

#include <vector>

#include "bscan/sampler_context.h"

namespace bscan
{

class ExtestJob
{
public:
    explicit ExtestJob( const SamplerContext& aContext );

    // Pending enter / take-over / leave requests. aReconnected: the
    // connection was lost and is back; EXTEST is never resumed then: the
    // TAPs that were active are left.
    void ProcessRequests( bool aReconnected );

    // 'jtag cur_ir' of the active TAPs: something else changed the
    // instruction? Then EXTEST and the image are shifted again in this
    // cycle (marked dirty) and the interruption is reported.
    void CheckInterruptions();

    // Without 'jtag scan': shifts changed images of TAPs that are not
    // sampled (sampling shifts the others). With 'jtag scan' the combined
    // sample scan does that.
    void ShiftChangedImages( const std::vector<SampleTarget>& aTargets );

    // The sampler is stopping: leave EXTEST everywhere, best effort
    void LeaveAll();

private:
    // Several pending take-overs in one scan (all or none)
    void        takeOverTogether();
    std::string checkAllInExtest( const ExtestTable::Entries& aPending ) const;
    // Without 'jtag scan': refuses aName if another TAP is held
    bool refusedAsSecond( const std::string& aName, const ExtestTable::Entry& aEntry );

    void enter( const std::string& aName, const ExtestTable::Entry& aEntry );
    void takeOver( const std::string& aName, const ExtestTable::Entry& aEntry );
    void leave( const std::string& aName, const ExtestTable::Entry& aEntry );

    const SamplerContext& m_ctx;
};

} // namespace bscan
