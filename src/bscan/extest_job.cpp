// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/extest_job.h"

#include <algorithm>

#include "bscan/extest.h"

namespace bscan
{

static const char* RESET_HINT = ". The device may still be in EXTEST: reset the board.";


ExtestJob::ExtestJob( const SamplerContext& aContext ) :
        m_ctx( aContext )
{
}


void ExtestJob::ProcessRequests( bool aReconnected )
{
    if( aReconnected )
        m_ctx.extest.LeaveLost();

    takeOverTogether();

    for( const auto& [name, e] : m_ctx.extest.All() )
    {
        if( refusedAsSecond( name, e ) )
            continue;

        switch( e.phase )
        {
        case ExtestTable::Phase::PendingEnter:    enter( name, e );    break;
        case ExtestTable::Phase::PendingTakeOver: takeOver( name, e ); break;
        case ExtestTable::Phase::PendingLeave:    leave( name, e );    break;
        case ExtestTable::Phase::Active:          break;   // images: see ShiftChangedImages
        }
    }
}


bool ExtestJob::refusedAsSecond( const std::string& aName, const ExtestTable::Entry& aEntry )
{
    if( m_ctx.jtagScan
        || ( aEntry.phase != ExtestTable::Phase::PendingEnter
             && aEntry.phase != ExtestTable::Phase::PendingTakeOver ) )
        return false;

    std::string other = m_ctx.extest.OtherHeld( aName );

    if( other.empty() )
        return false;

    m_ctx.extest.Erase( aName );
    m_ctx.Report( aName, ExtestUpdate::State::Failed,
                  other + " is in EXTEST; only one device can be in EXTEST at a time (OpenOCD "
                          "puts the other devices into BYPASS)" );
    return true;
}


void ExtestJob::enter( const std::string& aName, const ExtestTable::Entry& aEntry )
{
    // Other TAPs in EXTEST stay there (scanned along with their image)
    std::vector<ScanTap> along = m_ctx.ExtestAlong( aName );

    auto        r = EnterExtest( m_ctx.client, aEntry.tap, *aEntry.device, aEntry.image, along );
    std::string leaveProblem;   // leaving again after a failed enter

    if( !r )
    {
        // Not left half-way
        if( auto l = LeaveExtest( m_ctx.client, aEntry.tap, *aEntry.device, along ); !l )
            leaveProblem = ". Leaving it again failed too (" + l.error().message
                           + "): the device may be in EXTEST, reset the board.";
    }

    // LeaveExtest() may have been called while entering
    bool present   = m_ctx.extest.Settle( aName, ExtestTable::Phase::PendingEnter, r.has_value() );
    bool cancelled = r && !present;

    if( cancelled )
    {
        // Already reported as left by LiveSampler::LeaveExtest(): make that true
        if( auto l = LeaveExtest( m_ctx.client, aEntry.tap, *aEntry.device, along ); !l )
            m_ctx.Report( aName, ExtestUpdate::State::Left,
                          "EXTEST was cancelled while it was being entered, and leaving it failed ("
                                  + l.error().message + ")" + RESET_HINT );
    }
    else if( r )
    {
        m_ctx.Report( aName, ExtestUpdate::State::Active, {} );
    }
    else
    {
        m_ctx.Report( aName, ExtestUpdate::State::Failed,
                      "could not enter EXTEST: " + r.error().message + leaveProblem );
    }
}


void ExtestJob::takeOver( const std::string& aName, const ExtestTable::Entry& aEntry )
{
    std::vector<ScanTap> along = m_ctx.ExtestAlong( aName );

    // No automatic leave on failure: the user decides (the pins keep what
    // they had)
    auto r         = TakeOverExtest( m_ctx.client, aEntry.tap, *aEntry.device, aEntry.image, along );
    bool cancelled = !m_ctx.extest.Settle( aName, ExtestTable::Phase::PendingTakeOver,
                                           r.has_value() );

    if( cancelled && r )
    {
        // Cancelled meanwhile (LeaveExtest): leave as requested
        if( auto l = LeaveExtest( m_ctx.client, aEntry.tap, *aEntry.device, along ); !l )
            m_ctx.Report( aName, ExtestUpdate::State::Left,
                          "EXTEST was cancelled while it was being taken over, and leaving it "
                          "failed (" + l.error().message + ")" + RESET_HINT );
    }
    else if( r )
    {
        m_ctx.Report( aName, ExtestUpdate::State::Active, {} );
    }
    else
    {
        m_ctx.Report( aName, ExtestUpdate::State::Failed,
                      "could not take over EXTEST: " + r.error().message );
    }
}


void ExtestJob::leave( const std::string& aName, const ExtestTable::Entry& aEntry )
{
    std::vector<ScanTap> along = m_ctx.ExtestAlong( aName );
    auto                 r     = LeaveExtest( m_ctx.client, aEntry.tap, *aEntry.device, along );

    if( !r && IsConnectionError( r.error() ) )
    {
        // Keep trying
        m_ctx.Report( aName, ExtestUpdate::State::Failed,
                      "could not leave EXTEST yet (" + r.error().message + "); retrying" );
        return;
    }

    m_ctx.extest.Erase( aName );

    if( r )
    {
        m_ctx.Report( aName, ExtestUpdate::State::Left,
                      aEntry.lost ? "the connection to OpenOCD was interrupted; EXTEST was ended "
                                    "for safety"
                                  : "" );
    }
    else
    {
        // No longer tracked: say so clearly instead of pretending
        m_ctx.Report( aName, ExtestUpdate::State::Left,
                      "could not leave EXTEST: " + r.error().message + RESET_HINT );
    }
}


// Why the TAPs cannot be taken over together; empty if they can: every one
// must still be in EXTEST before anything is scanned
std::string ExtestJob::checkAllInExtest( const ExtestTable::Entries& aPending ) const
{
    if( !m_ctx.jtagScan )
        return "taking over several devices needs OpenOCD's 'jtag scan'";

    for( const auto& [name, e] : aPending )
    {
        auto ir = ReadCurIr( m_ctx.client, e.tap.name );

        if( !ir )
            return ir.error().message;

        if( !*ir )
            return "OpenOCD cannot report the current instruction ('jtag cur_ir'): taking over "
                   "is not safe";

        if( PinControlInstruction( *e.device, **ir ) != "EXTEST" )
            return name + " is not in EXTEST (current instruction "
                   + DescribeInstruction( *e.device, **ir ) + ")";
    }

    return {};
}


void ExtestJob::takeOverTogether()
{
    ExtestTable::Entries pending = m_ctx.extest.InPhase( ExtestTable::Phase::PendingTakeOver );

    // One take-over goes the usual way
    if( pending.size() < 2 )
        return;

    std::string error = checkAllInExtest( pending );

    // Devices already in EXTEST, then the ones taken over: one scan
    std::vector<ScanTap> scan;

    if( error.empty() )
    {
        scan = m_ctx.ExtestAlong( {} );

        for( const auto& [name, e] : pending )
        {
            if( auto t = ExtestScanTap( e.tap, *e.device, e.image ) )
                scan.push_back( std::move( *t ) );
            else
                error = name + ": " + t.error().message;
        }
    }

    if( error.empty() )
    {
        if( auto r = ScanTogether( m_ctx.client, scan ); !r )
            error = r.error().message;
    }

    // No automatic leave on failure: the user decides
    for( const auto& [name, e] : pending )
        m_ctx.extest.SettleIfPending( name, ExtestTable::Phase::PendingTakeOver, error.empty() );

    for( const auto& [name, e] : pending )
    {
        if( error.empty() )
            m_ctx.Report( name, ExtestUpdate::State::Active, {} );
        else
            m_ctx.Report( name, ExtestUpdate::State::Failed, "could not take over EXTEST: " + error );
    }
}


void ExtestJob::CheckInterruptions()
{
    for( const auto& [name, e] : m_ctx.extest.Active() )
    {
        auto ir = ReadCurIr( m_ctx.client, e.tap.name );

        if( !ir )
        {
            if( IsConnectionError( ir.error() ) )
                m_ctx.requestChain();

            continue;
        }

        // Without 'jtag cur_ir' nothing can be detected
        if( !*ir || PinControlInstruction( *e.device, **ir ) == "EXTEST" )
            continue;

        // The next scan (this cycle) loads EXTEST and the image again
        if( auto count = m_ctx.extest.Interrupted( name ) )
            m_ctx.Report( name, ExtestUpdate::State::Interrupted,
                          DescribeInstruction( *e.device, **ir ), *count );
    }
}


void ExtestJob::ShiftChangedImages( const std::vector<SampleTarget>& aTargets )
{
    if( m_ctx.jtagScan )
        return;

    for( const auto& [name, e] : m_ctx.extest.Active() )
    {
        bool sampled = std::ranges::any_of( aTargets, [&]( const SampleTarget& t )
                                            { return t.tap.name == name; } );

        if( e.dirty && !sampled && ExtestScan( m_ctx.client, e.tap, *e.device, e.image ) )
            m_ctx.extest.Shifted( name, e.image );
    }
}


void ExtestJob::LeaveAll()
{
    for( const auto& [name, e] : m_ctx.extest.TakeAll() )
    {
        // Best effort: the plugin is closing, nobody is left to tell
        if( e.phase != ExtestTable::Phase::PendingEnter )
            (void) LeaveExtest( m_ctx.client, e.tap, *e.device );
    }
}

} // namespace bscan
