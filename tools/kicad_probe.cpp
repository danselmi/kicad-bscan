// SPDX-License-Identifier: GPL-3.0-only
// Command line test for the KiCad connection, without GUI.
//
//   bscan-kicad-probe            print KiCad version and the current pad once
//   bscan-kicad-probe --watch    print every change of the selected pad (Ctrl+C to stop)
//
// Uses KICAD_API_SOCKET / KICAD_API_TOKEN if set, otherwise the default socket.

#include <atomic>
#include <csignal>
#include <cstring>
#include <iostream>
#include <thread>

#include <common/commands/base_commands.pb.h>

#include "kicad/board_watcher.h"
#include "kicad/ipc_client.h"

static std::atomic<bool> g_stop{ false };


static void print( const kicad::BoardState& aState )
{
    std::cout << "[" << kicad::ToString( aState.status ) << "]";

    if( !aState.boardFile.empty() )
        std::cout << " board=" << aState.boardFile;

    if( aState.status == kicad::BoardState::Status::MultiplePads )
    {
        std::cout << " count=" << aState.padCount << " pads=";

        for( const kicad::PadInfo& p : aState.pads )
            std::cout << ( p.reference.empty() ? "?" : p.reference ) << "." << p.number << " ";
    }

    if( aState.status == kicad::BoardState::Status::FootprintSelected )
        std::cout << " ref=" << aState.footprintRef;

    if( aState.status == kicad::BoardState::Status::PadSelected )
    {
        std::cout << " ref=" << ( aState.pad.reference.empty() ? "?" : aState.pad.reference )
                  << " pad=" << aState.pad.number
                  << " net=" << ( aState.pad.net.empty() ? "<none>" : aState.pad.net );
    }

    if( !aState.references.empty() )
        std::cout << " [" << aState.references.size() << " footprints]";

    if( !aState.detail.empty() )
        std::cout << "  (" << aState.detail << ")";

    std::cout << std::endl;
}


int main( int argc, char** argv )
{
    bool watch = argc > 1 && std::strcmp( argv[1], "--watch" ) == 0;

    kicad::Client::Config cfg = kicad::Client::ConfigFromEnvironment( "bscan-kicad-probe" );
    std::cout << "socket: " << cfg.socketUrl
              << ( cfg.token.empty() ? " (no token)" : " (token set)" ) << std::endl;

    {
        kicad::Client client( cfg );
        auto ver = client.Call<kiapi::common::commands::GetVersionResponse>(
                kiapi::common::commands::GetVersion() );

        if( !ver )
        {
            std::cerr << "GetVersion failed: " << kicad::ToString( ver.error().kind ) << ": "
                      << ver.error().message << std::endl;
            return 1;
        }

        std::cout << "KiCad " << ver->version().full_version() << std::endl;
    }

    kicad::BoardWatcher watcher( cfg, std::chrono::milliseconds( 250 ) );

    if( !watch )
    {
        print( watcher.PollOnce() );
        return 0;
    }

    std::signal( SIGINT, []( int ) { g_stop = true; } );

    watcher.Start( []( const kicad::BoardState& s ) { print( s ); } );

    while( !g_stop )
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );

    watcher.Stop();
    return 0;
}
