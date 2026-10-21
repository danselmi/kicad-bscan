// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/interconnect_job.h"

#include <algorithm>

namespace bscan
{

InterconnectJob::InterconnectJob( const SamplerContext& aContext, const InterconnectRequest& aRequest,
                                  const InterconnectCallback& aCallback ) :
        m_ctx( aContext ),
        m_req( aRequest ),
        m_callback( aCallback ),
        m_count( aRequest.taps.size() )
{
}


void InterconnectJob::fail( const std::string& aMessage )
{
    if( m_callback )
        m_callback( std::unexpected( openocd::Error{ openocd::ErrorKind::Command, aMessage } ) );
}


bool InterconnectJob::prepare()
{
    if( m_count == 0 || m_req.patterns.empty() || m_req.devices.size() != m_count
        || m_req.base.size() != m_count )
    {
        fail( "nothing to test" );
        return false;
    }

    if( std::string busy = readPrevious(); !busy.empty() )
    {
        fail( busy + " is entering or leaving EXTEST" );
        return false;
    }

    // Other TAPs in EXTEST are scanned along with their image
    for( ScanTap& t : m_ctx.ExtestAlong( {} ) )
    {
        bool inTest = std::ranges::any_of( m_req.taps,
                                           [&]( const TapInfo& x ) { return x.name == t.tap; } );

        if( !inTest )
            m_along.push_back( std::move( t ) );
    }

    if( !m_ctx.jtagScan && ( m_count > 1 || !m_along.empty() ) )
    {
        fail( "testing several devices together needs OpenOCD's 'jtag scan'" );
        return false;
    }

    m_restore.resize( m_count );

    for( size_t i = 0; i < m_count; ++i )
        m_restore[i] = m_previous[i] ? *m_previous[i] : m_req.base[i];

    registerEntered();
    return true;
}


// State before: devices in EXTEST get their image back at the end.
// Returns a device that is being entered or left (cannot be tested).
std::string InterconnectJob::readPrevious()
{
    ExtestTable::Entries held = m_ctx.extest.All();
    m_previous.assign( m_count, std::nullopt );

    for( size_t i = 0; i < m_count; ++i )
    {
        auto it = held.find( m_req.taps[i].name );

        if( it == held.end() )
            continue;

        if( it->second.phase != ExtestTable::Phase::Active || it->second.lost )
            return m_req.taps[i].name;

        m_previous[i] = it->second.image;
    }

    return {};
}


// Devices entered for the test are registered as in EXTEST, so a lost
// connection leaves them like any other EXTEST
void InterconnectJob::registerEntered()
{
    for( size_t i = 0; i < m_count; ++i )
    {
        if( m_previous[i] )
            continue;

        ExtestTable::Entry e;
        e.tap    = m_req.taps[i];
        e.device = m_req.devices[i];
        e.image  = m_restore[i];
        m_ctx.extest.PutActive( std::move( e ) );
        m_entered.push_back( i );
    }
}


std::optional<uint64_t> InterconnectJob::opcodeOf( size_t aDevice, const char* aInstruction ) const
{
    const BsdlDevice& dev = *m_req.devices[aDevice];

    if( std::string( aInstruction ) == "PRELOAD" && !dev.Opcode( "PRELOAD" ) )
        return dev.Opcode( "SAMPLE" );

    return dev.Opcode( aInstruction );
}


// Without 'jtag scan': one device, nothing else in EXTEST
Result<std::vector<Bits>> InterconnectJob::scanSingle( const Bits& aImage, bool aFirst )
{
    auto asVector = []( Bits b ) { return std::vector<Bits>{ std::move( b ) }; };

    if( aFirst && !m_previous[0] )
        return EnterExtest( m_ctx.client, m_req.taps[0], *m_req.devices[0], aImage ).transform( asVector );

    return ExtestScan( m_ctx.client, m_req.taps[0], *m_req.devices[0], aImage ).transform( asVector );
}


// One scan of all test devices (and the TAPs scanned along). The first scan
// of devices not yet in EXTEST is preceded by a PRELOAD scan.
Result<std::vector<Bits>> InterconnectJob::scanAll( const std::vector<Bits>& aImages, bool aFirst )
{
    if( !m_ctx.jtagScan )
        return scanSingle( aImages[0], aFirst );

    std::vector<ScanTap> scan;

    for( size_t i = 0; i < m_count; ++i )
    {
        auto op = opcodeOf( i, aFirst && !m_previous[i] ? "PRELOAD" : "EXTEST" );

        if( !op )
            return std::unexpected( openocd::Error{ openocd::ErrorKind::Command,
                                                    "BSDL: no EXTEST/PRELOAD instruction" } );

        scan.push_back( { m_req.taps[i].name, *op, aImages[i] } );
    }

    if( aFirst && std::ranges::any_of( m_previous, []( const auto& p ) { return !p.has_value(); } ) )
    {
        std::vector<ScanTap> preload = scan;
        preload.insert( preload.end(), m_along.begin(), m_along.end() );

        if( auto r = ScanTogether( m_ctx.client, preload ); !r )
            return std::unexpected( r.error() );

        for( size_t i = 0; i < m_count; ++i )
            scan[i].opcode = *opcodeOf( i, "EXTEST" );
    }

    scan.insert( scan.end(), m_along.begin(), m_along.end() );
    auto r = ScanTogether( m_ctx.client, scan );

    if( r )
        r->resize( m_count );   // without the TAPs scanned along

    return r;
}


// All patterns plus the restore scan. nullopt: failed (reported, job ends).
std::optional<Captures> InterconnectJob::runOnce( bool aFirst )
{
    const size_t patterns = m_req.patterns.size();
    Captures     captures;

    for( size_t k = 0; k <= patterns; ++k )
    {
        const std::vector<Bits>& images = k < patterns ? m_req.patterns[k] : m_restore;
        auto                     r      = scanAll( images, aFirst && k == 0 );

        if( r )
        {
            if( k > 0 )
                captures.push_back( std::move( *r ) );

            continue;
        }

        if( IsConnectionError( r.error() ) )
        {
            // The entered devices are left after the reconnect like any EXTEST
            for( size_t i : m_entered )
                m_ctx.extest.SetLost( m_req.taps[i].name );

            m_ctx.requestChain();
        }
        else
        {
            leaveEntered();
        }

        if( m_callback )
            m_callback( std::unexpected( r.error() ) );

        return std::nullopt;
    }

    return captures;
}


// Devices entered for the test leave EXTEST (others keep theirs)
void InterconnectJob::leaveEntered()
{
    if( m_entered.empty() )
        return;

    std::string problem;

    if( m_ctx.jtagScan )
    {
        std::vector<ScanTap> scan;

        for( size_t i = 0; i < m_count; ++i )
        {
            auto t = m_previous[i] ? ExtestScanTap( m_req.taps[i], *m_req.devices[i], m_restore[i] )
                                   : SampleScanTap( m_req.taps[i], *m_req.devices[i] );

            if( t )
                scan.push_back( std::move( *t ) );
        }

        scan.insert( scan.end(), m_along.begin(), m_along.end() );

        if( auto r = ScanTogether( m_ctx.client, scan ); !r )
            problem = r.error().message;
    }
    else if( auto r = LeaveExtest( m_ctx.client, m_req.taps[0], *m_req.devices[0] ); !r )
    {
        problem = r.error().message;
    }

    for( size_t i : m_entered )
        m_ctx.extest.Erase( m_req.taps[i].name );

    if( problem.empty() )
        return;

    for( size_t i : m_entered )
        m_ctx.Report( m_req.taps[i].name, ExtestUpdate::State::Left,
                      "could not leave EXTEST after the interconnect test (" + problem
                              + "). The device may still be in EXTEST: reset the board." );
}


void InterconnectJob::Run( const std::function<bool()>& aNextRun )
{
    if( !prepare() )
        return;

    for( bool first = true;; first = false )
    {
        std::optional<Captures> captures = runOnce( first );

        if( !captures )
            return;

        if( m_callback )
            m_callback( *captures );

        if( !m_req.repeat || !aNextRun() )
            break;
    }

    leaveEntered();
}

} // namespace bscan
