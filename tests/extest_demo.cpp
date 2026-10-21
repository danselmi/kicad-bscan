// SPDX-License-Identifier: GPL-3.0-only
// Exercises EXTEST in LiveSampler against OpenOCD + tests/sim (SIMCHIP):
//
//   extest-demo <file.bsd>
//
//  1. snapshot, plan (IO0 = pin 4 driven 0), enter EXTEST
//  2. change IO0 to 1 while active
//  2b. another client loads BYPASS: with 'jtag cur_ir' this is reported as
//      an interruption and EXTEST is restored
//  3. wait while the user/test script restarts OpenOCD: EXTEST must be left
//  4. enter again, then Stop(): EXTEST must be left on stop
//
// Prints every event with a time stamp.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "bscan/extest.h"
#include "bscan/live_sampler.h"
#include "openocd/tcl_client.h"

using namespace bscan;
using clk = std::chrono::steady_clock;

static clk::time_point g_t0 = clk::now();

static double now()
{
    return std::chrono::duration<double>( clk::now() - g_t0 ).count();
}


int main( int argc, char** argv )
{
    if( argc < 2 )
        return 2;

    auto dev = BsdlDevice::Load( argv[1] );

    if( !dev )
        return 1;

    std::shared_ptr<const BsdlDevice> d = *dev;
    LiveSampler                        sampler;
    std::atomic<bool>                  haveTap{ false };
    TapInfo                            tap;
    std::atomic<int>                   extestEvents{ 0 };

    sampler.SetExtestCallback(
            [&]( const LiveSampler::ExtestUpdate& u )
            {
                const char* st = u.state == LiveSampler::ExtestUpdate::State::Active ? "ACTIVE"
                                 : u.state == LiveSampler::ExtestUpdate::State::Left ? "LEFT"
                                 : u.state == LiveSampler::ExtestUpdate::State::Interrupted
                                         ? "INTERRUPTED"
                                         : "FAILED";
                std::printf( "%6.2f extest %s %s %s %d\n", now(), u.tap.c_str(), st,
                             u.message.c_str(), u.interruptions );
                std::fflush( stdout );
                ++extestEvents;
            } );

    sampler.Start(
            [&]( const LiveSampler::ChainUpdate& u )
            {
                std::printf( "%6.2f chain %s\n", now(), u.connected ? "connected" : u.error.c_str() );

                for( const TapInfo& t : u.taps )
                {
                    if( t.name == "simchip.tap" )
                    {
                        tap     = t;
                        haveTap = true;
                    }
                }

                std::fflush( stdout );
            },
            []( const LiveSampler::SampleUpdate& u )
            {
                std::printf( "%6.2f sample", now() );

                for( const auto& r : u.pins )
                    std::printf( "  %s in=%s out=%s%s", r.key.c_str(),
                                 r.value.input ? ( *r.value.input ? "1" : "0" ) : "-",
                                 ToString( r.value.output ), r.ok ? "" : " (error)" );

                std::printf( "\n" );
                std::fflush( stdout );
            } );

    while( !haveTap )
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );

    auto io0 = *d->LookupPin( "", "4" );
    sampler.SetTargets( { { "IO0", tap, d, "4", io0 } } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 500 ) );

    // 1. snapshot + plan + enter
    std::atomic<bool> snapDone{ false };
    std::vector<Bits> samples;
    sampler.RequestSnapshot( tap, d, 5,
                             [&]( const Result<LiveSampler::Snapshot>& r )
                             {
                                 if( r )
                                     samples = r->samples;

                                 snapDone = true;
                             } );

    while( !snapDone )
        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );

    ExtestPlan plan = PlanExtest( *d, "", samples, { { "4", PinDrive::Low } } );
    std::printf( "%6.2f plan: %zu samples, %zu conflicts\n", now(), samples.size(),
                 plan.conflicts.size() );
    sampler.EnterExtest( tap, d, plan.image );
    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );

    // 2. change while active
    auto ch = SetPin( *d, "", plan.image, "4", PinDrive::High );
    std::printf( "%6.2f set IO0=1\n", now() );
    sampler.UpdateExtest( tap.name, ch->image );
    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );

    // 2b. interruption by another client
    {
        openocd::TclClient other( openocd::TclClient::Config{} );
        auto               r = other.Run( "irscan simchip.tap 0xf" );
        std::printf( "%6.2f other client: BYPASS %s\n", now(), r ? "loaded" : "failed" );
        std::fflush( stdout );
    }

    std::this_thread::sleep_for( std::chrono::milliseconds( 1000 ) );

    // 3. the test script restarts OpenOCD now
    std::printf( "%6.2f waiting for OpenOCD restart\n", now() );
    std::fflush( stdout );
    std::this_thread::sleep_for( std::chrono::seconds( 8 ) );

    // 4. enter again, then stop
    sampler.EnterExtest( tap, d, plan.image );
    std::this_thread::sleep_for( std::chrono::milliseconds( 800 ) );
    std::printf( "%6.2f stop\n", now() );
    sampler.Stop();
    std::printf( "%6.2f stopped\n", now() );
    return 0;
}
