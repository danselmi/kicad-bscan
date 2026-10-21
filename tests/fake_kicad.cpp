// SPDX-License-Identifier: GPL-3.0-only
// Minimal stand-in for KiCad's API server, for testing without KiCad.
//
//   fake-kicad <socket-url> [token] [--kicad9]
//
// Answers GetVersion, GetOpenDocuments, GetSelection, GetItems (footprints)
// and GetItemsById for a board "demo.kicad_pcb". The selection changes every
// second through a fixed list of scenarios, and every 7th request is answered
// with AS_BUSY. --kicad9 behaves like KiCad 9: pads carry no parent field.

#include <chrono>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include <nng/nng.h>
#include <nng/protocol/reqrep0/rep.h>

#include <board/board_types.pb.h>
#include <common/commands/base_commands.pb.h>
#include <common/commands/editor_commands.pb.h>
#include <common/envelope.pb.h>

namespace cmd   = kiapi::common::commands;
namespace types = kiapi::common::types;
namespace board = kiapi::board::types;
using kiapi::common::ApiRequest;
using kiapi::common::ApiResponse;
using kiapi::common::ApiStatusCode;

static const char* BOARD = "demo.kicad_pcb";

struct FakePad
{
    const char* id;
    const char* number;
    const char* net;
    const char* footprintId;
};

struct FakeFootprint
{
    const char* id;
    const char* reference;
};

static const FakeFootprint FOOTPRINTS[] = {
    { "fp-u3", "U3" },
    { "fp-u7", "U7" },
};

static const FakePad PAD_A12 = { "pad-a12", "A12", "/SPI_CLK", "fp-u3" };
static const FakePad PAD_B3  = { "pad-b3", "B3", "", "fp-u3" };
static const FakePad PAD_5   = { "pad-5", "5", "GND", "fp-u7" };

static const FakePad ALL_PADS[] = { PAD_A12, PAD_B3, PAD_5 };

struct Scenario
{
    std::vector<FakePad> pads;
    const char*          footprint = nullptr;   // selected footprint id
};

// Selection scenarios, cycled once per second
static const std::vector<Scenario> SCENARIOS = {
    { {} },
    { { PAD_A12 } },
    { { PAD_B3 } },
    { { PAD_A12, PAD_B3 } },
    { { PAD_5 } },
    { {}, "fp-u7" },
};

static bool g_kicad9 = false;


static board::FootprintInstance makeFootprint( const FakeFootprint& f, bool aWithPads )
{
    board::FootprintInstance fp;
    fp.mutable_id()->set_value( f.id );
    fp.mutable_reference_field()->mutable_text()->mutable_text()->set_text( f.reference );

    if( aWithPads )
    {
        for( const FakePad& p : ALL_PADS )
        {
            if( std::string( p.footprintId ) != f.id )
                continue;

            board::Pad pad;
            pad.mutable_id()->set_value( p.id );
            pad.set_number( p.number );
            pad.mutable_net()->set_name( p.net );
            fp.mutable_definition()->add_items()->PackFrom( pad );
        }
    }

    return fp;
}


static bool docOk( const types::ItemHeader& aHeader )
{
    return aHeader.document().type() == types::DocumentType::DOCTYPE_PCB
           && aHeader.document().board_filename() == BOARD;
}


int main( int argc, char** argv )
{
    if( argc < 2 )
    {
        std::cerr << "usage: fake-kicad <socket-url> [token]\n";
        return 2;
    }

    std::string token;

    for( int i = 2; i < argc; ++i )
    {
        if( std::string( argv[i] ) == "--kicad9" )
            g_kicad9 = true;
        else
            token = argv[i];
    }

    nng_socket sock;

    if( int rv = nng_rep0_open( &sock ) )
    {
        std::cerr << "nng_rep0_open: " << nng_strerror( rv ) << "\n";
        return 1;
    }

    if( int rv = nng_listen( sock, argv[1], nullptr, 0 ) )
    {
        std::cerr << "listen " << argv[1] << ": " << nng_strerror( rv ) << "\n";
        return 1;
    }

    std::cerr << "fake-kicad listening on " << argv[1] << "\n";

    auto     start = std::chrono::steady_clock::now();
    unsigned count = 0;

    for( ;; )
    {
        char*  buf = nullptr;
        size_t len = 0;

        if( nng_recv( sock, &buf, &len, NNG_FLAG_ALLOC ) != 0 )
            continue;

        ApiRequest req;
        bool       ok = req.ParseFromArray( buf, static_cast<int>( len ) );
        nng_free( buf, len );

        ApiResponse resp;
        resp.mutable_header()->set_kicad_token( token );
        auto* st = resp.mutable_status();
        st->set_status( ApiStatusCode::AS_OK );

        ++count;
        auto        secs     = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start ).count();
        const auto& selected = SCENARIOS[secs % SCENARIOS.size()];

        if( !ok )
        {
            st->set_status( ApiStatusCode::AS_BAD_REQUEST );
        }
        else if( !token.empty() && !req.header().kicad_token().empty()
                 && req.header().kicad_token() != token )
        {
            st->set_status( ApiStatusCode::AS_TOKEN_MISMATCH );
        }
        else if( count % 7 == 0 )
        {
            st->set_status( ApiStatusCode::AS_BUSY );
            st->set_error_message( "KiCad is busy and cannot respond to API requests right now" );
        }
        else if( req.message().Is<cmd::GetVersion>() )
        {
            cmd::GetVersionResponse r;
            r.mutable_version()->set_major( 10 );
            r.mutable_version()->set_minor( 0 );
            r.mutable_version()->set_patch( 7 );
            r.mutable_version()->set_full_version( "10.0.7 (fake)" );
            resp.mutable_message()->PackFrom( r );
        }
        else if( req.message().Is<cmd::GetOpenDocuments>() )
        {
            cmd::GetOpenDocumentsResponse r;
            auto* d = r.add_documents();
            d->set_type( types::DocumentType::DOCTYPE_PCB );
            d->set_board_filename( BOARD );
            d->mutable_project()->set_name( "demo" );
            d->mutable_project()->set_path( "/home/user/demo" );
            resp.mutable_message()->PackFrom( r );
        }
        else if( req.message().Is<cmd::GetSelection>() )
        {
            cmd::GetSelection r;
            req.message().UnpackTo( &r );

            if( !docOk( r.header() ) )
            {
                st->set_status( ApiStatusCode::AS_UNHANDLED );
            }
            else
            {
                cmd::SelectionResponse out;

                for( const FakePad& p : selected.pads )
                {
                    board::Pad pad;
                    pad.mutable_id()->set_value( p.id );
                    pad.set_number( p.number );
                    pad.mutable_net()->set_name( p.net );

                    if( !g_kicad9 )
                        pad.mutable_parent()->set_value( p.footprintId );

                    out.add_items()->PackFrom( pad );
                }

                if( selected.footprint )
                {
                    for( const FakeFootprint& f : FOOTPRINTS )
                        if( std::string( f.id ) == selected.footprint )
                            out.add_items()->PackFrom( makeFootprint( f, true ) );
                }

                resp.mutable_message()->PackFrom( out );
            }
        }
        else if( req.message().Is<cmd::GetItems>() )
        {
            cmd::GetItems r;
            req.message().UnpackTo( &r );

            if( !docOk( r.header() ) )
            {
                st->set_status( ApiStatusCode::AS_UNHANDLED );
            }
            else
            {
                cmd::GetItemsResponse out;

                for( int t : r.types() )
                {
                    if( t == types::KiCadObjectType::KOT_PCB_FOOTPRINT )
                        for( const FakeFootprint& f : FOOTPRINTS )
                            out.add_items()->PackFrom( makeFootprint( f, true ) );
                }

                out.set_status( types::ItemRequestStatus::IRS_OK );
                resp.mutable_message()->PackFrom( out );
            }
        }
        else if( req.message().Is<cmd::GetItemsById>() )
        {
            cmd::GetItemsById r;
            req.message().UnpackTo( &r );

            if( !docOk( r.header() ) )
            {
                st->set_status( ApiStatusCode::AS_UNHANDLED );
            }
            else
            {
                cmd::GetItemsResponse out;

                for( const auto& id : r.items() )
                {
                    for( const FakeFootprint& f : FOOTPRINTS )
                    {
                        if( id.value() == f.id )
                            out.add_items()->PackFrom( makeFootprint( f, true ) );
                    }
                }

                if( out.items_size() == 0 )
                {
                    st->set_status( ApiStatusCode::AS_BAD_REQUEST );
                    st->set_error_message( "none of the requested IDs were found or valid" );
                }
                else
                {
                    out.set_status( types::ItemRequestStatus::IRS_OK );
                    resp.mutable_message()->PackFrom( out );
                }
            }
        }
        else
        {
            st->set_status( ApiStatusCode::AS_UNHANDLED );
        }

        std::string out;
        resp.SerializeToString( &out );
        nng_send( sock, out.data(), out.size(), 0 );
    }
}
