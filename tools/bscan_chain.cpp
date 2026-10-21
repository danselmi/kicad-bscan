// SPDX-License-Identifier: GPL-3.0-only
// Command line test for OpenOCD + BSDL, without KiCad and without GUI.
//
//   bscan-chain [--host H] [--port P] list
//   bscan-chain [--host H] [--port P] check  <tap> <file.bsd>
//   bscan-chain [--host H] [--port P] sample <tap> <file.bsd> [--map NAME] [--watch MS] [pin ...]
//   bscan-chain [--host H] [--port P] find-bsdl <tap|0xIDCODE> <dir> ...
//
// Developer tool (BSCAN_BUILD_TOOLS, not installed). It only reads: driving
// pins (EXTEST) is left to the plugin window with its review.
//
// <tap> is the OpenOCD TAP name (see 'list') or its position in the chain.
// sample without pins prints all pins of the pin map that have cells.
// sample uses only SAMPLE: the device keeps running normally.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "bscan/bsdl_index.h"
#include "bscan/chain.h"

using namespace bscan;

static std::atomic<bool> g_stop{ false };


static int usage()
{
    std::cerr << "usage:\n"
                 "  bscan-chain [--host H] [--port P] list\n"
                 "  bscan-chain [--host H] [--port P] check  <tap> <file.bsd>\n"
                 "  bscan-chain [--host H] [--port P] sample <tap> <file.bsd> [--map NAME] "
                 "[--watch MS] [pin ...]\n"
                 "  bscan-chain [--host H] [--port P] find-bsdl <tap|0xIDCODE> <dir> ...\n"
                 "      (searches the folders recursively; cache in "
                 "~/.cache/kicad-bscan/bsdl-index.json)\n";
    return 2;
}


static void printError( const char* aWhat, const openocd::Error& aErr )
{
    std::cerr << aWhat << ": " << openocd::ToString( aErr.kind ) << ": " << aErr.message << "\n";
}


static const TapInfo* findTap( const std::vector<TapInfo>& aTaps, const std::string& aName )
{
    for( const TapInfo& t : aTaps )
        if( t.name == aName || std::to_string( t.index ) == aName )
            return &t;

    return nullptr;
}


static std::string cacheFile()
{
    const char* xdg  = std::getenv( "XDG_CACHE_HOME" );
    const char* home = std::getenv( "HOME" );
    std::string base = xdg && *xdg ? xdg : home ? std::string( home ) + "/.cache" : ".";
    return base + "/kicad-bscan/bsdl-index.json";
}


static int cmdFindBsdl( openocd::TclClient& aClient, const std::string& aWhat,
                        const std::vector<std::string>& aDirs )
{
    uint32_t idcode = 0;
    int      irLen  = -1;

    if( aWhat.starts_with( "0x" ) || aWhat.starts_with( "0X" ) )
    {
        idcode = static_cast<uint32_t>( std::stoul( aWhat, nullptr, 16 ) );
    }
    else
    {
        auto taps = ReadChain( aClient );

        if( !taps )
        {
            printError( "reading the chain", taps.error() );
            return 1;
        }

        const TapInfo* tap = findTap( *taps, aWhat );

        if( !tap || !tap->idcode )
        {
            std::cerr << "TAP '" << aWhat << "' not found or without IDCODE\n";
            return 1;
        }

        idcode = tap->idcode;
        irLen  = tap->irLength;
    }

    auto      t0 = std::chrono::steady_clock::now();
    BsdlIndex index;
    index.LoadCache( cacheFile() );
    auto stats = index.Scan( aDirs );
    index.SaveCache( cacheFile() );
    double secs = std::chrono::duration<double>( std::chrono::steady_clock::now() - t0 ).count();

    std::printf( "%zu BSDL files (%zu parsed now, %zu not parseable) in %.2f s\n", stats.files,
                 stats.parsed, stats.failed, secs );

    auto matches = index.Match( idcode, irLen );
    std::printf( "IDCODE 0x%08x%s: %zu matching file(s)\n", idcode,
                 irLen > 0 ? ( ", IR length " + std::to_string( irLen ) ).c_str() : "",
                 matches.size() );

    for( const BsdlIndexEntry& e : matches )
    {
        std::string maps;

        for( const std::string& m : e.pinMaps )
            maps += ( maps.empty() ? "" : ", " ) + m;

        std::printf( "  %-20s %s\n  %20s pin maps: %s\n", e.entity.c_str(), e.path.c_str(), "",
                     maps.empty() ? "-" : maps.c_str() );
    }

    return matches.empty() ? 1 : 0;
}


static int cmdList( openocd::TclClient& aClient )
{
    auto taps = ReadChain( aClient );

    if( !taps )
    {
        printError( "scan_chain", taps.error() );
        return 1;
    }

    auto hasScan  = aClient.HasCommand( "jtag scan" );
    auto hasCurIr = aClient.HasCommand( "jtag cur_ir" );
    std::printf( "OpenOCD: jtag scan %s, jtag cur_ir %s\n\n",
                 hasScan && *hasScan ? "yes" : "no", hasCurIr && *hasCurIr ? "yes" : "no" );

    std::printf( "%-3s %-24s %-4s %-6s %-10s %-10s %s\n", "#", "TAP", "IR", "on", "IDCODE",
                 "expected", "cur IR" );

    for( const TapInfo& t : *taps )
    {
        char cur[32] = "-";   // '-': OpenOCD without 'jtag cur_ir'

        if( t.curIr )
            std::snprintf( cur, sizeof( cur ), "0x%llx", static_cast<unsigned long long>( *t.curIr ) );

        std::printf( "%-3d %-24s %-4d %-6s 0x%08x 0x%08x %s\n", t.index, t.name.c_str(),
                     t.irLength, t.enabled ? "yes" : "no", t.idcode,
                     t.expectedIds.empty() ? 0u : t.expectedIds[0], cur );
    }

    return 0;
}


struct Setup
{
    TapInfo                           tap;
    std::shared_ptr<const BsdlDevice> device;
};


static std::optional<Setup> prepare( openocd::TclClient& aClient, const std::string& aTap,
                                     const std::string& aBsdl, bool aVerbose )
{
    auto taps = ReadChain( aClient );

    if( !taps )
    {
        printError( "scan_chain", taps.error() );
        return std::nullopt;
    }

    const TapInfo* tap = findTap( *taps, aTap );

    if( !tap )
    {
        std::cerr << "TAP '" << aTap << "' not found, see 'bscan-chain list'\n";
        return std::nullopt;
    }

    auto dev = BsdlDevice::Load( aBsdl );

    if( !dev )
    {
        std::cerr << dev.error() << "\n";
        return std::nullopt;
    }

    size_t errors = 0, warnings = 0;

    for( const auto& d : ( *dev )->Diagnostics() )
        ( d.error ? errors : warnings )++;

    TapCheck check = CheckTap( *tap, **dev );

    if( aVerbose || !check.ok )
    {
        std::cout << "TAP " << tap->name << " <-> " << ( *dev )->EntityName() << " ("
                  << aBsdl << ")\n";

        if( errors || warnings )
            std::cout << "  BSDL: " << errors << " error(s), " << warnings << " warning(s)\n";

        for( const std::string& m : check.messages )
            std::cout << "  " << m << "\n";

        std::cout << "  pin maps:";

        for( const std::string& m : ( *dev )->PinMapNames() )
            std::cout << " " << m << ( m == ( *dev )->DefaultPinMap() ? " (default)" : "" );

        std::cout << "\n  boundary length: " << ( *dev )->BoundaryLength() << "\n";
    }

    if( !check.ok )
    {
        std::cerr << "TAP and BSDL do not match\n";
        return std::nullopt;
    }

    return Setup{ *tap, *dev };
}


static void printPins( const Bits& aBits, const BsdlDevice& aDev, const std::string& aMap,
                       const std::vector<std::string>& aPins )
{
    std::printf( "%-8s %-16s %-3s %s\n", "pin", "port", "in", "out" );

    for( const std::string& pin : aPins )
    {
        auto cells = aDev.LookupPin( aMap, pin );

        if( !cells )
        {
            std::printf( "%-8s (not in pin map)\n", pin.c_str() );
            continue;
        }

        if( !cells->HasCells() )
        {
            std::printf( "%-8s %-16s (no boundary cells)\n", pin.c_str(), cells->port.c_str() );
            continue;
        }

        PinValue v = DecodePin( aBits, *cells );
        std::printf( "%-8s %-16s %-3s %s\n", pin.c_str(), cells->port.c_str(),
                     v.input ? ( *v.input ? "1" : "0" ) : "-", ToString( v.output ) );
    }
}


static int cmdSample( openocd::TclClient& aClient, const Setup& aSetup, const std::string& aMap,
                      int aWatchMs, std::vector<std::string> aPins )
{
    const BsdlDevice& dev = *aSetup.device;

    if( !aMap.empty() && dev.PinNames( aMap ).empty() )
    {
        std::cerr << "pin map '" << aMap << "' not found\n";
        return 1;
    }

    bool allPins = aPins.empty();

    if( allPins )
    {
        for( const std::string& p : dev.PinNames( aMap ) )
        {
            auto c = dev.LookupPin( aMap, p );

            if( c && c->HasCells() )
                aPins.push_back( p );
        }
    }

    std::signal( SIGINT, []( int ) { g_stop = true; } );

    Bits last;
    bool first = true;

    do
    {
        auto t0   = std::chrono::steady_clock::now();
        auto bits = Sample( aClient, aSetup.tap, dev );

        if( !bits )
        {
            printError( "sample", bits.error() );

            if( aWatchMs <= 0 )
                return 1;
        }
        else if( first || *bits != last )
        {
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0 ).count();

            std::cout << "boundary 0x" << bits->ToHex() << "  (" << ms << " ms)\n";
            printPins( *bits, dev, aMap, aPins );
            std::cout << std::flush;
            last  = *bits;
            first = false;
        }

        if( aWatchMs > 0 )
            std::this_thread::sleep_for( std::chrono::milliseconds( aWatchMs ) );

    } while( aWatchMs > 0 && !g_stop );

    return 0;
}


int main( int argc, char** argv )
{
    openocd::TclClient::Config cfg;
    std::vector<std::string>   args;
    std::string                map;
    int                        watchMs = 0;

    for( int i = 1; i < argc; ++i )
    {
        std::string a = argv[i];
        auto        next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };

        if( a == "--host" )
            cfg.host = next();
        else if( a == "--port" )
            cfg.port = std::atoi( next().c_str() );
        else if( a == "--map" )
            map = next();
        else if( a == "--watch" )
            watchMs = std::atoi( next().c_str() );
        else if( a == "-h" || a == "--help" )
            return usage();
        else if( a.starts_with( "-" ) )
        {
            std::cerr << "unknown option '" << a << "'\n";
            return usage();
        }
        else
            args.push_back( a );
    }

    if( args.empty() )
        return usage();

    openocd::TclClient client( cfg );

    if( args[0] == "list" && args.size() == 1 )
        return cmdList( client );

    if( args[0] == "find-bsdl" && args.size() >= 3 )
        return cmdFindBsdl( client, args[1], std::vector<std::string>( args.begin() + 2, args.end() ) );

    if( ( args[0] == "check" && args.size() == 3 ) || ( args[0] == "sample" && args.size() >= 3 ) )
    {
        auto setup = prepare( client, args[1], args[2], args[0] == "check" );

        if( !setup )
            return 1;

        if( args[0] == "check" )
            return 0;

        return cmdSample( client, *setup, map, watchMs,
                          std::vector<std::string>( args.begin() + 3, args.end() ) );
    }

    return usage();
}
