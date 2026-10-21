// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/sample_job.h"

#include <algorithm>
#include <functional>
#include <map>
#include <thread>

#include "bscan/extest.h"

namespace bscan
{

namespace
{

// What one combined scan ('jtag scan') contains: one entry per TAP, the
// TAPs in EXTEST (all of them, sampled or not) first, then the sampled TAPs
// with SAMPLE
struct CombinedScan
{
    ExtestTable::Entries               active;    // in EXTEST: scanned with their image
    std::map<std::string, std::string> busy;      // being entered or left: not scanned
    std::map<std::string, std::string> errors;    // TAP -> why not scanned
    std::vector<ScanTap>               scan;
    std::vector<std::string>           scanned;   // TAP name per scan entry

    void Add( const std::string& aName, Result<ScanTap> aTap )
    {
        if( !aTap )
        {
            errors[aName] = aTap.error().message;
            return;
        }

        scan.push_back( std::move( *aTap ) );
        scanned.push_back( aName );
    }

    bool Has( const std::string& aName ) const
    {
        return active.contains( aName ) || errors.contains( aName ) || busy.contains( aName )
               || std::ranges::find( scanned, aName ) != scanned.end();
    }
};


CombinedScan planCombinedScan( const SamplerContext& aCtx, const std::vector<SampleTarget>& aTargets )
{
    CombinedScan c;

    for( const auto& [name, e] : aCtx.extest.All() )
    {
        if( e.phase == ExtestTable::Phase::Active && !e.lost )
            c.active[name] = e;
        else
            c.busy[name] = e.phase == ExtestTable::Phase::PendingLeave ? "leaving EXTEST"
                                                                       : "entering EXTEST";
    }

    for( const auto& [name, e] : c.active )
        c.Add( name, ExtestScanTap( e.tap, *e.device, e.image ) );

    for( const SampleTarget& t : aTargets )
    {
        const std::string& name = t.tap.name;

        if( c.Has( name ) )
            continue;

        if( !t.device )
            c.errors[name] = "no BSDL";
        else
            c.Add( name, SampleScanTap( t.tap, *t.device ) );
    }

    return c;
}


PinResult pinFromCombinedScan( const SampleTarget& aTarget, const CombinedScan& aPlan,
                               const Result<std::vector<Bits>>& aResult )
{
    PinResult r;
    r.key = aTarget.key;
    r.tap = aTarget.tap.name;

    if( auto b = aPlan.busy.find( r.tap ); b != aPlan.busy.end() )
    {
        r.error = "paused: " + b->second;
        return r;
    }

    if( auto e = aPlan.errors.find( r.tap ); e != aPlan.errors.end() )
    {
        r.error = e->second;
        return r;
    }

    if( !aResult )
    {
        r.error = aResult.error().message;
        return r;
    }

    size_t idx = std::ranges::find( aPlan.scanned, r.tap ) - aPlan.scanned.begin();

    if( idx >= aResult->size() )
    {
        r.error = "not scanned";
        return r;
    }

    r.ok = true;

    if( auto ex = aPlan.active.find( r.tap ); ex != aPlan.active.end() )
        r.value = DecodeExtest( ( *aResult )[idx], ex->second.image, aTarget.cells );
    else
        r.value = DecodePin( ( *aResult )[idx], aTarget.cells );

    return r;
}


// Lost connection: the chain may be different after a restart
template <typename T>
void checkConnection( const SamplerContext& aCtx, const Result<T>& aResult )
{
    if( !aResult && IsConnectionError( aResult.error() ) )
        aCtx.requestChain();
}

} // namespace


SampleUpdate SampleTogether( const SamplerContext& aCtx, const std::vector<SampleTarget>& aTargets,
                             unsigned aTargetsVersion )
{
    CombinedScan plan   = planCombinedScan( aCtx, aTargets );
    auto         result = ScanTogether( aCtx.client, plan.scan );

    checkConnection( aCtx, result );

    // The images just shifted are driven now
    if( result )
        for( const auto& [name, e] : plan.active )
            aCtx.extest.Shifted( name, e.image );

    SampleUpdate upd;
    upd.targetsVersion = aTargetsVersion;

    for( const SampleTarget& t : aTargets )
        upd.pins.push_back( pinFromCombinedScan( t, plan, result ) );

    return upd;
}


namespace
{

// One scan of aTarget's TAP: EXTEST (keeps driving the image, applies image
// changes) or SAMPLE
Result<Bits> scanTap( const SamplerContext& aCtx, const SampleTarget& aTarget,
                      const ExtestTable::Entry* aExtest )
{
    if( aExtest )
    {
        auto bits = ExtestScan( aCtx.client, aExtest->tap, *aExtest->device, aExtest->image );

        if( bits )
            aCtx.extest.Shifted( aTarget.tap.name, aExtest->image );

        return bits;
    }

    if( aTarget.device )
        return Sample( aCtx.client, aTarget.tap, *aTarget.device );

    return std::unexpected( openocd::Error{ openocd::ErrorKind::Command, "no BSDL" } );
}

} // namespace


SampleUpdate SampleEach( const SamplerContext& aCtx, const std::vector<SampleTarget>& aTargets,
                         unsigned aTargetsVersion )
{
    SampleUpdate upd;
    upd.targetsVersion = aTargetsVersion;
    upd.pins.resize( aTargets.size() );

    ExtestTable::Entries extest = aCtx.extest.Active();
    ExtestTable::Entries held   = aCtx.extest.All();   // any phase
    std::vector<bool>    done( aTargets.size(), false );

    // All targets on the same TAP (and BSDL) share one scan
    auto fillSameTap = [&]( size_t aFirst, const std::function<void( PinResult&, const SampleTarget& )>& aFill )
    {
        const SampleTarget& t = aTargets[aFirst];

        for( size_t j = aFirst; j < aTargets.size(); ++j )
        {
            const SampleTarget& u = aTargets[j];

            if( done[j] || u.tap.name != t.tap.name || u.device != t.device )
                continue;

            upd.pins[j].key = u.key;
            upd.pins[j].tap = u.tap.name;
            aFill( upd.pins[j], u );
            done[j] = true;
        }
    };

    for( size_t i = 0; i < aTargets.size(); ++i )
    {
        if( done[i] )
            continue;

        const SampleTarget& t = aTargets[i];

        // Another TAP is held in EXTEST: scanning this one would end that
        if( !held.empty() && !held.contains( t.tap.name ) )
        {
            std::string why = "paused: " + held.begin()->first + " is in EXTEST";
            fillSameTap( i, [&]( PinResult& r, const SampleTarget& ) { r.error = why; } );
            continue;
        }

        auto                      ex   = extest.find( t.tap.name );
        const ExtestTable::Entry* in   = ex != extest.end() ? &ex->second : nullptr;
        Result<Bits>              bits = scanTap( aCtx, t, in );

        checkConnection( aCtx, bits );

        fillSameTap( i,
                     [&]( PinResult& r, const SampleTarget& u )
                     {
                         if( !bits )
                         {
                             r.error = bits.error().message;
                             return;
                         }

                         r.ok    = true;
                         r.value = in ? DecodeExtest( *bits, in->image, u.cells )
                                      : DecodePin( *bits, u.cells );
                     } );
    }

    return upd;
}


Result<Snapshot> TakeSnapshot( const SamplerContext& aCtx, const TapInfo& aTap,
                               const BsdlDevice& aDevice, int aCount )
{
    // A SAMPLE would end EXTEST on that TAP, and without 'jtag scan' on any
    // other TAP too (OpenOCD puts all TAPs not scanned into BYPASS)
    if( aCtx.extest.Contains( aTap.name ) || ( !aCtx.jtagScan && !aCtx.extest.Empty() ) )
        return std::unexpected( openocd::Error{ openocd::ErrorKind::Command,
                                                "a device is in EXTEST" } );

    Snapshot             snap;
    std::vector<ScanTap> along = aCtx.ExtestAlong( aTap.name );

    // Before the samples: 'poll' polls the current target once, which would
    // interrupt EXTEST of other TAPs: not checked then
    if( along.empty() )
    {
        auto polling = BackgroundPolling( aCtx.client );

        if( !polling && IsConnectionError( polling.error() ) )
            return std::unexpected( polling.error() );

        snap.backgroundPolling = polling && *polling;
    }

    for( int i = 0; i < aCount; ++i )
    {
        auto bits = Sample( aCtx.client, aTap, aDevice, along );

        if( !bits )
            return std::unexpected( bits.error() );

        snap.samples.push_back( std::move( *bits ) );

        if( i + 1 < aCount )
            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }

    return snap;
}

} // namespace bscan
