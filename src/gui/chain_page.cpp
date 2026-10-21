// SPDX-License-Identifier: GPL-3.0-only
#include "chain_page.h"

#include <algorithm>
#include <cctype>
#include <set>

#include <wx/busyinfo.h>
#include <wx/button.h>
#include <wx/config.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/stdpaths.h>
#include <wx/tokenzr.h>
#include <wx/utils.h>

#include "bscan/tap_status.h"

#include "bsdl_dialogs.h"
#include "chain_table.h"
#include "extest_controller.h"
#include "gui_events.h"
#include "session.h"

using bscan::TapAssignment;
using bscan::TapInfo;

static const char* CFG_BSDL_DIRS = "BsdlDirs";   // user's BSDL folders, one per line

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


static wxString dash()
{
    return wxString::FromUTF8( "\u2013" );
}


static bool equalsNoCase( const std::string& a, const std::string& b )
{
    return a.size() == b.size()
           && std::equal( a.begin(), a.end(), b.begin(), []( unsigned char x, unsigned char y )
                          { return std::tolower( x ) == std::tolower( y ); } );
}


ChainPage::ChainPage( wxWindow* aParent, Session& aSession, ExtestController& aExtest ) :
        wxPanel( aParent ),
        m_session( aSession ),
        m_extest( aExtest )
{
    auto* sizer = new wxBoxSizer( wxVERTICAL );
    buildChainBox( this, sizer );
    SetSizer( sizer );

    wxStringTokenizer dirs( wxConfigBase::Get()->Read( CFG_BSDL_DIRS, wxEmptyString ), "\n" );

    while( dirs.HasMoreTokens() )
        if( wxString d = dirs.GetNextToken(); !d.empty() )
            m_userBsdlDirs.emplace_back( d.utf8_str() );
}


ChainPage::~ChainPage()
{
    StopBsdlScan();
}


void ChainPage::Rebuild()
{
    rebuildRows();
    Fill();
}


void ChainPage::EnableTakeSelection( bool aEnable )
{
    m_chainTable->EnableTakeSelection( aEnable );
}


void ChainPage::ShowNote( const wxString& aText )
{
    m_rowDetails->SetLabel( aText );
}


void ChainPage::assignmentsChanged()
{
    SendGuiEvent( this, EVT_ASSIGNMENTS_CHANGED, std::exchange( m_resample, false ) ? 1 : 0 );
}


void ChainPage::layoutChanged()
{
    SendGuiEvent( this, EVT_LAYOUT_CHANGED );
}


wxWindow* ChainPage::buildChainBox( wxWindow* aParent, wxSizer* aSizer )
{
    // On its own tab (titled "JTAG chain"): no box around it
    auto*     sizer = new wxBoxSizer( wxVERTICAL );
    wxWindow* box   = aParent;

    m_chainTable = new ChainTable( box );
    // No tooltip on the table itself: disabled controls in it would show it
    // Details and buttons follow the table directly (empty space below them)
    sizer->Add( m_chainTable, 0, wxEXPAND | wxALL, FromDIP( 4 ) );

    // Up to three lines of details for the row last worked on (full text in the tooltip)
    m_rowDetails = new wxStaticText( box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                     wxST_NO_AUTORESIZE );
    m_rowDetails->SetMinSize( wxSize( FromDIP( 100 ), 3 * m_rowDetails->GetCharHeight() ) );

    auto* bottom  = new wxBoxSizer( wxHORIZONTAL );
    auto* refresh = new wxButton( box, wxID_ANY, _( "Re-read chain" ) );
    auto* folders = new wxButton( box, wxID_ANY, _( "BSDL folders..." ) );
    folders->SetToolTip( _( "Folders searched for BSDL files matching the IDCODE of a TAP" ) );
    bottom->Add( m_rowDetails, 1, wxALIGN_TOP );
    bottom->Add( folders, 0, wxALIGN_BOTTOM | wxLEFT, FromDIP( 8 ) );
    bottom->Add( refresh, 0, wxALIGN_BOTTOM | wxLEFT, FromDIP( 4 ) );
    folders->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { onBsdlFolders(); } );
    sizer->Add( bottom, 0, wxEXPAND | wxALL, FromDIP( 4 ) );

    aSizer->Add( sizer, 1, wxEXPAND | wxALL, FromDIP( 4 ) );

    refresh->Bind( wxEVT_BUTTON, &ChainPage::onRefreshChain, this );

    auto text = []( const wxCommandEvent& aEvent )
    { return std::string( aEvent.GetString().utf8_str() ); };

    m_chainTable->Bind( EVT_CHAIN_REFERENCE_CHANGED, [this, text]( wxCommandEvent& aEvent )
                        { onReferenceChanged( aEvent.GetInt(), text( aEvent ) ); } );
    m_chainTable->Bind( EVT_CHAIN_TAKE_SELECTION, [this]( wxCommandEvent& aEvent )
                        { onTakeSelection( aEvent.GetInt() ); } );
    m_chainTable->Bind( EVT_CHAIN_PIN_MAP_CHANGED, [this, text]( wxCommandEvent& aEvent )
                        { onPinMapChanged( aEvent.GetInt(), text( aEvent ) ); } );
    m_chainTable->Bind( EVT_CHAIN_EXTEST,
                        [this]( wxCommandEvent& aEvent )
                        {
                            int r = aEvent.GetInt();

                            if( r >= 0 && r < static_cast<int>( m_rows.size() ) )
                                m_extest.Setup( m_rows[r].tap.name );
                        } );
    m_chainTable->Bind( EVT_CHAIN_FORGET,
                        [this]( wxCommandEvent& aEvent ) { onForgetTap( aEvent.GetInt() ); } );
    m_chainTable->Bind( EVT_CHAIN_SUGGEST,
                        [this]( wxCommandEvent& aEvent ) { onSuggestBsdl( aEvent.GetInt() ); } );
    m_chainTable->Bind( EVT_CHAIN_ROW_FOCUSED,
                        [this]( wxCommandEvent& aEvent )
                        {
                            m_detailRow = aEvent.GetInt();
                            showRowDetails( m_detailRow );
                        } );
    m_chainTable->OnBsdlChosen = [this]( int r, const std::string& p )
    { return onBsdlChosen( r, p ); };
    return box;
}


void ChainPage::onRefreshChain( wxCommandEvent& )
{
    m_session.sampler.RefreshChain();

    if( m_session.watcher )
        m_session.watcher->RefreshReferences();
}


void ChainPage::rebuildRows()
{
    m_rows.clear();

    if( m_session.chain.connected )
    {
        for( const TapInfo& t : m_session.chain.taps )
            m_rows.push_back( { t, true } );
    }

    // Assignments for TAPs that are not in the (current) chain stay visible
    for( const TapAssignment& a : m_session.config.taps )
    {
        bool inChain = std::any_of( m_rows.begin(), m_rows.end(),
                                    [&]( const ChainRow& r ) { return r.tap.name == a.tap; } );

        if( !inChain )
        {
            TapInfo t;
            t.name   = a.tap;
            t.idcode = a.idcode;
            m_rows.push_back( { t, false } );
        }
    }
}


bscan::TapFacts ChainPage::factsFor( const ChainRow& aRow )
{
    const TapAssignment* a    = m_session.config.Find( aRow.tap.name );
    const auto&          refs = m_session.board.references;

    bscan::TapFacts f;
    f.present   = aRow.present;
    f.connected = m_session.chain.connected;
    f.assigned  = a && !a->bsdl.empty();
    f.device    = f.assigned ? m_session.DeviceFor( a->bsdl ).get() : nullptr;   // cached

    if( a )
        f.assignedIdcode = a->idcode;

    f.referenceUnknown = a && !a->reference.empty() && !refs.empty()
                         && std::ranges::find( refs, a->reference ) == refs.end();
    f.inExtest = m_session.InExtest( aRow.tap.name );
    f.held     = m_session.IsHeld( aRow.tap.name );

    if( f.held )
    {
        f.heldInstruction = m_session.HardwarePinControl( aRow.tap.name );
        f.canTakeOver     = m_session.CanTakeOver( aRow.tap.name );
    }

    f.elsewhere = m_session.PinControlElsewhere( aRow.tap.name );
    return f;
}


static wxString checkText( const bscan::TapStatus& aStatus, const bscan::TapFacts& aFacts )
{
    using Check = bscan::TapStatus::Check;

    switch( aStatus.check )
    {
    case Check::Bypass:
        return _( "BYPASS" );
    case Check::BsdlError:
        return _( "BSDL error" );
    case Check::NotInChain:
        return _( "not in chain" );
    case Check::Offline:
        return _( "offline" );
    case Check::Mismatch:
        return _( "mismatch" );
    case Check::IdcodeChanged:
        return _( "IDCODE changed" );
    case Check::UnknownReference:
        return _( "unknown reference" );
    case Check::Ok:
        return _( "OK" );
    case Check::Extest:
        return _( "EXTEST" );
    case Check::Held:
        return aFacts.heldInstruction ? fromUtf8( *aFacts.heldInstruction ) : _( "EXTEST?" );
    }

    return "?";
}


ChainTable::Row ChainPage::tableRow( const ChainRow& aRow )
{
    const TapAssignment*   a      = m_session.config.Find( aRow.tap.name );
    const bscan::TapFacts  facts  = factsFor( aRow );
    const bscan::TapStatus status = bscan::EvaluateTap( aRow.tap, facts );
    ChainTable::Row        v;

    v.tap     = aRow.tap.name;
    v.present = aRow.present;

    // Settings of a TAP OpenOCD does not have (connected: not only offline)
    v.forgettable = !aRow.present && m_session.chain.connected && a;
    v.ir          = aRow.present ? wxString::Format( "%d", aRow.tap.irLength ) : dash();

    if( a )
    {
        v.reference = a->reference;
        v.bsdl      = a->bsdl;
    }

    if( facts.device )
    {
        v.pinMaps = facts.device->PinMapNames();
        v.pinMap  = a->pinMap.empty() ? facts.device->DefaultPinMap() : a->pinMap;
    }

    v.check          = checkText( status, facts );
    v.problem        = status.problem;
    v.extestActive   = facts.inExtest;
    v.extestAllowed  = status.extestAllowed;
    v.extestTakeOver = status.takeOver;

    if( status.blocked )
        v.extestNote = m_session.chain.jtagScan
                               ? wxString::Format( _( "%s is in EXTEST, set before this window "
                                                      "was opened or by another program: take "
                                                      "it over or leave EXTEST first." ),
                                                   fromUtf8( facts.elsewhere ) )
                               : wxString::Format( _( "Only one device can be in EXTEST at a "
                                                      "time: %s is in EXTEST (OpenOCD puts the "
                                                      "other devices into BYPASS for every "
                                                      "scan)." ),
                                                   fromUtf8( facts.elsewhere ) );

    if( m_bsdlIndex && status.lookForBsdl )
        v.suggestions =
                static_cast<int>( m_bsdlIndex->Match( aRow.tap.idcode, aRow.tap.irLength ).size() );

    v.details = rowDetails( aRow );

    // A file is assigned but does not match: the field shows no hint
    if( v.suggestions > 0 && a && !a->bsdl.empty() )
        v.details << "\n"
                  << wxString::Format( wxPLURAL( "%d matching BSDL file found in your BSDL "
                                                 "folders: click \"...\".",
                                                 "%d matching BSDL files found in your BSDL "
                                                 "folders: click \"...\".",
                                                 v.suggestions ),
                                       v.suggestions );

    return v;
}


void ChainPage::Fill()
{
    std::vector<ChainTable::Row> rows;

    for( const ChainRow& row : m_rows )
        rows.push_back( tableRow( row ) );

    m_chainTable->SetInitialDirectory( fromUtf8( m_session.board.projectPath ) );
    m_chainTable->SetRows( rows );

    if( m_detailRow >= static_cast<int>( m_rows.size() ) )
        m_detailRow = m_rows.empty() ? -1 : 0;
    else if( m_detailRow < 0 && !m_rows.empty() )
        m_detailRow = 0;

    showRowDetails( m_detailRow );
    layoutChanged();
}


wxString ChainPage::rowDetails( const ChainRow& aRow )
{
    const TapAssignment* a    = m_session.config.Find( aRow.tap.name );
    wxString             text = fromUtf8( aRow.tap.name );

    if( aRow.present )
        text << wxString::Format( ": IDCODE 0x%08x", aRow.tap.idcode );

    if( a && !a->bsdl.empty() )
    {
        std::string err;
        auto        dev = m_session.DeviceFor( a->bsdl, &err );

        if( !dev )
        {
            text << "\n" << fromUtf8( err );
        }
        else
        {
            text << ", " << fromUtf8( dev->EntityName() );

            if( aRow.present )
                for( const std::string& m : bscan::CheckTap( aRow.tap, *dev ).messages )
                    text << "\n" << fromUtf8( m );

            if( a->idcode && aRow.tap.idcode && a->idcode != aRow.tap.idcode )
                text << wxString::Format( _( "\nassigned when IDCODE was 0x%08x" ), a->idcode );
        }
    }

    if( a && !a->reference.empty() && !m_session.board.references.empty()
        && std::none_of( m_session.board.references.begin(), m_session.board.references.end(),
                         [&]( const std::string& ref ) { return ref == a->reference; } ) )
    {
        text << wxString::Format( _( "\n%s is not on the board" ), fromUtf8( a->reference ) );
    }

    return text;
}


void ChainPage::showRowDetails( int aRow )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) )
    {
        m_rowDetails->SetLabel( m_rows.empty() ? _( "No TAPs: is OpenOCD running?" ) : wxString() );
        m_rowDetails->SetToolTip( wxEmptyString );
        return;
    }

    wxString text = rowDetails( m_rows[aRow] );
    m_rowDetails->SetLabel( text );
    m_rowDetails->SetToolTip( text );
}


void ChainPage::onReferenceChanged( int aRow, const std::string& aValue )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) )
        return;

    const ChainRow& row   = m_rows[aRow];
    std::string     value = aValue;

    // Use the board's spelling if it matches case-insensitively
    for( const std::string& ref : m_session.board.references )
    {
        if( equalsNoCase( ref, value ) )
        {
            value = ref;
            break;
        }
    }

    if( !value.empty() && !m_session.board.references.empty()
        && std::find( m_session.board.references.begin(), m_session.board.references.end(), value )
                   == m_session.board.references.end()
        && m_session.watcher )
    {
        m_session.watcher->RefreshReferences();   // maybe added after the last read
    }

    TapAssignment& a = m_session.AssignmentFor( row.tap );
    a.reference      = value;

    if( row.present && row.tap.idcode )
        a.idcode = row.tap.idcode;

    m_detailRow = aRow;
    m_session.Save();
    CallAfter(
            [this]
            {
                Fill();
                assignmentsChanged();
            } );   // not inside the control's event
}


void ChainPage::onPinMapChanged( int aRow, const std::string& aMap )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) )
        return;

    if( m_session.extest.contains( m_rows[aRow].tap.name ) )
    {
        wxMessageBox( _( "Leave EXTEST before changing the pin map." ), _( "Pin map" ),
                      wxOK | wxICON_WARNING, this );
        CallAfter( [this] { Fill(); } );   // restore the choice
        return;
    }

    TapAssignment& a   = m_session.AssignmentFor( m_rows[aRow].tap );
    auto           dev = m_session.DeviceFor( a.bsdl );
    a.pinMap           = ( dev && aMap == dev->DefaultPinMap() ) ? std::string() : aMap;

    m_detailRow = aRow;
    m_session.Save();
    CallAfter(
            [this, tap = a.tap]
            {
                Fill();
                assignmentsChanged();
                m_extest.WarnStaleSettings( tap );
            } );
}


void ChainPage::onTakeSelection( int aRow )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) )
        return;

    if( m_session.board.footprintRef.empty() )
    {
        wxMessageBox( _( "Select a footprint (or one of its pads) in the PCB editor first." ),
                      _( "Take selection" ), wxOK | wxICON_INFORMATION, this );
        return;
    }

    const ChainRow& row = m_rows[aRow];
    TapAssignment&  a   = m_session.AssignmentFor( row.tap );
    a.reference         = m_session.board.footprintRef;

    if( row.present && row.tap.idcode )
        a.idcode = row.tap.idcode;

    m_detailRow = aRow;
    m_session.Save();
    CallAfter(
            [this]
            {
                Fill();
                assignmentsChanged();
            } );
}


bool ChainPage::onBsdlChosen( int aRow, const std::string& aPath )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) )
        return false;

    if( m_session.extest.contains( m_rows[aRow].tap.name ) )
    {
        wxMessageBox( _( "Leave EXTEST before changing the BSDL file." ), _( "BSDL file" ),
                      wxOK | wxICON_WARNING, this );
        return false;
    }

    const ChainRow row = m_rows[aRow];   // copy: dialogs below run an event loop

    // Empty: remove the assignment (starting states and reference are kept)
    if( aPath.empty() )
    {
        if( TapAssignment* a = m_session.config.Find( row.tap.name ); a && !a->bsdl.empty() )
        {
            a->bsdl.clear();
            a->pinMap.clear();
            m_resample  = true;
            m_detailRow = aRow;
            m_session.Save();
        }

        CallAfter(
                [this]
                {
                    Fill();
                    assignmentsChanged();
                } );
        return true;
    }

    // Always re-read a chosen file (it may have been edited)
    m_session.ForgetDevice( aPath );

    std::string err;
    auto        dev = m_session.DeviceFor( aPath, &err );

    if( !dev )
    {
        wxMessageBox(
                wxString::Format( _( "The BSDL file could not be read:\n%s" ), fromUtf8( err ) ),
                _( "BSDL file" ), wxOK | wxICON_ERROR, this );
        return false;
    }

    if( row.present )
    {
        bscan::TapCheck check = bscan::CheckTap( row.tap, *dev );

        if( !check.ok )
        {
            wxString msg;

            for( const std::string& m : check.messages )
                msg << fromUtf8( m ) << "\n";

            if( wxMessageBox( wxString::Format( _( "%s does not match TAP %s:\n\n%s\n"
                                                   "Assign it anyway?" ),
                                                fromUtf8( dev->EntityName() ),
                                                fromUtf8( row.tap.name ), msg ),
                              _( "BSDL file" ), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this )
                != wxYES )
            {
                return false;
            }
        }
    }

    TapAssignment& a = m_session.AssignmentFor( row.tap );

    if( row.present && row.tap.idcode )
        a.idcode = row.tap.idcode;

    // Same file re-read: keep the chosen pin map if the file still has it,
    // otherwise use the default pin map of the (new) file
    const std::vector<std::string> maps    = dev->PinMapNames();
    const bool                     keepMap = a.bsdl == aPath && !a.pinMap.empty()
                         && std::ranges::find( maps, a.pinMap ) != maps.end();

    a.bsdl = aPath;

    if( !keepMap )
        a.pinMap.clear();

    m_resample  = true;   // sample with the newly read device
    m_detailRow = aRow;
    m_session.Save();
    CallAfter(
            [this, tap = a.tap]
            {
                Fill();
                assignmentsChanged();
                m_extest.WarnStaleSettings( tap );
            } );
    return true;
}


void ChainPage::onForgetTap( int aRow )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) || m_rows[aRow].present )
        return;

    const std::string    name = m_rows[aRow].tap.name;   // copy: the dialog runs an event loop
    const TapAssignment* a    = m_session.config.Find( name );

    if( !a )
        return;

    wxString what = fromUtf8( name );

    if( !a->reference.empty() )
        what << " (" << fromUtf8( a->reference ) << ")";

    wxString text = wxString::Format(
            _( "%s is not in OpenOCD's chain. Remove its settings from the project?\n\n"
               "Reference, BSDL file and pin map, and the EXTEST starting states are removed. "
               "The watch list is not changed." ),
            what );

    if( !a->bsdl.empty() )
        text << "\n\n" << _( "BSDL file:" ) << " " << fromUtf8( a->bsdl );

    if( wxMessageBox( text, _( "Forget TAP" ), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this )
        != wxYES )
        return;

    std::erase_if( m_session.config.taps, [&]( const TapAssignment& t ) { return t.tap == name; } );
    std::erase( m_session.config.extestLeft, name );
    m_session.config.extestImages.erase( name );
    m_resample  = true;
    m_detailRow = -1;
    m_session.Save();

    CallAfter(
            [this]
            {
                rebuildRows();
                Fill();
                assignmentsChanged();
            } );
}


void ChainPage::StartBsdlScan()
{
    if( m_scanRunning )
    {
        m_scanAgain = true;   // folders changed while scanning
        return;
    }

    std::vector<std::string> dirs = m_session.config.bsdlDirs;
    dirs.insert( dirs.end(), m_userBsdlDirs.begin(), m_userBsdlDirs.end() );

    if( dirs.empty() )
    {
        m_bsdlIndex.reset();
        m_scanStatus = _( "No BSDL folders set." );
        Fill();
        return;
    }

    if( m_scanThread.joinable() )
        m_scanThread.join();

    wxFileName cache( wxStandardPaths::Get().GetUserDir( wxStandardPaths::Dir_Cache ),
                      wxEmptyString );
    cache.AppendDir( "kicad-bscan" );
    cache.SetFullName( "bsdl-index.json" );
    std::string cacheFile( cache.GetFullPath().utf8_str() );

    m_scanRunning = true;
    m_scanStatus  = _( "Searching BSDL folders..." );
    m_scanCancel  = false;

    m_scanThread = std::thread(
            [this, dirs, cacheFile]
            {
                auto index = std::make_shared<bscan::BsdlIndex>();
                index->LoadCache( cacheFile );
                auto stats = index->Scan( dirs, [this] { return m_scanCancel.load(); } );

                if( m_scanCancel )
                    return;

                index->SaveCache( cacheFile );
                CallAfter( [this, index, stats] { onBsdlScanDone( index, stats ); } );
            } );
}


void ChainPage::StopBsdlScan()
{
    m_scanCancel = true;

    if( m_scanThread.joinable() )
        m_scanThread.join();

    m_scanRunning = false;
}


void ChainPage::onBsdlScanDone( std::shared_ptr<const bscan::BsdlIndex> aIndex,
                                const bscan::BsdlIndex::Stats&          aStats )
{
    if( m_scanThread.joinable() )
        m_scanThread.join();

    m_scanRunning = false;
    m_bsdlIndex   = std::move( aIndex );
    m_scanStatus  = aStats.failed
                            ? wxString::Format( _( "%zu BSDL files found (%zu could not be read)." ),
                                                aStats.files, aStats.failed )
                            : wxString::Format( _( "%zu BSDL files found." ), aStats.files );
    Fill();

    if( m_scanAgain )
    {
        m_scanAgain = false;
        StartBsdlScan();
    }
}


void ChainPage::onBsdlFolders()
{
    BsdlFoldersDialog dlg( this, m_userBsdlDirs, m_session.config.bsdlDirs,
                           m_session.projectFile.empty() ? std::string()
                                                         : m_session.board.projectPath,
                           m_scanStatus );

    if( dlg.ShowModal() != wxID_OK )
        return;

    m_userBsdlDirs = dlg.GetUserDirs();

    wxString stored;

    for( const std::string& d : m_userBsdlDirs )
        stored << fromUtf8( d ) << "\n";

    wxConfigBase::Get()->Write( CFG_BSDL_DIRS, stored );

    if( !m_session.projectFile.empty() && dlg.GetProjectDirs() != m_session.config.bsdlDirs )
    {
        m_session.config.bsdlDirs = dlg.GetProjectDirs();
        m_session.Save();
    }

    StartBsdlScan();
}


void ChainPage::onSuggestBsdl( int aRow )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_rows.size() ) || !m_bsdlIndex )
        return;

    const TapInfo        tap = m_rows[aRow].tap;
    const TapAssignment* a   = m_session.config.Find( tap.name );

    // Pads of the footprint, to rank the pin maps
    std::set<std::string> pads;

    if( a && !a->reference.empty() )
        if( auto it = m_session.board.padNets.find( a->reference );
            it != m_session.board.padNets.end() )
            for( const auto& [pad, net] : it->second )
                pads.insert( pad );

    wxBusyCursor busy;
    auto ranked = bscan::RankSuggestions( m_bsdlIndex->Match( tap.idcode, tap.irLength ), pads,
                                          m_session.config.bsdlDirs );

    if( ranked.empty() )
    {
        wxMessageBox( _( "The matching files could not be read any more; the BSDL folders are "
                         "searched again." ),
                      _( "BSDL file" ), wxOK | wxICON_INFORMATION, this );
        StartBsdlScan();
        return;
    }

    wxString label =
            a && !a->reference.empty()
                    ? wxString::Format( "%s (%s)", fromUtf8( a->reference ), fromUtf8( tap.name ) )
                    : fromUtf8( tap.name );

    BsdlChoice choice = ChooseBsdl( this, label, tap.idcode, ranked );

    if( choice.kind == BsdlChoice::Kind::OtherFile )
    {
        m_chainTable->BrowseFile( aRow );
        return;
    }

    if( choice.kind != BsdlChoice::Kind::Chosen || !onBsdlChosen( aRow, choice.suggestion.path ) )
        return;

    // onBsdlChosen() used the default pin map of the file; take the chosen one
    const std::string& map      = choice.suggestion.pinMap;
    TapAssignment&     assigned = m_session.AssignmentFor( tap );
    auto               dev      = m_session.DeviceFor( assigned.bsdl );
    assigned.pinMap             = ( dev && map == dev->DefaultPinMap() ) ? std::string() : map;
    m_session.Save();
}
