// SPDX-License-Identifier: GPL-3.0-only
// Two devices in EXTEST at the same time (needs OpenOCD with 'jtag scan'),
// against OpenOCD + tests/sim, where IO0 of each chip is wired to IN1 of the
// other:
//
//   multi-extest-demo tests/sim/simchip.bsd tests/sim/simchip2.bsd
//
//  1. both IN1 read 1 (pull-ups)
//  2. EXTEST on simchip, IO0 = 0          -> other.IN1 = 0
//  3. EXTEST on other as well, IO0 = 0    -> simchip.IN1 = 0, other.IN1 still 0
//  4. leave simchip                       -> other.IN1 = 1, simchip.IN1 still 0
//  5. leave other                         -> both 1
//  6. both into EXTEST again (IO0 = 0), the sampler stops keeping EXTEST
//     (window closed with "keep"); a new sampler takes both over together,
//     other.IO0 now Z                     -> simchip.IN1 = 1, other.IN1 = 0
//  7. leave both                          -> both 1
//
// Exits with 1 at the first expectation that is not met within 2 s.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "bscan/extest.h"
#include "bscan/live_sampler.h"

using namespace bscan;
using clk = std::chrono::steady_clock;

static clk::time_point g_t0 = clk::now();

static double now()
{
    return std::chrono::duration<double>( clk::now() - g_t0 ).count();
}


int main( int argc, char** argv )
{
    if( argc < 3 )
        return 2;

    auto devA = BsdlDevice::Load( argv[1] );
    auto devB = BsdlDevice::Load( argv[2] );

    if( !devA || !devB )
        return 2;

    std::shared_ptr<const BsdlDevice> a = *devA, b = *devB;

    auto                                 sampler = std::make_unique<LiveSampler>();
    std::mutex                           mutex;
    std::map<std::string, TapInfo>       taps;
    std::map<std::string, std::string>   values;    // key -> "in/out" or error
    bool                                 jtagScan = false;

    auto start = [&]( LiveSampler& aSampler )
    {
        aSampler.SetExtestCallback(
                [&]( const LiveSampler::ExtestUpdate& u )
                {
                    std::printf( "%6.2f extest %s state %d %s\n", now(), u.tap.c_str(),
                                 static_cast<int>( u.state ), u.message.c_str() );
                    std::fflush( stdout );
                } );

        aSampler.Start(
                [&]( const LiveSampler::ChainUpdate& u )
                {
                    std::lock_guard lock( mutex );
                    jtagScan = u.jtagScan;

                    for( const TapInfo& t : u.taps )
                        taps[t.name] = t;
                },
                [&]( const LiveSampler::SampleUpdate& u )
                {
                    std::lock_guard lock( mutex );
                    std::string     line;

                    for( const auto& p : u.pins )
                    {
                        std::string v = p.ok ? ( p.value.input ? ( *p.value.input ? "1" : "0" ) : "-" )
                                                       + std::string( "/" ) + ToString( p.value.output )
                                             : "ERR " + p.error;
                        values[p.key] = v;
                        line += "  " + p.key + "=" + v;
                    }

                    std::printf( "%6.2f sample%s\n", now(), line.c_str() );
                    std::fflush( stdout );
                } );
    };

    start( *sampler );

    auto waitFor = [&]( const char* aWhat, const std::function<bool()>& aCond )
    {
        auto until = clk::now() + std::chrono::seconds( 2 );

        while( clk::now() < until )
        {
            {
                std::lock_guard lock( mutex );

                if( aCond() )
                {
                    std::printf( "%6.2f OK   %s\n", now(), aWhat );
                    return true;
                }
            }

            std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
        }

        std::printf( "%6.2f FAIL %s\n", now(), aWhat );
        return false;
    };

    auto in = [&]( const std::string& aKey, char aValue )
    {
        auto it = values.find( aKey );
        return it != values.end() && !it->second.empty() && it->second[0] == aValue;
    };

    if( !waitFor( "chain with simchip.tap and other.tap",
                  [&] { return taps.contains( "simchip.tap" ) && taps.contains( "other.tap" ); } ) )
        return 1;

    if( !jtagScan )
    {
        std::printf( "OpenOCD has no 'jtag scan'\n" );
        return 1;
    }

    TapInfo tapA = taps["simchip.tap"], tapB = taps["other.tap"];

    sampler->SetTargets( { { "A.IN1", tapA, a, "2", *a->LookupPin( "", "2" ) },
                          { "A.IO0", tapA, a, "4", *a->LookupPin( "", "4" ) },
                          { "B.IN1", tapB, b, "2", *b->LookupPin( "", "2" ) },
                          { "B.IO0", tapB, b, "4", *b->LookupPin( "", "4" ) } } );

    if( !waitFor( "1. both IN1 = 1", [&] { return in( "A.IN1", '1' ) && in( "B.IN1", '1' ); } ) )
        return 1;

    // Images from the safe values, IO0 driven 0
    auto imageA = SetPin( *a, "", a->SafeBits(), "4", PinDrive::Low );
    auto imageB = SetPin( *b, "", b->SafeBits(), "4", PinDrive::Low );

    sampler->EnterExtest( tapA, a, imageA->image );

    if( !waitFor( "2. simchip EXTEST IO0=0 -> B.IN1 = 0",
                  [&] { return in( "B.IN1", '0' ) && in( "A.IN1", '1' ); } ) )
        return 1;

    sampler->EnterExtest( tapB, b, imageB->image );

    if( !waitFor( "3. both in EXTEST -> A.IN1 = 0, B.IN1 = 0",
                  [&] { return in( "A.IN1", '0' ) && in( "B.IN1", '0' ); } ) )
        return 1;

    sampler->LeaveExtest( tapA.name );

    if( !waitFor( "4. simchip left -> B.IN1 = 1, A.IN1 still 0",
                  [&] { return in( "B.IN1", '1' ) && in( "A.IN1", '0' ); } ) )
        return 1;

    sampler->LeaveExtest( tapB.name );

    if( !waitFor( "5. both left -> both IN1 = 1",
                  [&] { return in( "A.IN1", '1' ) && in( "B.IN1", '1' ); } ) )
        return 1;

    // 6. keep both in EXTEST over a "restart", then take both over together
    sampler->EnterExtest( tapA, a, imageA->image );
    sampler->EnterExtest( tapB, b, imageB->image );

    if( !waitFor( "6a. both in EXTEST again", [&] { return in( "A.IN1", '0' ) && in( "B.IN1", '0' ); } ) )
        return 1;

    sampler->SetLeaveExtestOnStop( false );
    sampler->Stop();
    sampler = std::make_unique<LiveSampler>();
    {
        std::lock_guard lock( mutex );
        values.clear();
    }
    start( *sampler );

    auto releaseB = SetPin( *b, "", b->SafeBits(), "4", PinDrive::HighZ );
    sampler->TakeOverExtest( { { tapA, a, imageA->image }, { tapB, b, releaseB->image } } );
    sampler->SetTargets( { { "A.IN1", tapA, a, "2", *a->LookupPin( "", "2" ) },
                           { "B.IN1", tapB, b, "2", *b->LookupPin( "", "2" ) } } );

    if( !waitFor( "6. both taken over, B.IO0 = Z -> A.IN1 = 1, B.IN1 = 0",
                  [&] { return in( "A.IN1", '1' ) && in( "B.IN1", '0' ); } ) )
        return 1;

    sampler->LeaveExtest( tapA.name );
    sampler->LeaveExtest( tapB.name );

    if( !waitFor( "7. both left -> both IN1 = 1",
                  [&] { return in( "A.IN1", '1' ) && in( "B.IN1", '1' ); } ) )
        return 1;

    sampler->Stop();
    std::printf( "%6.2f all OK\n", now() );
    return 0;
}
