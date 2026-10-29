// SPDX-License-Identifier: GPL-3.0-only
#include "main_frame.h"

#include <algorithm>
#include <utility>

#include <wx/button.h>
#include <wx/config.h>
#include <wx/filedlg.h>
#include <wx/filename.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/persist/toplevel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textdlg.h>
#include <wx/tglbtn.h>

#include "chain_page.h"
#include "chip_view.h"
#include "extest_banner.h"
#include "gui_events.h"
#include "interconnect_page.h"
#include "pins_page.h"
#include "svg_icons.h"

using bscan::LiveSampler;
using bscan::TapAssignment;
using Status = kicad::BoardState::Status;

// How often the KiCad selection is polled
static constexpr std::chrono::milliseconds SELECTION_POLL{ 250 };

static const char* CFG_STAY_ON_TOP = "StayOnTop";
static const char* CFG_INTERVAL    = "SampleIntervalMs";   // also PinsPage
static const char* CFG_TAB         = "Tab";                // tab shown last
static const char* CFG_SETTINGS_FILE = "SettingsFile";     // without KiCad project

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


static std::string toUtf8( const wxString& aText )
{
    return std::string( aText.utf8_str() );
}


// "Keep window on top" button: pin symbol, upright and red while pressed.
// The symbol when not pressed follows the light or dark theme.
static wxBitmapToggleButton* newStayOnTopButton( wxWindow* aParent )
{
    auto svg = []( const unsigned char* aData, size_t aSize )
    { return wxBitmapBundle::FromSVG( aData, aSize, wxSize( 16, 16 ) ); };

    auto offSymbol = [svg]
    {
        return wxSystemSettings::GetAppearance().IsDark()
                       ? svg( s_svg_pin_off_dark, sizeof( s_svg_pin_off_dark ) )
                       : svg( s_svg_pin_off_light, sizeof( s_svg_pin_off_light ) );
    };

    auto* button = new wxBitmapToggleButton( aParent, wxID_ANY, offSymbol() );
    button->SetBitmapPressed( svg( s_svg_pin, sizeof( s_svg_pin ) ) );
    button->SetToolTip( _( "Keep this window on top of other windows.\n"
                           "Note: some Wayland desktops (e.g. GNOME) ignore this request." ) );
    button->Bind( wxEVT_SYS_COLOUR_CHANGED,
                  [button, offSymbol]( wxSysColourChangedEvent& aEvent )
                  {
                      button->SetBitmap( offSymbol() );
                      aEvent.Skip();
                  } );
    return button;
}


// ============================================================================
// Construction
// ============================================================================

MainFrame::MainFrame( const kicad::Client::Config& aKiCadConfig ) :
        wxFrame( nullptr, wxID_ANY, _( "Boundary Scan" ) ),
        m_session( this, m_sampler ),
        m_extestCtl( this, m_session )
{
    m_panel   = new wxPanel( this );
    auto* top = new wxBoxSizer( wxVERTICAL );

    buildConnectionLine( top );

    // --- EXTEST bar (hidden while no device is in EXTEST) -----------------------
    m_extestBanner = new ExtestBanner( m_panel, m_session, m_extestCtl );
    top->Add( m_extestBanner, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    // --- tabs ---------------------------------------------------------------------
    m_tabs      = new wxNotebook( m_panel, wxID_ANY );
    m_chainPage = new ChainPage( m_tabs, m_session, m_extestCtl );
    m_pinsPage  = new PinsPage( m_tabs, m_session, m_extestCtl );
    m_icPage    = new InterconnectPage( m_tabs, m_session, m_extestCtl );
    m_tabs->AddPage( m_chainPage, _( "JTAG chain" ) );
    m_tabs->AddPage( m_pinsPage, _( "Pins" ) );
    m_tabs->AddPage( m_icPage, _( "Interconnect test" ) );
    top->Add( m_tabs, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    long tab = wxConfigBase::Get()->ReadLong( CFG_TAB, 0 );

    if( tab >= 0 && tab < static_cast<long>( m_tabs->GetPageCount() ) )
        m_tabs->SetSelection( static_cast<size_t>( tab ) );

    m_panel->SetSizer( top );

    auto* frameSizer = new wxBoxSizer( wxVERTICAL );
    frameSizer->Add( m_panel, 1, wxEXPAND );
    SetSizerAndFit( frameSizer );
    SetSize( wxSize( std::max( GetSize().x, FromDIP( 520 ) ), -1 ) );

    // Restores position and size from the last session
    wxPersistentRegisterAndRestore( this, "MainFrame" );
    ensureFits();

    bool onTop = wxConfigBase::Get()->ReadBool( CFG_STAY_ON_TOP, false );
    m_pin->SetValue( onTop );
    applyStayOnTop( onTop );

    connectParts();
    m_chainPage->Rebuild();
    m_chainPage->StartBsdlScan();
    startWorkers( aKiCadConfig );
}


void MainFrame::buildConnectionLine( wxSizer* aSizer )
{
    auto* conn = new wxFlexGridSizer( 3, FromDIP( 2 ), FromDIP( 6 ) );
    conn->AddGrowableCol( 0 );

    m_kicadStatus = new wxStaticText( m_panel, wxID_ANY, _( "KiCad: connecting..." ) );
    m_pin         = newStayOnTopButton( m_panel );
    auto* chips   = new wxButton( m_panel, wxID_ANY, _( "Chip view..." ), wxDefaultPosition,
                                  wxDefaultSize, wxBU_EXACTFIT );
    chips->SetToolTip( _( "A chip of the chain drawn from its BSDL file, with the live state of "
                          "its pins (also without a board)" ) );
    m_settingsFile = new wxButton( m_panel, wxID_ANY, _( "Settings file..." ), wxDefaultPosition,
                                   wxDefaultSize, wxBU_EXACTFIT );
    m_settingsFile->SetToolTip( _( "Without a KiCad project the settings are kept in a file of "
                                   "their own: open another one, save them as a new one, or go "
                                   "back to the default file" ) );
    m_settingsFile->Hide();

    auto* buttons = new wxBoxSizer( wxHORIZONTAL );
    buttons->Add( m_settingsFile, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
    buttons->Add( chips, 0, wxALIGN_CENTER_VERTICAL );

    conn->Add( m_kicadStatus, 1, wxALIGN_CENTER_VERTICAL );
    conn->Add( buttons, 0, wxALIGN_CENTER_VERTICAL );
    conn->Add( m_pin, 0, wxALIGN_CENTER_VERTICAL );

    m_openocdStatus = new wxStaticText( m_panel, wxID_ANY, _( "OpenOCD: connecting..." ) );
    auto* server    = new wxButton( m_panel, wxID_ANY, _( "Address..." ), wxDefaultPosition,
                                    wxDefaultSize, wxBU_EXACTFIT );
    server->SetToolTip( _( "Host and Tcl port of the running OpenOCD" ) );
    conn->Add( m_openocdStatus, 1, wxALIGN_CENTER_VERTICAL );
    conn->Add( server, 0, wxALIGN_CENTER_VERTICAL );
    conn->AddSpacer( 0 );

    aSizer->Add( conn, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    Bind( wxEVT_TOGGLEBUTTON, &MainFrame::onPinToggled, this, m_pin->GetId() );
    server->Bind( wxEVT_BUTTON, &MainFrame::onChangeServer, this );
    chips->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { openChipView(); } );
    m_settingsFile->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { onSettingsFile(); } );
}


void MainFrame::openChipView()
{
    if( !m_chipView )
    {
        m_chipView = new ChipView( this, m_session, m_extestCtl );

        // A pin clicked there is the selected pad in the Pins tab
        m_chipView->Bind( EVT_CHIP_PIN_SELECTED,
                          [this]( wxCommandEvent& )
                          {
                              m_pinsPage->ShowBoard();
                              m_pinsPage->SelectWatchPins( m_chipView->SelectedPins() );
                          } );
        m_chipView->Bind( EVT_CHIP_PINS_CHANGED,
                          [this]( wxCommandEvent& ) { m_pinsPage->UpdateTargets(); } );
        m_chipView->Bind( EVT_WATCH_LIST_CHANGED,
                          [this]( wxCommandEvent& )
                          {
                              m_pinsPage->FillWatchList();
                              m_pinsPage->UpdateTargets();
                          } );
    }

    m_chipView->Show();
    m_chipView->Raise();
    m_chipView->UpdateState();
}


void MainFrame::updateChipView()
{
    if( m_chipView && m_chipView->IsShown() )
        m_chipView->UpdateState();
}


// What happens in one part and concerns the others
void MainFrame::connectParts()
{
    // Rows selected in the watch list: highlighted in the chip view
    Bind( EVT_WATCH_SELECTION,
          [this]( wxCommandEvent& )
          {
              if( m_chipView )
                  m_chipView->HighlightPins( m_pinsPage->SelectedWatchKeys() );
          } );

    // From the EXTEST bar and the pages (they travel up to the frame)
    Bind( EVT_LAYOUT_CHANGED,
          [this]( wxCommandEvent& )
          {
              m_panel->Layout();
              ensureFits();
          } );

    m_session.OnNotSaved = [this]( const wxString& aText ) { m_chainPage->ShowNote( aText ); };

    m_extestCtl.OnChanged = [this]
    {
        m_extestBanner->UpdateState();
        m_chainPage->Fill();
        m_pinsPage->UpdateTargets();
        updateInterconnect();
        updateChipView();
    };
    m_extestCtl.OnStartStatesChanged = [this] { m_icPage->UpdatePlan(); };
    m_extestCtl.OnStatus = [this]( const wxString& aText ) { m_pinsPage->SetStatus( aText ); };

    m_chainPage->Bind( EVT_ASSIGNMENTS_CHANGED,
                       [this]( wxCommandEvent& aEvent )
                       {
                           if( aEvent.GetInt() )
                               m_pinsPage->ResampleAll();

                           m_pinsPage->UpdateTargets();
                           m_extestBanner->UpdateState();
                           updateInterconnect();
                           updateChipView();
                       } );

    m_tabs->Bind( wxEVT_NOTEBOOK_PAGE_CHANGED,
                  [this]( wxBookCtrlEvent& aEvent )
                  {
                      aEvent.Skip();
                      wxConfigBase::Get()->Write( CFG_TAB, m_tabs->GetSelection() );
                      updateInterconnect();
                  } );

    Bind( wxEVT_CLOSE_WINDOW, &MainFrame::onClose, this );
}


void MainFrame::startWorkers( const kicad::Client::Config& aKiCadConfig )
{
    // --- OpenOCD ---
    long interval = wxConfigBase::Get()->ReadLong( CFG_INTERVAL, 100 );
    m_sampler.SetInterval( std::chrono::milliseconds( interval ) );
    m_sampler.SetServer( m_session.config.host, m_session.config.port );
    m_sampler.SetExtestCallback( [this]( const LiveSampler::ExtestUpdate& aUpd )
                                 { CallAfter( [this, aUpd] { m_extestCtl.OnUpdate( aUpd ); } ); } );
    m_sampler.Start( [this]( const LiveSampler::ChainUpdate& aUpd )
                     { CallAfter( [this, aUpd] { showChain( aUpd ); } ); },
                     [this]( const LiveSampler::SampleUpdate& aUpd )
                     {
                         CallAfter(
                                 [this, aUpd]
                                 {
                                     m_pinsPage->ShowSample( aUpd );

                                     if( m_chipView )
                                         m_chipView->ShowSample( aUpd );
                                 } );
                     } );

    // --- KiCad ---
    kicad::Client::Config cfg = aKiCadConfig;
    cfg.timeoutMs             = 1000;
    m_watcher                 = std::make_unique<kicad::BoardWatcher>( cfg, SELECTION_POLL );
    m_session.watcher         = m_watcher.get();
    m_watcher->Start(
            [this]( const kicad::BoardState& aState )
            {
                // CallAfter is thread-safe; the copy is shown in the GUI thread.
                // Events still queued when the frame is destroyed are discarded.
                CallAfter( [this, aState] { showBoardState( aState ); } );
            } );
}


MainFrame::~MainFrame()
{
    stopWorkers();
}


void MainFrame::stopWorkers()
{
    m_chainPage->StopBsdlScan();

    if( m_watcher )
        m_watcher->Stop();

    m_sampler.Stop();
}


void MainFrame::BringToFront()
{
    if( IsIconized() )
        Iconize( false );

    Show();
    Raise();
    RequestUserAttention( wxUSER_ATTENTION_INFO );
}


void MainFrame::updateInterconnect()
{
    // Planned when it is shown (and again whenever something changes then)
    if( m_tabs->GetCurrentPage() == m_icPage )
        m_icPage->UpdatePlan();
}


// ============================================================================
// Events of the window
// ============================================================================

void MainFrame::onClose( wxCloseEvent& aEvent )
{
    if( !m_extestCtl.ConfirmClose( aEvent.CanVeto() ) )
    {
        aEvent.Veto();
        return;
    }

    // Stop the workers before the window goes away (may wait for a pending request)
    stopWorkers();
    aEvent.Skip();   // default handling destroys the frame
}


void MainFrame::onPinToggled( wxCommandEvent& aEvent )
{
    applyStayOnTop( aEvent.IsChecked() );
    wxConfigBase::Get()->Write( CFG_STAY_ON_TOP, aEvent.IsChecked() );
}


void MainFrame::applyStayOnTop( bool aOnTop )
{
    long style = GetWindowStyleFlag();
    style      = aOnTop ? ( style | wxSTAY_ON_TOP ) : ( style & ~wxSTAY_ON_TOP );
    SetWindowStyleFlag( style );
}


void MainFrame::onChangeServer( wxCommandEvent& )
{
    const bscan::ProjectConfig& cfg = m_session.config;
    wxString current                = wxString::Format( "%s:%d", fromUtf8( cfg.host ), cfg.port );

    wxTextEntryDialog dlg( this, _( "OpenOCD host and Tcl port (host:port):" ),
                           _( "OpenOCD address" ), current );

    if( dlg.ShowModal() != wxID_OK )
        return;

    wxString value = dlg.GetValue().Trim().Trim( false );
    wxString host  = value.BeforeLast( ':' );
    long     port  = 0;

    if( host.empty() || !value.AfterLast( ':' ).ToLong( &port ) || port <= 0 || port > 65535 )
    {
        wxMessageBox( _( "Expected host:port, e.g. 127.0.0.1:6666" ), _( "OpenOCD address" ),
                      wxOK | wxICON_WARNING, this );
        return;
    }

    m_session.config.host = toUtf8( host );
    m_session.config.port = static_cast<int>( port );
    m_session.Save();
    m_sampler.SetServer( m_session.config.host, m_session.config.port );
    m_openocdStatus->SetLabel( wxString::Format( _( "OpenOCD %s: connecting..." ), value ) );
}


// ============================================================================
// Updates from the workers
// ============================================================================

void MainFrame::showBoardState( const kicad::BoardState& aState )
{
    bool referencesChanged = aState.references != m_session.board.references;
    bool netsChanged       = aState.padNets != m_session.board.padNets;

    // A new selection in KiCad replaces a pin picked in the plugin, and is
    // shown in the watch list and the chip view - unless it is KiCad's
    // answer to a selection the plugin asked for
    bool fromKiCad = false;

    if( aState.status != m_session.board.status || aState.pads != m_session.board.pads
        || aState.footprintRef != m_session.board.footprintRef )
    {
        fromKiCad = !m_session.IsOwnSelection( aState.pads );

        if( fromKiCad )
            m_session.pickedPin.reset();
    }

    m_session.board = aState;

    if( fromKiCad )
    {
        std::vector<Session::PadKey> pads;

        for( const kicad::PadInfo& p : aState.pads )
            pads.push_back( { p.reference, p.number } );

        m_pinsPage->SelectWatchPins( pads );

        if( m_chipView )
            m_chipView->HighlightPins( pads );
    }

    if( netsChanged )
        updateInterconnect();

    wxString conn;

    switch( aState.status )
    {
    case Status::Disconnected:
        conn = _( "KiCad: not connected" );
        break;

    case Status::NoBoard:
        conn = _( "KiCad: connected, no board open" );
        break;

    default:
        conn = wxString::Format( _( "KiCad: %s" ), fromUtf8( aState.boardFile ) );
        break;
    }

    m_kicadText = conn;
    m_kicadStatus->SetToolTip( fromUtf8( aState.detail ) );

    loadProject( aState );

    // No KiCad project (yet): settings of their own, so nothing is lost
    if( m_session.projectFile.empty() && aState.projectPath.empty() )
        useStandaloneSettings();

    showSettingsSource();
    m_chainPage->EnableTakeSelection( !aState.footprintRef.empty() );

    if( referencesChanged )
    {
        m_chainPage->Fill();   // reference check results may change
        updateInterconnect();
    }

    if( referencesChanged || netsChanged )
        updateChipView();   // net names, warning without board data

    m_pinsPage->ShowBoard();
}


void MainFrame::loadProject( const kicad::BoardState& aState )
{
    if( m_session.LoadProject( aState ) )
        applySettings();
}


void MainFrame::applySettings()
{
    m_sampler.SetServer( m_session.config.host, m_session.config.port );
    m_chainPage->Rebuild();
    m_pinsPage->FillWatchList();
    m_extestBanner->UpdateState();
    m_chainPage->StartBsdlScan();
    updateInterconnect();
    updateChipView();

    // A project without any BSDL file needs the setup first
    if( std::ranges::none_of( m_session.config.taps,
                              []( const TapAssignment& t ) { return !t.bsdl.empty(); } ) )
        m_tabs->SetSelection( 0 );
}


void MainFrame::showChain( const LiveSampler::ChainUpdate& aUpdate )
{
    m_session.chain = aUpdate;

    const bscan::ProjectConfig& cfg  = m_session.config;
    wxString                    addr = wxString::Format( "%s:%d", fromUtf8( cfg.host ), cfg.port );

    if( aUpdate.connected )
    {
        m_openocdStatus->SetLabel(
                wxString::Format( _( "OpenOCD %s: %zu TAP(s)" ), addr, aUpdate.taps.size() ) );
        m_openocdStatus->SetToolTip( wxEmptyString );
    }
    else
    {
        m_openocdStatus->SetLabel( wxString::Format( _( "OpenOCD %s: not connected" ), addr ) );
        m_openocdStatus->SetToolTip( fromUtf8( aUpdate.error ) );
        m_pinsPage->SetStatus( _( "Not sampling: no connection to OpenOCD." ) );
    }

    m_session.SyncExtestRecord();
    m_chainPage->Rebuild();
    m_pinsPage->UpdateTargets();
    m_extestBanner->UpdateState();
    updateInterconnect();
    updateChipView();
}


void MainFrame::ensureFits()
{
    // The window may be resized by the user (or restored from the last
    // session), but never below the size its contents need (the chain table
    // scrolls sideways instead, see ChainTable::updateMinSize()).
    wxSize need = GetSizer()->ComputeFittingWindowSize( this );
    need.x      = std::max( need.x, FromDIP( 400 ) );
    SetMinSize( need );

    if( GetSize().x < need.x || GetSize().y < need.y )
        SetSize( wxSize( std::max( GetSize().x, need.x ), std::max( GetSize().y, need.y ) ) );
}


void MainFrame::useStandaloneSettings()
{
    // Once: an unreadable file must not bring up its message at every poll
    if( std::exchange( m_standaloneTried, true ) )
        return;

    wxString    last = wxConfigBase::Get()->Read( CFG_SETTINGS_FILE, wxEmptyString );
    std::string file = !last.empty() && wxFileName::FileExists( last )
                               ? std::string( last.utf8_str() )
                               : Session::DefaultSettingsFile();

    if( m_session.UseSettingsFile( file ) )
        applySettings();
}


void MainFrame::showSettingsSource()
{
    wxString label = m_kicadText;
    bool     own   = !m_session.settingsFromKiCad && !m_session.projectFile.empty();

    if( own )
        label << wxString::FromUTF8( " \u2013 " ) << _( "settings: " )
              << wxFileName( fromUtf8( m_session.projectFile ) ).GetFullName();

    if( m_kicadStatus->GetLabel() != label )
        m_kicadStatus->SetLabel( label );

    if( m_settingsFile->IsShown() != own )
    {
        m_settingsFile->Show( own );
        m_panel->Layout();
        ensureFits();
    }

    if( own )
        m_settingsFile->SetToolTip( fromUtf8( m_session.projectFile ) );
}


void MainFrame::onSettingsFile()
{
    enum
    {
        ID_OPEN = 1,
        ID_SAVE_AS,
        ID_DEFAULT
    };

    const std::string def = Session::DefaultSettingsFile();

    wxMenu menu;
    menu.Append( ID_OPEN, _( "Open settings..." ) );
    menu.Append( ID_SAVE_AS, _( "Save settings as..." ) );
    menu.Append( ID_DEFAULT, _( "Use the default file" ) );
    menu.Enable( ID_DEFAULT, m_session.projectFile != def );

    const wxString wildcard = _( "Boundary-scan settings (*.bscan.json)|*.bscan.json|"
                                 "All files (*.*)|*.*" );
    wxFileName current( fromUtf8( m_session.projectFile ) );

    switch( m_settingsFile->GetPopupMenuSelectionFromUser( menu ) )
    {
    case ID_OPEN:
    {
        wxFileDialog dlg( this, _( "Open settings" ), current.GetPath(), wxEmptyString, wildcard,
                          wxFD_OPEN | wxFD_FILE_MUST_EXIST );

        if( dlg.ShowModal() != wxID_OK )
            return;

        if( m_session.UseSettingsFile( std::string( dlg.GetPath().utf8_str() ) ) )
        {
            wxConfigBase::Get()->Write( CFG_SETTINGS_FILE, dlg.GetPath() );
            applySettings();
        }

        break;
    }

    case ID_SAVE_AS:
    {
        wxFileDialog dlg( this, _( "Save settings as" ), current.GetPath(), "board.bscan.json",
                          wildcard, wxFD_SAVE | wxFD_OVERWRITE_PROMPT );

        if( dlg.ShowModal() != wxID_OK )
            return;

        if( m_session.SaveSettingsAs( std::string( dlg.GetPath().utf8_str() ) ) )
            wxConfigBase::Get()->Write( CFG_SETTINGS_FILE, dlg.GetPath() );

        break;
    }

    case ID_DEFAULT:
        if( m_session.UseSettingsFile( def ) )
        {
            wxConfigBase::Get()->DeleteEntry( CFG_SETTINGS_FILE );
            applySettings();
        }

        break;

    default:
        return;
    }

    showSettingsSource();
}
