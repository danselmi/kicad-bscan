// SPDX-License-Identifier: GPL-3.0-only
// What the jobs of LiveSampler's thread share during one cycle: the
// connection to OpenOCD, the EXTEST table and the way back to LiveSampler.

#pragma once

#include <functional>
#include <string>
#include <vector>

#include "bscan/extest_table.h"
#include "bscan/sampler_types.h"

namespace bscan
{

struct SamplerContext
{
    openocd::TclClient&   client;
    bool                  jtagScan;       // several TAPs in one scan possible
    ExtestTable&          extest;
    const ExtestCallback& onExtest;       // may be empty
    std::function<void()> requestChain;   // re-read the chain next cycle

    void Report( const std::string& aTap, ExtestUpdate::State aState,
                 const std::string& aMessage, int aInterruptions = 0 ) const
    {
        if( onExtest )
            onExtest( ExtestUpdate{ aTap, aState, aMessage, aInterruptions } );
    }

    // With 'jtag scan': the active EXTEST TAPs except aExcept, to be scanned
    // along (keeps them in EXTEST); empty without 'jtag scan'
    std::vector<ScanTap> ExtestAlong( const std::string& aExcept ) const
    {
        std::vector<ScanTap> along;

        if( !jtagScan )
            return along;

        for( const auto& [name, e] : extest.Active() )
        {
            if( name == aExcept )
                continue;

            if( auto t = ExtestScanTap( e.tap, *e.device, e.image ) )
                along.push_back( std::move( *t ) );
        }

        return along;
    }
};

// Not an error reply of a command: the connection is gone (or timed out)
inline bool IsConnectionError( const openocd::Error& aError )
{
    return aError.kind != openocd::ErrorKind::Command;
}

} // namespace bscan
