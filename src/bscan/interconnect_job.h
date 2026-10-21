// SPDX-License-Identifier: GPL-3.0-only
// Applies a list of boundary-scan images to several devices and returns
// what they captured (LiveSampler::RunInterconnect). It knows nothing about
// nets: the interconnect test (interconnect.h) builds the patterns and
// evaluates the captures.
//
//  - Devices not in EXTEST enter it (PRELOAD with the first pattern, then
//    EXTEST) and leave it at the end; devices already in EXTEST get their
//    image back. Other TAPs in EXTEST are scanned along.
//  - Scan k shifts pattern k and captures what pattern k-1 drove; one more
//    scan (the restore image) captures the last pattern.
//  - Several devices need 'jtag scan'.
//  - Connection lost: the entered devices stay registered (lost) and are
//    left after the reconnect like any EXTEST.

#pragma once

#include <functional>
#include <optional>
#include <vector>

#include "bscan/sampler_context.h"

namespace bscan
{

class InterconnectJob
{
public:
    InterconnectJob( const SamplerContext& aContext, const InterconnectRequest& aRequest,
                     const InterconnectCallback& aCallback );

    // Runs the patterns once, or (aRequest.repeat) until aNextRun() returns
    // false; aNextRun waits between the runs. One callback per run; an error
    // ends the job.
    void Run( const std::function<bool()>& aNextRun );

private:
    // Checks the request and the EXTEST state, registers the entered devices.
    // False: refused (already reported).
    bool        prepare();
    std::string readPrevious();
    void        registerEntered();

    std::optional<Captures> runOnce( bool aFirst );
    Result<std::vector<Bits>> scanAll( const std::vector<Bits>& aImages, bool aFirst );
    Result<std::vector<Bits>> scanSingle( const Bits& aImage, bool aFirst );
    std::optional<uint64_t>   opcodeOf( size_t aDevice, const char* aInstruction ) const;

    void leaveEntered();
    void fail( const std::string& aMessage );

    const SamplerContext&       m_ctx;
    const InterconnectRequest&  m_req;
    const InterconnectCallback& m_callback;
    size_t                      m_count = 0;   // devices

    std::vector<std::optional<Bits>> m_previous;   // image of devices already in EXTEST
    std::vector<Bits>                m_restore;    // per device, after the test
    std::vector<size_t>              m_entered;    // devices entered for the test
    std::vector<ScanTap>             m_along;      // other TAPs in EXTEST
};

} // namespace bscan
