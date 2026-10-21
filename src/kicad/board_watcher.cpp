// SPDX-License-Identifier: GPL-3.0-only
#include "kicad/board_watcher.h"

#include <algorithm>

#include <board/board_types.pb.h>
#include <common/commands/editor_commands.pb.h>
#include <common/types/base_types.pb.h>
#include <common/types/enums.pb.h>
#include <google/protobuf/empty.pb.h>

namespace kicad
{


namespace cmd   = kiapi::common::commands;
namespace types = kiapi::common::types;
namespace board = kiapi::board::types;


// KiCad names the "net" of an unconnected pad "unconnected-(<ref>-<pin>-Pad<n>)";
// that is no connection, so it is reported as no net. ("Net-(...)" names of
// unnamed but connected nets are kept.)
static std::string netName( const board::Pad& aPad )
{
    const std::string& name = aPad.net().name();
    return name.starts_with( "unconnected-" ) ? std::string() : name;
}


// How often the open document is re-checked even if everything works
// (to notice another board being opened).
static constexpr auto DOCUMENT_RECHECK = std::chrono::seconds( 5 );

// Minimum time between two automatic reloads of all footprints (a pad that
// is not in the table triggers a reload; avoid doing that on every poll).
static constexpr auto FOOTPRINT_RELOAD_MIN = std::chrono::seconds( 3 );


const char* ToString( BoardState::Status aStatus )
{
    switch( aStatus )
    {
    case BoardState::Status::Disconnected:      return "disconnected";
    case BoardState::Status::NoBoard:           return "no board";
    case BoardState::Status::NothingSelected:   return "nothing selected";
    case BoardState::Status::MultiplePads:      return "multiple pads selected";
    case BoardState::Status::PadSelected:       return "pad selected";
    case BoardState::Status::FootprintSelected: return "footprint selected";
    }

    return "?";
}


BoardWatcher::BoardWatcher( Client::Config aConfig, std::chrono::milliseconds aInterval ) :
        m_client( std::move( aConfig ) ),
        m_interval( aInterval )
{
}


BoardWatcher::~BoardWatcher()
{
    Stop();
}


void BoardWatcher::Start( Callback aCallback )
{
    Stop();
    m_callback = std::move( aCallback );
    m_stop     = false;
    m_thread   = std::thread( [this] { run(); } );
}


void BoardWatcher::Stop()
{
    if( !m_thread.joinable() )
        return;

    {
        std::lock_guard lock( m_waitMutex );
        m_stop = true;
    }

    m_wake.notify_all();
    m_thread.join();
}


void BoardWatcher::RefreshReferences()
{
    m_refreshRefs = true;
    m_wake.notify_all();
}


void BoardWatcher::SelectNet( std::string aNet )
{
    {
        std::lock_guard lock( m_selectMutex );
        m_selectNet = std::move( aNet );
    }

    m_wake.notify_all();
}




BoardState BoardWatcher::PollOnce()
{
    // Retry a few times if KiCad is busy
    for( int i = 0; i < 10 && !poll( m_state ); ++i )
        std::this_thread::sleep_for( m_interval );

    return m_state;
}


void BoardWatcher::run()
{
    bool reported = false;

    while( !m_stop )
    {
        BoardState before  = m_state;
        bool       updated = poll( m_state );

        // Report the first real result, then only changes. A skipped cycle
        // (KiCad busy) leaves the state as it was.
        if( m_callback && updated && ( !reported || !( m_state == before ) ) )
        {
            m_callback( m_state );
            reported = true;
        }

        selectNet();

        std::unique_lock lock( m_waitMutex );
        m_wake.wait_for( lock, m_interval,
                         [this]
                         {
                             std::lock_guard sel( m_selectMutex );
                             return m_stop.load() || m_refreshRefs.load() || m_selectNet.has_value();
                         } );
    }
}


static void setDisconnected( BoardState& aState, const Error& aError )
{
    aState        = BoardState{};
    aState.status = BoardState::Status::Disconnected;

    if( aError.kind == ErrorKind::TokenMismatch )
        aState.detail = "another KiCad instance is answering (KiCad restarted?)";
    else
        aState.detail = aError.message;
}


static bool isConnectionError( const Error& aError )
{
    return aError.kind == ErrorKind::NotConnected || aError.kind == ErrorKind::Timeout
           || aError.kind == ErrorKind::TokenMismatch || aError.kind == ErrorKind::Other;
}


static bool isBusy( const Error& aError )
{
    return aError.kind == ErrorKind::Busy || aError.kind == ErrorKind::NotReady;
}


static void setBoardDocument( types::ItemHeader* aHeader, const std::string& aFile )
{
    types::DocumentSpecifier* doc = aHeader->mutable_document();
    doc->set_type( types::DocumentType::DOCTYPE_PCB );
    doc->set_board_filename( aFile );
}


static std::string referenceOf( const board::FootprintInstance& aFp )
{
    return aFp.reference_field().text().text().text();
}


// Returns false if this cycle should be skipped (KiCad busy).
bool BoardWatcher::refreshDocument( BoardState& aState )
{
    cmd::GetOpenDocuments req;
    req.set_type( types::DocumentType::DOCTYPE_PCB );

    auto resp = m_client.Call<cmd::GetOpenDocumentsResponse>( req );

    if( !resp )
    {
        const Error& e = resp.error();

        if( isBusy( e ) )
            return false;

        m_haveDocument = false;

        if( isConnectionError( e ) )
        {
            setDisconnected( aState, e );
        }
        else
        {
            // e.g. AS_UNHANDLED when the PCB editor is not open
            aState        = BoardState{};
            aState.status = BoardState::Status::NoBoard;
            aState.detail = e.message;
        }

        return true;
    }

    m_docCheckedAt = std::chrono::steady_clock::now();

    if( resp->documents_size() == 0 )
    {
        m_haveDocument = false;
        aState         = BoardState{};
        aState.status  = BoardState::Status::NoBoard;
        return true;
    }

    // KiCad 10 has a single PCB editor, so there is at most one board.
    const types::DocumentSpecifier& doc = resp->documents( 0 );

    if( !m_haveDocument || m_docFile != doc.board_filename() )
    {
        // Another board: forget everything about the previous one
        m_footprintsLoaded = false;
        m_padToFootprint.clear();
        m_footprintRefs.clear();
        m_references.clear();
        m_padNets.clear();
    }

    m_haveDocument   = true;
    m_docFile        = doc.board_filename();
    m_docProjectName = doc.has_project() ? doc.project().name() : std::string();
    m_docProjectPath = doc.has_project() ? doc.project().path() : std::string();
    return true;
}


bool BoardWatcher::loadFootprints( std::optional<Error>& aError )
{
    cmd::GetItems req;
    setBoardDocument( req.mutable_header(), m_docFile );
    req.add_types( types::KiCadObjectType::KOT_PCB_FOOTPRINT );

    auto resp = m_client.Call<cmd::GetItemsResponse>( req );

    if( !resp )
    {
        if( !isBusy( resp.error() ) )
            aError = resp.error();

        return false;
    }

    m_padToFootprint.clear();
    m_footprintRefs.clear();
    m_references.clear();
    m_padNets.clear();
    m_padIds.clear();

    for( const google::protobuf::Any& any : resp->items() )
    {
        board::FootprintInstance fp;

        if( !any.UnpackTo( &fp ) )
            continue;

        FootprintEntry entry{ fp.id().value(), referenceOf( fp ) };
        m_footprintRefs[entry.id] = entry.reference;
        m_references.push_back( entry.reference );

        // The footprint definition lists its pads (with their board KIIDs)
        for( const google::protobuf::Any& item : fp.definition().items() )
        {
            board::Pad pad;

            if( item.Is<board::Pad>() && item.UnpackTo( &pad ) )
            {
                m_padToFootprint[pad.id().value()] = entry;
                m_padNets[entry.reference][pad.number()] = netName( pad );
                m_padIds[{ entry.reference, pad.number() }] = pad.id().value();
            }
        }
    }

    std::sort( m_references.begin(), m_references.end() );
    m_references.erase( std::unique( m_references.begin(), m_references.end() ),
                        m_references.end() );

    m_footprintsLoaded   = true;
    m_footprintsLoadedAt = std::chrono::steady_clock::now();
    return true;
}


std::string BoardWatcher::referenceForPad( const std::string& aPadId,
                                           const std::string& aFootprintId, bool& aBusy,
                                           std::optional<Error>& aError )
{
    aBusy = false;

    auto lookup = [&]() -> std::optional<std::string>
    {
        if( !aFootprintId.empty() )
        {
            // KiCad >= 10.0.6: parent reported
            if( auto it = m_footprintRefs.find( aFootprintId ); it != m_footprintRefs.end() )
                return it->second;
        }
        else if( auto it = m_padToFootprint.find( aPadId ); it != m_padToFootprint.end() )
        {
            return it->second.reference;
        }

        return std::nullopt;
    };

    if( auto ref = lookup() )
        return *ref;

    // Unknown pad or footprint: the board was probably edited. Reload, but
    // not more often than FOOTPRINT_RELOAD_MIN.
    auto now = std::chrono::steady_clock::now();

    if( m_footprintsLoaded && now - m_footprintsLoadedAt < FOOTPRINT_RELOAD_MIN )
        return {};

    if( !loadFootprints( aError ) )
    {
        aBusy = !aError.has_value();
        return {};
    }

    return lookup().value_or( std::string() );
}


bool BoardWatcher::poll( BoardState& aState )
{
    auto now = std::chrono::steady_clock::now();

    if( !m_haveDocument || now - m_docCheckedAt > DOCUMENT_RECHECK )
    {
        if( !refreshDocument( aState ) )
            return false;   // busy: keep previous state

        if( !m_haveDocument )
            return true;    // state already set (disconnected / no board)
    }

    auto handleError = [&]( const Error& e ) -> bool
    {
        m_haveDocument = false;   // re-check the document next cycle

        if( isConnectionError( e ) )
        {
            setDisconnected( aState, e );
            return true;
        }

        // AS_UNHANDLED: the board changed or was closed -> re-check next cycle
        if( e.kind == ErrorKind::Unhandled )
            return false;

        aState        = BoardState{};
        aState.status = BoardState::Status::NoBoard;
        aState.detail = std::string( "query failed: " ) + ToString( e.kind ) + ": " + e.message;
        return true;
    };

    // All footprints once per board (reference list, KiCad 9 pad table) and
    // whenever the GUI asks for it
    if( !m_footprintsLoaded || m_refreshRefs.exchange( false ) )
    {
        std::optional<Error> err;

        if( !loadFootprints( err ) )
        {
            if( err )
                return handleError( *err );

            return false;   // busy
        }
    }

    cmd::GetSelection req;
    setBoardDocument( req.mutable_header(), m_docFile );
    req.add_types( types::KiCadObjectType::KOT_PCB_PAD );
    req.add_types( types::KiCadObjectType::KOT_PCB_FOOTPRINT );

    auto sel = m_client.Call<cmd::SelectionResponse>( req );

    if( !sel )
    {
        if( isBusy( sel.error() ) )
            return false;

        return handleError( sel.error() );
    }

    BoardState next;
    next.boardFile   = m_docFile;
    next.projectName = m_docProjectName;
    next.projectPath = m_docProjectPath;
    next.references  = m_references;
    next.padNets     = m_padNets;

    std::vector<board::Pad>               pads;
    std::vector<board::FootprintInstance> footprints;

    for( const google::protobuf::Any& any : sel->items() )
    {
        if( any.Is<board::Pad>() )
        {
            board::Pad pad;

            if( any.UnpackTo( &pad ) )
                pads.push_back( std::move( pad ) );
        }
        else if( any.Is<board::FootprintInstance>() )
        {
            board::FootprintInstance fp;

            if( any.UnpackTo( &fp ) )
                footprints.push_back( std::move( fp ) );
        }
    }

    next.padCount = static_cast<int>( pads.size() );

    if( pads.empty() )
    {
        if( footprints.size() == 1 )
        {
            next.status       = BoardState::Status::FootprintSelected;
            next.footprintRef = referenceOf( footprints[0] );
        }
        else
        {
            next.status = BoardState::Status::NothingSelected;
        }

        aState = next;
        return true;
    }

    // Resolve every selected pad (single pad and multi-selection alike)
    for( const board::Pad& pad : pads )
    {
        PadInfo info;
        info.padId       = pad.id().value();
        info.number      = pad.number();
        info.net         = netName( pad );
        info.footprintId = pad.parent().value();   // empty before KiCad 10.0.6

        bool                 busy = false;
        std::optional<Error> err;
        info.reference = referenceForPad( info.padId, info.footprintId, busy, err );

        if( busy )
            return false;   // try again next cycle, avoid showing a half-filled list

        if( err )
            return handleError( *err );

        if( info.footprintId.empty() )
        {
            if( auto it = m_padToFootprint.find( info.padId ); it != m_padToFootprint.end() )
                info.footprintId = it->second.id;
        }

        next.pads.push_back( std::move( info ) );
    }

    // The reload may have added references
    next.references = m_references;
    next.padNets    = m_padNets;

    if( pads.size() > 1 )
    {
        next.status = BoardState::Status::MultiplePads;
        aState      = next;
        return true;
    }

    next.pad = next.pads[0];

    if( next.pad.reference.empty() )
        next.detail = "footprint of this pad not found";

    next.footprintRef = next.pad.reference;
    next.status       = BoardState::Status::PadSelected;
    aState            = next;
    return true;
}

void BoardWatcher::selectNet()
{
    std::optional<std::string> net;

    {
        std::lock_guard lock( m_selectMutex );
        net.swap( m_selectNet );
    }

    if( !net || !m_haveDocument || !m_footprintsLoaded )
        return;

    cmd::ClearSelection clear;
    setBoardDocument( clear.mutable_header(), m_docFile );
    // Selecting is only a help for the user: errors are not reported
    (void) m_client.Call<google::protobuf::Empty>( clear );

    if( net->empty() )
        return;

    cmd::AddToSelection add;
    setBoardDocument( add.mutable_header(), m_docFile );

    // Pads of the net (from the footprints read before)
    for( const auto& [ref, pads] : m_padNets )
        for( const auto& [pad, name] : pads )
            if( name == *net )
                if( auto it = m_padIds.find( { ref, pad } ); it != m_padIds.end() )
                    add.add_items()->set_value( it->second );

    // Tracks, arcs and vias of the net (GetItemsByNet exists only in KiCad 10)
    cmd::GetItems req;
    setBoardDocument( req.mutable_header(), m_docFile );
    req.add_types( types::KiCadObjectType::KOT_PCB_TRACE );
    req.add_types( types::KiCadObjectType::KOT_PCB_ARC );
    req.add_types( types::KiCadObjectType::KOT_PCB_VIA );

    if( auto resp = m_client.Call<cmd::GetItemsResponse>( req ) )
    {
        for( const google::protobuf::Any& any : resp->items() )
        {
            board::Track track;
            board::Arc   arc;
            board::Via   via;

            if( any.Is<board::Track>() && any.UnpackTo( &track ) && track.net().name() == *net )
                add.add_items()->set_value( track.id().value() );
            else if( any.Is<board::Arc>() && any.UnpackTo( &arc ) && arc.net().name() == *net )
                add.add_items()->set_value( arc.id().value() );
            else if( any.Is<board::Via>() && any.UnpackTo( &via ) && via.net().name() == *net )
                add.add_items()->set_value( via.id().value() );
        }
    }

    if( add.items_size() > 0 )
        (void) m_client.Call<cmd::SelectionResponse>( add );
}


} // namespace kicad
