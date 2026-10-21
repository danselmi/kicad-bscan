// SPDX-License-Identifier: GPL-3.0-only
// Interconnect test through LiveSampler against OpenOCD + tests/sim:
//
//   interconnect-demo tests/sim/simchip.bsd tests/sim/simchip2.bsd [expect]
//
// expect: ok (default), short, open - as started: jtag_sim.py [--short|--open]
//
//  1. single run with both chips out of EXTEST: results as expected, both
//     chips back out of EXTEST ('jtag cur_ir' SAMPLE) afterwards
//  2. simchip in EXTEST first (IO0 = 0): single run, then simchip is still in
//     EXTEST with its image, other.tap is not
//  3. repeat: several runs, stopped
//  4. one device only (also without 'jtag scan'): simchip drives IO0 0, 1, 0
//     and reads it back
//
// Exits with 1 if a result differs.

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

#include "bscan/extest.h"
#include "bscan/interconnect.h"
#include "bscan/live_sampler.h"

using namespace bscan;
using clk = std::chrono::steady_clock;

int main( int argc, char** argv )
{
    if( argc < 3 )
        return 2;

    std::string expect = argc > 3 ? argv[3] : "ok";
    auto        u1     = BsdlDevice::Load( argv[1] );
    auto        u2     = BsdlDevice::Load( argv[2] );

    if( !u1 || !u2 )
        return 2;

    LiveSampler                    sampler;
    std::mutex                     mutex;
    std::map<std::string, TapInfo> taps;
    std::atomic<int>               in1{ -1 };   // other.IN1 while sampled (step 2a)

    sampler.SetExtestCallback(
            []( const LiveSampler::ExtestUpdate& u )
            {
                std::printf( "extest %s state %d %s\n", u.tap.c_str(), static_cast<int>( u.state ),
                             u.message.c_str() );
            } );
    sampler.Start(
            [&]( const LiveSampler::ChainUpdate& u )
            {
                std::lock_guard lock( mutex );

                for( const TapInfo& t : u.taps )
                    taps[t.name] = t;
            },
            [&]( const LiveSampler::SampleUpdate& u )
            {
                if( !u.pins.empty() && u.pins[0].ok && u.pins[0].value.input )
                    in1 = *u.pins[0].value.input ? 1 : 0;
            } );

    for( int i = 0; i < 100; ++i )
    {
        {
            std::lock_guard lock( mutex );

            if( taps.contains( "simchip.tap" ) && taps.contains( "other.tap" ) )
                break;
        }

        std::this_thread::sleep_for( std::chrono::milliseconds( 20 ) );
    }

    TapInfo tapA, tapB;

    {
        std::lock_guard lock( mutex );
        tapA = taps["simchip.tap"];
        tapB = taps["other.tap"];
    }

    // The simulated board (QFN16): NET_A = U1.IO0 (4) - U2.IN1 (2),
    // NET_B = U2.IO0 (4) - U1.IN1 (2)
    PadNets board = {
        { "U1", { { "2", "NET_B" }, { "4", "NET_A" } } },
        { "U2", { { "2", "NET_A" }, { "4", "NET_B" } } },
    };

    InterconnectPlan plan = PlanInterconnect(
            { { tapA.name, "U1", *u1, "", {}, ( *u1 )->SafeBits() },
              { tapB.name, "U2", *u2, "", {}, ( *u2 )->SafeBits() } },
            board, {} );

    std::printf( "%zu nets tested, %zu patterns\n", plan.TestedCount(), plan.patterns.size() );

    auto request = [&]( bool aRepeat )
    {
        LiveSampler::InterconnectRequest r;
        r.taps     = { tapA, tapB };
        r.devices  = { *u1, *u2 };
        r.base     = { plan.devices[0].baseImage, plan.devices[1].baseImage };
        r.patterns = plan.patterns;
        r.repeat   = aRepeat;
        return r;
    };

    int failures = 0;

    auto check = [&]( const char* aWhat, const std::vector<NetResult>& aResults )
    {
        bool ok = aResults.size() == 2;

        for( const NetResult& r : aResults )
        {
            std::printf( "  %-6s %s\n", r.net.c_str(), r.text.c_str() );

            if( expect == "ok" )
                ok = ok && r.kind == NetResult::Kind::Ok;
            else if( expect == "short" )
                ok = ok && r.kind == NetResult::Kind::Short;
            else if( expect == "open" && r.net == "NET_A" )
                ok = ok && r.kind == NetResult::Kind::Stuck1;
            else if( expect == "open" )
                ok = ok && r.kind == NetResult::Kind::Ok;
        }

        std::printf( "%s %s\n", ok ? "OK  " : "FAIL", aWhat );
        failures += ok ? 0 : 1;
    };

    auto runOnce = [&]( const char* aWhat )
    {
        std::atomic<bool>        done{ false };
        std::vector<NetResult>   results;
        std::string              error;

        sampler.RunInterconnect( request( false ),
                                 [&]( const Result<LiveSampler::Captures>& c )
                                 {
                                     if( c )
                                         results = EvaluateInterconnect( plan, *c );
                                     else
                                         error = c.error().message;

                                     done = true;
                                 } );

        while( !done )
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

        if( !error.empty() )
        {
            std::printf( "FAIL %s: %s\n", aWhat, error.c_str() );
            ++failures;
        }
        else
        {
            check( aWhat, results );
        }
    };

    // 1.
    runOnce( "1. single run" );

    // 2. simchip in EXTEST with IO0 = 0 before the test
    auto drive0 = SetPin( **u1, "", ( *u1 )->SafeBits(), "4", PinDrive::Low );
    sampler.EnterExtest( tapA, *u1, drive0->image );
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );
    runOnce( "2. run with simchip in EXTEST" );

    // simchip is still driving IO0 = 0: other.IN1 reads it (unless --open)
    sampler.SetTargets( { { "B.IN1", tapB, *u2, "2", *( *u2 )->LookupPin( "", "2" ) } } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 400 ) );
    std::printf( "%s 2a. simchip still in EXTEST (other.IN1 = %d)\n",
                 in1 == ( expect == "open" ? 1 : 0 ) ? "OK  " : "FAIL", in1.load() );
    failures += in1 == ( expect == "open" ? 1 : 0 ) ? 0 : 1;
    sampler.LeaveExtest( tapA.name );
    sampler.SetTargets( {} );
    std::this_thread::sleep_for( std::chrono::milliseconds( 300 ) );

    // 3. repeat
    std::atomic<int> runs{ 0 };
    std::atomic<int> bad{ 0 };
    sampler.SetInterval( std::chrono::milliseconds( 10 ) );
    sampler.RunInterconnect( request( true ),
                             [&]( const Result<LiveSampler::Captures>& c )
                             {
                                 ++runs;

                                 if( !c )
                                     ++bad;
                             } );
    std::this_thread::sleep_for( std::chrono::milliseconds( 600 ) );
    sampler.StopInterconnect();
    std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
    int after = runs;
    std::this_thread::sleep_for( std::chrono::milliseconds( 200 ) );
    bool repOk = after > 3 && runs == after && bad == 0;
    std::printf( "%s 3. repeat: %d runs, stopped\n", repOk ? "OK  " : "FAIL", after );
    failures += repOk ? 0 : 1;

    // 4. one device: IO0 driven 0, 1, 0 and read back by its own input cell
    {
        Bits                           safe = ( *u1 )->SafeBits();
        PinCells                       io0  = *( *u1 )->LookupPin( "", "4" );
        std::vector<std::vector<Bits>> patterns;

        for( PinDrive drive : { PinDrive::Low, PinDrive::High, PinDrive::Low } )
            patterns.push_back( { SetPin( **u1, "", safe, "4", drive )->image } );

        LiveSampler::InterconnectRequest r;
        r.taps     = { tapA };
        r.devices  = { *u1 };
        r.base     = { safe };
        r.patterns = patterns;

        std::atomic<bool> done{ false };
        std::string       got;

        sampler.RunInterconnect( std::move( r ),
                                 [&]( const Result<LiveSampler::Captures>& c )
                                 {
                                     if( !c )
                                         got = c.error().message;
                                     else
                                         for( const auto& pattern : *c )
                                             got += pattern[0][io0.inputCell] ? '1' : '0';

                                     done = true;
                                 } );

        while( !done )
            std::this_thread::sleep_for( std::chrono::milliseconds( 10 ) );

        std::printf( "%s 4. one device: IO0 read back %s\n", got == "010" ? "OK  " : "FAIL",
                     got.c_str() );
        failures += got == "010" ? 0 : 1;
    }

    sampler.Stop();
    return failures ? 1 : 0;
}
