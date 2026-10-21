// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/live_sampler.h"

#include "bscan/extest.h"
#include "bscan/extest_job.h"
#include "bscan/interconnect_job.h"
#include "bscan/sample_job.h"

#include <algorithm>

namespace bscan
{

// While no pin is watched, the chain is re-read this often (connection check)
static constexpr auto IDLE_CHAIN_CHECK = std::chrono::seconds( 3 );

// Retry interval while OpenOCD is not reachable
static constexpr auto RECONNECT_DELAY = std::chrono::seconds( 2 );


LiveSampler::LiveSampler() = default;


LiveSampler::~LiveSampler()
{
    Stop();
}


void LiveSampler::Start( ChainCallback aOnChain, SampleCallback aOnSample )
{
    Stop();
    m_onChain  = std::move( aOnChain );
    m_onSample = std::move( aOnSample );

    {
        std::lock_guard lock( m_mutex );
        m_stop = false;
    }

    m_thread = std::thread( [this] { run(); } );
}


void LiveSampler::Stop()
{
    if( !m_thread.joinable() )
        return;

    {
        std::lock_guard lock( m_mutex );
        m_stop = true;
    }

    m_wake.notify_all();
    m_thread.join();
}


void LiveSampler::SetServer( const std::string& aHost, int aPort )
{
    {
        std::lock_guard lock( m_mutex );

        if( aHost == m_host && aPort == m_port )
            return;

        m_host = aHost;
        m_port = aPort;
        ++m_serverVersion;
        m_chainRequested = true;
        m_wakeup         = true;
    }

    m_wake.notify_all();
}


unsigned LiveSampler::SetTargets( std::vector<Target> aTargets )
{
    unsigned version;

    {
        std::lock_guard lock( m_mutex );
        m_targets = std::move( aTargets );
        version   = ++m_targetsVersion;
        m_wakeup  = true;
    }

    m_wake.notify_all();
    return version;
}


void LiveSampler::SetInterval( std::chrono::milliseconds aInterval )
{
    std::lock_guard lock( m_mutex );
    m_interval = std::max( aInterval, std::chrono::milliseconds( 10 ) );
}


void LiveSampler::RefreshChain()
{
    {
        std::lock_guard lock( m_mutex );
        m_chainRequested = true;
        m_wakeup         = true;
    }

    m_wake.notify_all();
}


void LiveSampler::SetExtestCallback( ExtestCallback aCallback )
{
    m_onExtest = std::move( aCallback );
}


void LiveSampler::EnterExtest( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                               const Bits& aImage )
{
    m_extest.RequestEnter( aTap, std::move( aDevice ), aImage );
    wake();
}


void LiveSampler::UpdateExtest( const std::string& aTap, const Bits& aImage )
{
    if( m_extest.RequestImage( aTap, aImage ) )
        wake();
}


void LiveSampler::LeaveExtest( const std::string& aTap )
{
    auto request = m_extest.RequestLeave( aTap );

    if( request == ExtestTable::LeaveRequest::Unknown )
        return;

    wake();

    if( request == ExtestTable::LeaveRequest::Cancelled )
        reportExtest( aTap, ExtestUpdate::State::Left, "" );
}


void LiveSampler::TakeOverExtest( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                                  const Bits& aImage )
{
    m_extest.RequestTakeOver( aTap, std::move( aDevice ), aImage );
    wake();
}


void LiveSampler::TakeOverExtest( const std::vector<TakeOver>& aTakeOvers )
{
    for( const TakeOver& t : aTakeOvers )
        m_extest.RequestTakeOver( t.tap, t.device, t.image );

    wake();
}


void LiveSampler::ForceLeaveExtest( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice )
{
    m_extest.RequestForceLeave( aTap, std::move( aDevice ) );
    wake();
}


void LiveSampler::SetLeaveExtestOnStop( bool aLeave )
{
    std::lock_guard lock( m_mutex );
    m_leaveOnStop = aLeave;
}


void LiveSampler::RequestSnapshot( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                                   int aCount, SnapshotCallback aCallback )
{
    {
        std::lock_guard lock( m_mutex );
        m_snapshot = SnapshotRequest{ aTap, std::move( aDevice ), std::max( aCount, 1 ),
                                      std::move( aCallback ) };
        m_wakeup   = true;
    }

    m_wake.notify_all();
}


void LiveSampler::RunInterconnect( InterconnectRequest aRequest, InterconnectCallback aCallback )
{
    {
        std::lock_guard lock( m_mutex );
        m_interconnect.emplace( std::move( aRequest ), std::move( aCallback ) );
        m_interconnectStop = false;
        m_wakeup           = true;
    }

    m_wake.notify_all();
}


void LiveSampler::StopInterconnect()
{
    m_interconnectStop = true;
    m_wake.notify_all();
}


void LiveSampler::wake()
{
    {
        std::lock_guard lock( m_mutex );
        m_wakeup = true;
    }

    m_wake.notify_all();
}


void LiveSampler::requestChain()
{
    std::lock_guard lock( m_mutex );
    m_chainRequested = true;
}


void LiveSampler::reportExtest( const std::string& aTap, ExtestUpdate::State aState,
                                const std::string& aMessage, int aInterruptions )
{
    if( m_onExtest )
        m_onExtest( ExtestUpdate{ aTap, aState, aMessage, aInterruptions } );
}


void LiveSampler::processSnapshot( const SamplerContext& aCtx )
{
    std::optional<SnapshotRequest> req;

    {
        std::lock_guard lock( m_mutex );
        req.swap( m_snapshot );
    }

    if( !req )
        return;

    auto snap = TakeSnapshot( aCtx, req->tap, *req->device, req->count );

    if( req->callback )
        req->callback( snap );
}


SamplerContext LiveSampler::context( openocd::TclClient& aClient )
{
    auto jtagScan = aClient.HasCommand( "jtag scan" );

    return SamplerContext{ aClient, jtagScan && *jtagScan, m_extest, m_onExtest,
                           [this] { requestChain(); } };
}


void LiveSampler::processInterconnect( const SamplerContext& aCtx )
{
    std::optional<std::pair<InterconnectRequest, InterconnectCallback>> job;

    {
        std::lock_guard lock( m_mutex );
        job.swap( m_interconnect );
    }

    if( !job )
        return;

    // Repeating: wait the sampling interval between the runs
    auto nextRun = [this]
    {
        std::unique_lock lock( m_mutex );

        if( !m_stop && !m_interconnectStop )
            m_wake.wait_for( lock, m_interval,
                             [this] { return m_stop || m_interconnectStop.load(); } );

        return !m_stop && !m_interconnectStop;
    };

    InterconnectJob( aCtx, job->first, job->second ).Run( nextRun );
}


void LiveSampler::leaveAllExtest( openocd::TclClient& aClient )
{
    {
        std::lock_guard lock( m_mutex );

        if( !m_leaveOnStop )
            return;
    }

    ExtestJob( context( aClient ) ).LeaveAll();
}


bool LiveSampler::takeSettings( Settings& aSettings )
{
    std::lock_guard lock( m_mutex );

    if( m_stop )
        return false;

    aSettings.host           = m_host;
    aSettings.port           = m_port;
    aSettings.serverVersion  = m_serverVersion;
    aSettings.targets        = m_targets;
    aSettings.targetsVersion = m_targetsVersion;
    aSettings.interval       = m_interval;
    aSettings.chainRequested = m_chainRequested;
    m_chainRequested         = false;
    m_wakeup                 = false;
    return true;
}


void LiveSampler::ensureClient( Settings& aSettings )
{
    if( m_client && m_clientVersion == aSettings.serverVersion )
        return;

    openocd::TclClient::Config cfg;
    cfg.host        = aSettings.host;
    cfg.port        = aSettings.port;
    cfg.timeoutMs   = 2000;
    m_client        = std::make_unique<openocd::TclClient>( cfg );
    m_clientVersion = aSettings.serverVersion;
    m_lastChain.reset();
    aSettings.chainRequested = true;
}


bool LiveSampler::updateChain( const Settings& aSettings )
{
    using clock = std::chrono::steady_clock;

    bool connected = m_lastChain && m_lastChain->connected;
    bool idleCheck = aSettings.targets.empty() && clock::now() - m_lastChainRead > IDLE_CHAIN_CHECK;

    if( !aSettings.chainRequested && connected && !idleCheck )
        return true;

    ChainUpdate upd;
    auto        taps = ReadChain( *m_client );
    m_lastChainRead  = clock::now();

    if( taps )
    {
        upd.connected = true;
        upd.taps      = std::move( *taps );

        auto jtagScan = m_client->HasCommand( "jtag scan" );
        upd.jtagScan  = jtagScan && *jtagScan;
    }
    else
    {
        upd.error = std::string( openocd::ToString( taps.error().kind ) ) + ": "
                    + taps.error().message;
    }

    if( !m_lastChain || !( *m_lastChain == upd ) )
    {
        m_lastChain = upd;

        if( m_onChain )
            m_onChain( upd );
    }

    return upd.connected;
}


bool LiveSampler::trackConnection( bool aConnected )
{
    bool reconnected = aConnected && !m_wasConnected;

    if( !aConnected && m_wasConnected )
    {
        m_lastSample.reset();   // report the values again after reconnecting
        m_extest.MarkActiveLost();
    }

    m_wasConnected = aConnected;
    return reconnected;
}


void LiveSampler::runJobs( const Settings& aSettings, bool aReconnected )
{
    SamplerContext ctx = context( *m_client );
    ExtestJob      extest( ctx );

    extest.ProcessRequests( aReconnected );
    processSnapshot( ctx );
    processInterconnect( ctx );

    // Restores EXTEST after an interruption (marks the TAP dirty)
    extest.CheckInterruptions();

    bool anyDirty = false;

    for( const auto& [name, e] : m_extest.Active() )
        anyDirty = anyDirty || e.dirty;

    extest.ShiftChangedImages( aSettings.targets );
    sample( ctx, aSettings, anyDirty );
}


void LiveSampler::sample( const SamplerContext& aCtx, const Settings& aSettings, bool aAnyDirty )
{
    const auto& targets = aSettings.targets;

    // With 'jtag scan' the combined scan also shifts changed images
    if( aCtx.jtagScan && ( !targets.empty() || aAnyDirty ) )
        reportSample( SampleTogether( aCtx, targets, aSettings.targetsVersion ) );
    else if( !targets.empty() )
        reportSample( SampleEach( aCtx, targets, aSettings.targetsVersion ) );
    else if( !m_lastSample || m_lastSample->targetsVersion != aSettings.targetsVersion )
        reportSample( SampleUpdate{ aSettings.targetsVersion, {} } );   // targets removed
}


void LiveSampler::reportSample( const SampleUpdate& aUpdate )
{
    if( m_lastSample && *m_lastSample == aUpdate )
        return;

    m_lastSample = aUpdate;

    if( m_onSample )
        m_onSample( aUpdate );
}


std::chrono::milliseconds LiveSampler::nextWait( const Settings& aSettings, bool aConnected ) const
{
    using std::chrono::milliseconds;

    milliseconds wait = aConnected ? aSettings.interval
                                   : std::chrono::duration_cast<milliseconds>( RECONNECT_DELAY );

    if( aSettings.targets.empty() )
        wait = std::max( wait, milliseconds( 500 ) );

    // Pending EXTEST work (e.g. a retry to leave) should not wait long
    if( m_extest.WorkPending() )
        wait = std::min( wait, milliseconds( 200 ) );

    return wait;
}


void LiveSampler::run()
{
    // A fresh start (Start() may follow a Stop())
    m_client.reset();
    m_clientVersion = ~0u;
    m_lastChain.reset();
    m_lastSample.reset();
    m_lastChainRead = {};
    m_wasConnected  = false;

    for( ;; )
    {
        Settings settings;

        if( !takeSettings( settings ) )
        {
            if( m_client )
                leaveAllExtest( *m_client );

            return;
        }

        ensureClient( settings );

        bool connected   = updateChain( settings );
        bool reconnected = trackConnection( connected );

        if( connected )
            runJobs( settings, reconnected );

        std::unique_lock lock( m_mutex );
        m_wake.wait_for( lock, nextWait( settings, connected ),
                         [this] { return m_stop || m_wakeup; } );
    }
}


} // namespace bscan
