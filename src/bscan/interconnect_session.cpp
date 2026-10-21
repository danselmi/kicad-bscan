// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/interconnect_session.h"

#include <algorithm>

namespace bscan
{

std::vector<size_t> UsedDevices( const InterconnectPlan& aPlan )
{
    std::vector<size_t> used;

    for( size_t d = 0; d < aPlan.devices.size(); ++d )
    {
        bool driven = std::ranges::any_of(
                aPlan.nets,
                [&]( const TestNet& n )
                {
                    return n.status == TestNet::Status::Tested
                           && std::ranges::any_of( n.bsPins, [&]( const NetPin& p )
                                                   { return p.device == static_cast<int>( d ); } );
                } );

        if( driven )
            used.push_back( d );
    }

    return used;
}


std::optional<InterconnectRequest> InterconnectSession::Start( const std::vector<TapInfo>& aChain,
                                                                bool aRepeat )
{
    std::vector<size_t> used = UsedDevices( m_plan );
    InterconnectRequest req;
    req.repeat = aRepeat;

    for( size_t d : used )
    {
        const IcDevice& dev = m_plan.devices[d];
        auto tap = std::ranges::find_if( aChain, [&]( const TapInfo& t ) { return t.name == dev.tap; } );

        if( tap == aChain.end() )
            return std::nullopt;

        req.taps.push_back( *tap );
        req.devices.push_back( dev.device );
        req.base.push_back( dev.baseImage );
    }

    for( const std::vector<Bits>& pattern : m_plan.patterns )
    {
        std::vector<Bits> images;

        for( size_t d : used )
            images.push_back( pattern[d] );

        req.patterns.push_back( std::move( images ) );
    }

    m_runPlan = m_plan;
    m_used    = std::move( used );
    m_stats.clear();
    m_runs    = 0;
    m_running = true;
    m_repeat  = aRepeat;
    return req;
}


InterconnectSession::RunSummary InterconnectSession::AddRun( const Captures& aCaptures )
{
    // Captures of the used devices -> per device of the plan
    Captures full;

    for( const std::vector<Bits>& pattern : aCaptures )
    {
        std::vector<Bits> images( m_runPlan.devices.size() );

        for( size_t i = 0; i < m_used.size() && i < pattern.size(); ++i )
            images[m_used[i]] = pattern[i];

        full.push_back( std::move( images ) );
    }

    RunSummary sum;
    sum.runs   = ++m_runs;
    sum.tested = m_runPlan.TestedCount();

    for( const NetResult& r : EvaluateInterconnect( m_runPlan, full ) )
    {
        NetStat& s   = m_stats[r.net];
        s.failingNow = r.kind != NetResult::Kind::Ok;

        if( s.failingNow )
        {
            ++s.fails;
            s.lastFail = r.text;
            ++sum.failingNow;
        }

        sum.failing += s.fails > 0 ? 1 : 0;
    }

    if( !m_repeat )
        m_running = false;

    return sum;
}


const InterconnectSession::NetStat* InterconnectSession::Stat( const std::string& aNet ) const
{
    if( m_runs == 0 )
        return nullptr;

    auto it = m_stats.find( aNet );
    return it != m_stats.end() ? &it->second : nullptr;
}

} // namespace bscan
