// SPDX-License-Identifier: GPL-3.0-only
// The interconnect test between the GUI and the sampler: the current plan,
// the run in progress (with the plan it was started with) and the fault
// statistics since it was started (Repeat counts the faults per net).
//
// No GUI: the window shows what this class holds and passes the request to
// LiveSampler::RunInterconnect and its captures back to AddRun().

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "bscan/interconnect.h"
#include "bscan/sampler_types.h"

namespace bscan
{

// Devices of aPlan with at least one tested net (they are driven by a run),
// as indices into aPlan.devices, in device order
std::vector<size_t> UsedDevices( const InterconnectPlan& aPlan );

class InterconnectSession
{
public:
    struct NetStat
    {
        int         fails = 0;
        std::string lastFail;            // NetResult::text of the last fault
        bool        failingNow = false;  // in the latest run
    };

    struct RunSummary
    {
        int    runs       = 0;
        size_t tested     = 0;   // nets
        int    failing    = 0;   // nets that failed at least once
        int    failingNow = 0;   // nets that failed in the latest run
    };

    // The plan for the next run (shown while no test runs)
    void                    SetPlan( InterconnectPlan aPlan ) { m_plan = std::move( aPlan ); }
    const InterconnectPlan& Plan() const { return m_plan; }

    // Starts a run with the current plan: the request for the sampler
    // (aChain: the TAPs as OpenOCD reports them). Resets the statistics.
    // nullopt if a device of the plan is not in aChain.
    std::optional<InterconnectRequest> Start( const std::vector<TapInfo>& aChain, bool aRepeat );

    // The captures of one run (devices in request order). Evaluates them and
    // counts the faults per net.
    RunSummary AddRun( const Captures& aCaptures );

    // The run ended (stopped, failed, or a single run done)
    void Stop() { m_running = false; }

    bool Running() const { return m_running; }
    bool Repeating() const { return m_running && m_repeat; }
    int  Runs() const { return m_runs; }

    // Statistics of a net since the start; nullptr before its first result
    const NetStat* Stat( const std::string& aNet ) const;

private:
    InterconnectPlan               m_plan;      // current
    InterconnectPlan               m_runPlan;   // of the run in progress / the last run
    std::vector<size_t>            m_used;      // devices of m_runPlan in the request
    std::map<std::string, NetStat> m_stats;
    int                            m_runs    = 0;
    bool                           m_running = false;
    bool                           m_repeat  = false;
};

} // namespace bscan
