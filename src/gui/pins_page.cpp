// SPDX-License-Identifier: GPL-3.0-only
#include "pins_page.h"

#include <algorithm>
#include <utility>
#include <set>

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/config.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/statbox.h>
#include <wx/stattext.h>

#include "extest_controller.h"
#include "extest_dialogs.h"
#include "gui_events.h"
#include "session.h"

using bscan::LiveSampler;
using bscan::PinDrive;
using bscan::PinValue;
using bscan::TapAssignment;
using Status = kicad::BoardState::Status;

static const char* CFG_INTERVAL = "SampleIntervalMs";

static const int SAMPLE_INTERVALS[] = { 50, 100, 200, 500, 1000 };

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


static std::string toUtf8( const wxString& aText )
{
    return std::string( aText.utf8_str() );
}


static wxString dash()
{
    return wxString::FromUTF8( "\u2013" );
}


// Small 0 / 1 / Z button. Exact fit keeps them small; the minimum width keeps
// them easy to hit where "exact" means a frame tight around one character
// (Windows).
static wxButton* newDriveButton( wxWindow* aParent, const wxString& aLabel )
{
    auto* b = new wxButton( aParent, wxID_ANY, aLabel, wxDefaultPosition, wxDefaultSize,
                            wxBU_EXACTFIT );
    b->SetMinSize( wxSize( std::max( b->GetBestSize().x, aParent->FromDIP( 28 ) ), -1 ) );
    return b;
}


// Drive buttons tooltips: drive directly, or start EXTEST via setup and review
static void setDriveTips( const std::vector<wxButton*>& aButtons, const wxString& aStartFor )
{
    const PinDrive drives[] = { PinDrive::Low, PinDrive::High, PinDrive::HighZ };

    for( size_t i = 0; i < aButtons.size() && i < 3; ++i )
    {
        wxString tip = aStartFor.empty()
                               ? wxString::Format( _( "Drive: %s" ), DriveLabel( drives[i] ) )
                               : wxString::Format( _( "Starts EXTEST for %s with this pin at %s: "
                                                      "setup and review first, nothing is "
                                                      "driven before you confirm." ),
                                                   aStartFor, DriveLabel( drives[i] ) );

        if( aButtons[i]->GetToolTipText() != tip )
            aButtons[i]->SetToolTip( tip );
    }
}


static wxString inputText( const PinValue& aValue )
{
    return aValue.input ? wxString( *aValue.input ? "1" : "0" ) : wxString::FromUTF8( "–" );
}


static wxString outputText( const PinValue& aValue )
{
    switch( aValue.output )
    {
    case PinValue::Output::None:
        return wxString::FromUTF8( "–" );
    case PinValue::Output::HighZ:
        return _( "off (high-Z)" );
    case PinValue::Output::Drive0:
        return _( "drives 0" );
    case PinValue::Output::Drive1:
        return _( "drives 1" );
    }

    return "?";
}


PinsPage::PinsPage( wxWindow* aParent, Session& aSession, ExtestController& aExtest ) :
        wxPanel( aParent ),
        m_session( aSession ),
        m_extest( aExtest )
{
    auto* sizer = new wxBoxSizer( wxVERTICAL );
    buildLiveLine( this, sizer );
    buildPadBox( this, sizer );
    buildWatchBox( this, sizer );
    SetSizer( sizer );

    showPadState( m_session.SelectedPad() );
    showLiveMessage( _( "Select a pad in the PCB editor." ) );
    FillWatchList();
    updateDriveButtons();
}


void PinsPage::layoutChanged()
{
    SendGuiEvent( this, EVT_LAYOUT_CHANGED );
}


void PinsPage::ShowBoard()
{
    showPadState( m_session.SelectedPad() );
    m_addSelection->Enable( !m_session.board.pads.empty() || m_session.pickedPin.has_value() );
    UpdateTargets();
}


void PinsPage::buildLiveLine( wxWindow* aParent, wxSizer* aSizer )
{
    wxWindow* box = aParent;

    // Sampling status (selected pad and watch list) and interval
    auto* live   = new wxBoxSizer( wxHORIZONTAL );
    m_liveStatus = new wxStaticText( box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                     wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END );
    m_liveStatus->SetMinSize( wxSize( FromDIP( 100 ), m_liveStatus->GetCharHeight() ) );
    live->Add( m_liveStatus, 1, wxALIGN_CENTER_VERTICAL );

    m_rate = new wxChoice( box, wxID_ANY );

    long interval = wxConfigBase::Get()->ReadLong( CFG_INTERVAL, 100 );

    for( int ms : SAMPLE_INTERVALS )
    {
        m_rate->Append( wxString::Format( _( "every %d ms" ), ms ) );

        if( ms == interval )
            m_rate->SetSelection( m_rate->GetCount() - 1 );
    }

    if( m_rate->GetSelection() == wxNOT_FOUND )
        m_rate->SetSelection( 1 );   // 100 ms

    m_rate->SetToolTip( _( "Sampling interval for the selected pad and the watch list. The "
                           "real rate depends on the JTAG adapter and is not suitable for "
                           "timing measurements." ) );
    live->Add( m_rate, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 8 ) );
    aSizer->Add( live, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_rate->Bind( wxEVT_CHOICE, &PinsPage::onRateChanged, this );
}


wxWindow* PinsPage::buildPadBox( wxWindow* aParent, wxSizer* aSizer )
{
    auto*     sizer = new wxStaticBoxSizer( wxVERTICAL, aParent, _( "Selected pad" ) );
    wxWindow* box   = sizer->GetStaticBox();

    m_padGrid = new wxFlexGridSizer( 2, FromDIP( 4 ), FromDIP( 12 ) );
    m_padGrid->AddGrowableCol( 1 );

    auto addRow = [&]( const wxString& aLabel, bool aLarge ) -> wxStaticText*
    {
        auto*  value = new wxStaticText( box, wxID_ANY, wxEmptyString );
        wxFont font  = value->GetFont().Bold();

        if( aLarge )
            font = font.Larger();

        value->SetFont( font );
        m_padGrid->Add( new wxStaticText( box, wxID_ANY, aLabel ), 0, wxALIGN_CENTER_VERTICAL );
        m_padGrid->Add( value, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL );
        return value;
    };

    m_reference  = addRow( _( "Reference:" ), false );
    m_padNumber  = addRow( _( "Pad / ball:" ), false );
    m_net        = addRow( _( "Net:" ), false );
    m_livePort   = addRow( _( "Port:" ), false );
    m_liveInput  = addRow( _( "Input:" ), true );
    m_liveOutput = addRow( _( "Output:" ), true );

    auto* row = new wxBoxSizer( wxHORIZONTAL );
    row->Add( m_padGrid, 1, wxEXPAND );
    m_bsdlInfo = new wxButton( box, wxID_ANY, _( "BSDL info..." ) );
    row->Add( m_bsdlInfo, 0, wxALIGN_BOTTOM | wxLEFT, FromDIP( 8 ) );
    sizer->Add( row, 0, wxEXPAND | wxALL, FromDIP( 4 ) );

    // EXTEST line: drive buttons while the pad's device is in EXTEST, otherwise
    // a hint where EXTEST is started. Keeps its height so the layout is stable.
    m_driveRow   = new wxBoxSizer( wxHORIZONTAL );
    m_driveLabel = new wxStaticText( box, wxID_ANY, _( "EXTEST drive:" ) );
    m_driveRow->Add( m_driveLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );

    const std::pair<PinDrive, wxString> drives[] = { { PinDrive::Low, _( "0" ) },
                                                     { PinDrive::High, _( "1" ) },
                                                     { PinDrive::HighZ, _( "Z" ) } };

    int buttonHeight = 0;

    for( const auto& [d, label] : drives )
    {
        auto*    b    = newDriveButton( box, label );
        PinDrive what = d;
        b->Bind( wxEVT_BUTTON,
                 [this, what]( wxCommandEvent& )
                 {
                     Session::ShownPad shown = m_session.SelectedPad();

                     if( shown.status == Status::PadSelected )
                         m_extest.DrivePins( { { shown.pad.reference, shown.pad.number, {} } },
                                             what );
                 } );
        b->SetToolTip( wxString::Format( _( "Drive this pin: %s" ), DriveLabel( d ) ) );
        m_driveRow->Add( b, 0, wxRIGHT, FromDIP( 4 ) );
        m_driveButtons.push_back( b );
        buttonHeight = std::max( buttonHeight, b->GetBestSize().y );
    }

    m_driveHint = new wxStaticText( box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                    wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END );
    m_driveHint->SetMinSize( wxSize( FromDIP( 100 ), m_driveHint->GetCharHeight() ) );
    m_driveRow->Add( m_driveHint, 1, wxALIGN_CENTER_VERTICAL );
    m_driveRow->SetMinSize( wxSize( -1, buttonHeight ) );
    sizer->Add( m_driveRow, 0, wxEXPAND | wxALL, FromDIP( 4 ) );

    // Hint line below the values; reserves one text line so the layout is stable.
    // Long hints are shortened with "..."; the full text is in the tooltip.
    m_padHint = new wxStaticText( box, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                                  wxST_NO_AUTORESIZE | wxST_ELLIPSIZE_END );
    m_padHint->SetMinSize( wxSize( FromDIP( 100 ), m_padHint->GetCharHeight() ) );
    sizer->Add( m_padHint, 0, wxEXPAND | wxALL, FromDIP( 4 ) );

    aSizer->Add( sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    m_bsdlInfo->Bind( wxEVT_BUTTON, &PinsPage::onBsdlInfo, this );
    return box;
}


wxWindow* PinsPage::buildWatchBox( wxWindow* aParent, wxSizer* aSizer )
{
    auto*     sizer = new wxStaticBoxSizer( wxVERTICAL, aParent, _( "Watch list" ) );
    wxWindow* box   = sizer->GetStaticBox();

    m_watchList = new wxListView( box, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxLC_REPORT | wxLC_HRULES );
    m_watchList->AppendColumn( _( "Reference" ) );
    m_watchList->AppendColumn( _( "Pad" ) );
    m_watchList->AppendColumn( _( "Net" ) );
    m_watchList->AppendColumn( _( "Port" ) );
    m_watchList->AppendColumn( _( "In" ), wxLIST_FORMAT_CENTER );
    m_watchList->AppendColumn( _( "Out" ) );
    m_watchList->SetMinSize( wxSize( FromDIP( 480 ), FromDIP( 120 ) ) );

    // The last column ("Out") takes the remaining width, so a click right of
    // the text still hits the row
    m_watchList->Bind( wxEVT_SIZE,
                       [this]( wxSizeEvent& aEvent )
                       {
                           aEvent.Skip();
                           CallAfter( [this] { fitLastWatchColumn(); } );
                       } );
    m_watchList->Bind( wxEVT_LIST_COL_END_DRAG,
                       [this]( wxListEvent& aEvent )
                       {
                           aEvent.Skip();
                           CallAfter( [this] { fitLastWatchColumn(); } );
                       } );
    m_watchList->SetToolTip( _( "Pins sampled continuously, independent of the selection in "
                                "KiCad. Add pads with \"+ Selection\" (Ctrl+click selects "
                                "several pads in KiCad).\n\"Drive selected\" (or right-click / "
                                "double-click) drives the selected pins; if EXTEST is off, it "
                                "opens the setup and review first. Pins being driven are marked "
                                "with \u25b6." ) );
    sizer->Add( m_watchList, 1, wxEXPAND | wxALL, FromDIP( 4 ) );

    auto* buttons  = new wxBoxSizer( wxHORIZONTAL );
    m_addSelection = new wxButton( box, wxID_ANY, _( "+ Selection" ) );
    m_addSelection->SetToolTip( _( "Add all pads selected in the PCB editor, or the pin "
                                   "clicked in the chip view" ) );
    auto* remove = new wxButton( box, wxID_ANY, _( "Remove" ) );
    auto* clear  = new wxButton( box, wxID_ANY, _( "Remove all" ) );
    buttons->Add( m_addSelection, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->Add( remove, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->AddStretchSpacer();

    // Drive buttons for the selected rows, shown while a device is in EXTEST
    m_watchDriveLabel = new wxStaticText( box, wxID_ANY, _( "Drive selected:" ) );
    buttons->Add( m_watchDriveLabel, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 6 ) );

    const std::pair<PinDrive, wxString> drives[] = { { PinDrive::Low, _( "0" ) },
                                                     { PinDrive::High, _( "1" ) },
                                                     { PinDrive::HighZ, _( "Z" ) } };

    for( const auto& [d, label] : drives )
    {
        auto*    b    = newDriveButton( box, label );
        PinDrive what = d;
        b->Bind( wxEVT_BUTTON, [this, what]( wxCommandEvent& )
                 { m_extest.DrivePins( selectedWatchPins(), what ); } );
        b->SetToolTip( wxString::Format( _( "Drive the selected pins: %s" ), DriveLabel( d ) ) );
        buttons->Add( b, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
        m_watchDriveButtons.push_back( b );
    }

    buttons->AddSpacer( FromDIP( 12 ) );
    buttons->Add( clear, 0 );
    sizer->Add( buttons, 0, wxEXPAND | wxALL, FromDIP( 4 ) );
    m_watchButtons = buttons;

    aSizer->Add( sizer, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP( 8 ) );

    m_addSelection->Bind( wxEVT_BUTTON, &PinsPage::onAddSelection, this );
    m_watchList->Bind( wxEVT_LIST_ITEM_RIGHT_CLICK, &PinsPage::onWatchMenu, this );
    m_watchList->Bind( wxEVT_LIST_ITEM_ACTIVATED, &PinsPage::onWatchMenu, this );
    // Selecting rows: drive buttons; the user's selection also goes to KiCad
    // and the chip view (once per change, not per row)
    auto selection = [this]( wxListEvent& )
    {
        updateWatchDrive();

        if( m_watchSyncing || std::exchange( m_watchPending, true ) )
            return;

        CallAfter(
                [this]
                {
                    m_watchPending = false;
                    onWatchSelection();
                } );
    };

    m_watchList->Bind( wxEVT_LIST_ITEM_SELECTED, selection );
    m_watchList->Bind( wxEVT_LIST_ITEM_DESELECTED, selection );
    remove->Bind( wxEVT_BUTTON, &PinsPage::onRemoveWatch, this );
    clear->Bind( wxEVT_BUTTON, &PinsPage::onClearWatch, this );
    return box;
}


void PinsPage::onRateChanged( wxCommandEvent& )
{
    int sel = m_rate->GetSelection();

    if( sel < 0 || sel >= static_cast<int>( std::size( SAMPLE_INTERVALS ) ) )
        return;

    int ms = SAMPLE_INTERVALS[sel];
    m_session.sampler.SetInterval( std::chrono::milliseconds( ms ) );
    wxConfigBase::Get()->Write( CFG_INTERVAL, static_cast<long>( ms ) );
}


void PinsPage::showPadState( const Session::ShownPad& aState )
{
    wxString hint;

    switch( aState.status )
    {
    case Status::Disconnected:
    case Status::NoBoard:
        hint = _( "No board available." );
        break;

    case Status::NothingSelected:
        hint = _( "Click a pad in the PCB editor." );
        break;

    case Status::FootprintSelected:
        hint = wxString::Format( _( "Footprint %s selected; click one of its pads." ),
                                 fromUtf8( aState.footprintRef ) );
        break;

    case Status::MultiplePads:
        hint = wxString::Format( _( "%d pads selected, select a single pad." ), aState.padCount );
        break;

    case Status::PadSelected:
        hint = aState.fromChipView ? _( "Pin clicked in the chip view." )
                                   : fromUtf8( aState.detail );   // e.g. footprint lookup problems
        break;
    }

    // The values stay visible so the window does not change height while the
    // user clicks around; without a pad they show a dash.
    const bool            havePad = aState.status == Status::PadSelected;
    const kicad::PadInfo& pad     = aState.pad;

    m_reference->SetLabel( !havePad                ? dash()
                           : pad.reference.empty() ? wxString( "?" )
                                                   : fromUtf8( pad.reference ) );
    m_padNumber->SetLabel( havePad ? fromUtf8( pad.number ) : dash() );
    // A pin from the chip view has no net when there is no board
    bool netKnown = !aState.fromChipView || m_session.board.padNets.contains( pad.reference );

    m_net->SetLabel( !havePad          ? dash()
                     : !netKnown       ? _( "(no board)" )
                     : pad.net.empty() ? _( "(unconnected)" )
                                       : fromUtf8( pad.net ) );

    m_boardHint = hint;
    updatePadHint();
    layoutChanged();
}


std::string PinsPage::watchKey( const bscan::WatchEntry& aEntry )
{
    return "w:" + aEntry.reference + "|" + aEntry.pad;
}


void PinsPage::UpdateTargets()
{
    std::vector<LiveSampler::Target> targets;
    std::string                      identity;

    // --- the selected pad ---
    wxString          curMessage;
    wxString          curPort   = dash();
    bool              curTarget = false;
    Session::ShownPad shown     = m_session.SelectedPad();

    if( shown.status != Status::PadSelected )
    {
        curMessage = shown.status == Status::MultiplePads
                             ? _( "Several pads selected; \"+ Selection\" adds them to the "
                                  "watch list." )
                             : _( "Select a pad in the PCB editor." );
    }
    else
    {
        LiveSampler::Target t;
        std::string         id;
        curMessage = m_session.ResolvePin( shown.pad.reference, shown.pad.number, t, id );

        if( !t.cells.port.empty() )
        {
            curPort = fromUtf8( t.cells.port );
            m_livePort->SetLabel( curPort );
        }

        if( curMessage.empty() )
        {
            t.key = "cur";
            targets.push_back( t );
            identity += "cur=" + id + ";";
            curTarget = true;
        }
    }

    // --- the watch list ---
    m_watchNotes.clear();

    for( const bscan::WatchEntry& w : m_session.config.watch )
    {
        LiveSampler::Target t;
        std::string         id;
        wxString            msg = m_session.ResolvePin( w.reference, w.pad, t, id );
        std::string         key = watchKey( w );

        m_watchPorts[key] = t.cells.port;

        if( !msg.empty() )
        {
            m_watchNotes[key] = msg;
            continue;
        }

        t.key = key;
        targets.push_back( t );
        identity += key + "=" + id + ";";
    }

    // --- the pins the chip view shows ---
    for( const std::string& pin : m_session.chipViewPins )
    {
        LiveSampler::Target t;
        std::string         id;

        if( !m_session.ResolvePin( m_session.chipViewReference, pin, t, id ).empty() )
            continue;   // the chip view shows them as not sampled

        t.key = "chip:" + m_session.chipViewReference + "|" + pin;
        identity += t.key + "=" + id + ";";
        targets.push_back( t );
    }

    if( !m_targetsIdentity || identity != *m_targetsIdentity )
    {
        m_targetsIdentity = identity;
        m_targetsCount    = targets.size();
        m_targetsVersion  = m_session.sampler.SetTargets( std::move( targets ) );
        m_watchValues.clear();

        if( curTarget )
        {
            m_liveInput->SetLabel( "..." );
            m_liveOutput->SetLabel( "..." );
            m_padLiveNote.clear();
            updatePadHint();
        }

        SetStatus( m_targetsCount == 0 ? _( "Nothing to sample: select a pad or add pins to the "
                                            "watch list." )
                                       : _( "Sampling..." ) );
    }

    if( !curTarget )
    {
        showLiveMessage( curMessage );
        m_livePort->SetLabel( curPort );   // e.g. the port of a pad without cells
    }

    updateWatchValues();
    updateDriveButtons();
}


void PinsPage::showLiveMessage( const wxString& aMessage )
{
    m_livePort->SetLabel( dash() );
    m_liveInput->SetLabel( dash() );
    m_liveOutput->SetLabel( dash() );
    m_padLiveNote = aMessage;
    updatePadHint();
}


void PinsPage::updatePadHint()
{
    // Selection problems first, then problems with the live value of the pad
    wxString hint = !m_boardHint.empty() ? m_boardHint : m_padLiveNote;

    if( m_padHint->GetLabel() != hint )
    {
        m_padHint->SetLabel( hint );
        m_padHint->SetToolTip( hint );
    }
}


void PinsPage::SetStatus( const wxString& aText )
{
    if( m_liveStatus->GetLabel() != aText )
    {
        m_liveStatus->SetLabel( aText );
        m_liveStatus->SetToolTip( aText );
    }
}


void PinsPage::ShowSample( const LiveSampler::SampleUpdate& aUpdate )
{
    // Results for an older set of targets are of no interest
    if( aUpdate.targetsVersion != m_targetsVersion )
        return;

    std::set<std::string> taps;
    size_t                ok = 0;
    std::string           firstError;

    // The selected pad is sampled separately; do not count it twice when it is
    // also in the watch list
    std::string       curKey;
    Session::ShownPad shown = m_session.SelectedPad();

    if( shown.status == Status::PadSelected )
        curKey = watchKey( { shown.pad.reference, shown.pad.number, {} } );

    size_t total = 0;

    for( const LiveSampler::PinResult& r : aUpdate.pins )
    {
        if( r.key.starts_with( "chip:" ) )
            continue;   // shown by the chip view

        bool duplicate =
                r.key == "cur" && !curKey.empty()
                && std::any_of( m_session.config.watch.begin(), m_session.config.watch.end(),
                                [&]( const bscan::WatchEntry& w )
                                { return watchKey( w ) == curKey; } );

        if( !duplicate )
        {
            ++total;

            if( r.ok )
            {
                ++ok;
                taps.insert( r.tap );
            }
            else if( firstError.empty() )
            {
                firstError = r.error;
            }
        }

        if( r.key == "cur" )
        {
            if( !r.ok )
            {
                m_liveInput->SetLabel( "?" );
                m_liveOutput->SetLabel( "?" );
                m_padLiveNote = fromUtf8( r.error );
            }
            else
            {
                m_liveInput->SetLabel( inputText( r.value ) );
                m_liveOutput->SetLabel( outputText( r.value ) );
                m_padLiveNote.clear();
            }

            updatePadHint();
        }
        else
        {
            m_watchValues[r.key] = r;
        }
    }

    // One summary line for the selected pad and the watch list
    wxString tapList;

    for( const std::string& t : taps )
    {
        tapList << ( tapList.empty() ? "" : ", " ) << fromUtf8( t );

        if( m_session.extest.contains( t ) )
            tapList << _( " (EXTEST)" );
    }

    wxString status;

    if( ok > 0 )
        status = wxString::Format( wxPLURAL( "Live: %zu pin via %s", "Live: %zu pins via %s", ok ),
                                   ok, tapList );

    if( ok < total )
    {
        status << ( status.empty() ? "" : "; " )
               << wxString::Format( _( "%zu failed: %s" ), total - ok, fromUtf8( firstError ) );
    }

    SetStatus( status );
    updateWatchValues();
}


void PinsPage::FillWatchList()
{
    m_watchList->Freeze();
    m_watchList->DeleteAllItems();

    long i = 0;

    for( const bscan::WatchEntry& w : m_session.config.watch )
    {
        m_watchList->InsertItem( i, fromUtf8( w.reference ) );
        m_watchList->SetItem( i, 1, fromUtf8( w.pad ) );
        m_watchList->SetItem( i, 2, fromUtf8( w.net ) );
        ++i;
    }

    updateWatchValues();
    fitWatchColumns();
    m_watchList->Thaw();
}


void PinsPage::fitWatchColumns()
{
    // All but the last column fit their longest entry (or the header); net
    // names can be long, so that column stops at a maximum (it can still be
    // widened by dragging). The last column takes the rest.
    const int netMax = FromDIP( 160 );
    int       last   = m_watchList->GetColumnCount() - 1;

    for( int c = 0; c < last; ++c )
    {
        m_watchList->SetColumnWidth( c, wxLIST_AUTOSIZE_USEHEADER );

        if( c == 2 && m_watchList->GetColumnWidth( c ) > netMax )
            m_watchList->SetColumnWidth( c, netMax );
    }

    fitLastWatchColumn();
}


void PinsPage::fitLastWatchColumn()
{
    int last = m_watchList->GetColumnCount() - 1;

    if( last < 0 )
        return;

    int others = 0;

    for( int c = 0; c < last; ++c )
        others += m_watchList->GetColumnWidth( c );

    // A few pixels less than the client width: no horizontal scroll bar
    int width = std::max( FromDIP( 120 ), m_watchList->GetClientSize().x - others - FromDIP( 2 ) );

    if( m_watchList->GetColumnWidth( last ) != width )
        m_watchList->SetColumnWidth( last, width );
}


void PinsPage::updateWatchValues()
{
    if( m_watchList->GetItemCount() != static_cast<int>( m_session.config.watch.size() ) )
        return;   // list is being rebuilt

    long i     = 0;
    bool refit = false;

    for( const bscan::WatchEntry& w : m_session.config.watch )
    {
        std::string key  = watchKey( w );
        wxString    port = fromUtf8( m_watchPorts[key] );
        wxString    in   = dash();
        wxString    out;

        if( auto n = m_watchNotes.find( key ); n != m_watchNotes.end() )
        {
            out = n->second;   // why this pin cannot be sampled
        }
        else if( auto v = m_watchValues.find( key ); v != m_watchValues.end() )
        {
            if( v->second.ok )
            {
                in  = inputText( v->second.value );
                out = outputText( v->second.value );

                if( m_session.ActiveExtestFor( w.reference ) )
                    out = wxString::FromUTF8( "\u25b6 " ) + out;   // driven by EXTEST
            }
            else
            {
                in  = "?";
                out = fromUtf8( v->second.error );
            }
        }
        else
        {
            in  = "...";
            out = "...";
        }

        // Only touch cells that changed (avoids flicker at 10 Hz)
        auto set = [&]( int aCol, const wxString& aText )
        {
            if( m_watchList->GetItemText( i, aCol ) == aText )
                return;

            // A longer or shorter port or input text: fit the columns again
            if( aCol < 5 && m_watchList->GetItemText( i, aCol ).length() != aText.length() )
                refit = true;

            m_watchList->SetItem( i, aCol, aText );
        };

        set( 3, port );
        set( 4, in );
        set( 5, out );
        ++i;
    }

    if( refit )
        fitWatchColumns();
}


void PinsPage::onAddSelection( wxCommandEvent& )
{
    int added = 0, unknown = 0;

    // The pin clicked in the chip view, otherwise the pads selected in KiCad
    std::vector<kicad::PadInfo> pads = m_session.pickedPin
                                               ? std::vector<kicad::PadInfo>{ *m_session.pickedPin }
                                               : m_session.board.pads;

    for( const kicad::PadInfo& pad : pads )
    {
        if( pad.reference.empty() )
        {
            ++unknown;
            continue;
        }

        bscan::WatchEntry e{ pad.reference, pad.number, pad.net };

        bool exists = std::any_of( m_session.config.watch.begin(), m_session.config.watch.end(),
                                   [&]( const bscan::WatchEntry& w )
                                   { return w.reference == e.reference && w.pad == e.pad; } );

        if( !exists )
        {
            m_session.config.watch.push_back( e );
            ++added;
        }
    }

    if( pads.empty() )
    {
        wxMessageBox( _( "Select one or more pads in the PCB editor first (Ctrl+click selects "
                         "several), or click a pin in the chip view." ),
                      _( "Watch list" ), wxOK | wxICON_INFORMATION, this );
        return;
    }

    if( unknown )
        wxBell();

    if( added )
    {
        m_session.Save();
        FillWatchList();
        UpdateTargets();
    }
}


void PinsPage::onRemoveWatch( wxCommandEvent& )
{
    std::vector<long> rows;

    for( long i = m_watchList->GetFirstSelected(); i != -1; i = m_watchList->GetNextSelected( i ) )
        rows.push_back( i );

    if( rows.empty() )
        return;

    // Erase from the back so the indices stay valid
    for( auto it = rows.rbegin(); it != rows.rend(); ++it )
        if( *it < static_cast<long>( m_session.config.watch.size() ) )
            m_session.config.watch.erase( m_session.config.watch.begin() + *it );

    m_session.Save();
    FillWatchList();
    UpdateTargets();
}


void PinsPage::onClearWatch( wxCommandEvent& )
{
    if( m_session.config.watch.empty() )
        return;

    m_session.config.watch.clear();
    m_session.Save();
    FillWatchList();
    UpdateTargets();
}


void PinsPage::onBsdlInfo( wxCommandEvent& )
{
    Session::ShownPad    shown = m_session.SelectedPad();
    wxString             ref   = shown.status == Status::PadSelected
                                         ? fromUtf8( shown.pad.reference )
                                         : fromUtf8( shown.footprintRef );
    const TapAssignment* a   = m_session.config.FindByReference( toUtf8( ref ) );

    if( ref.empty() || !a || a->bsdl.empty() )
    {
        wxMessageBox( ref.empty() ? _( "Select a pad or footprint in the PCB editor." )
                                  : wxString::Format( _( "No BSDL file is assigned to %s." ), ref ),
                      _( "BSDL info" ), wxOK | wxICON_INFORMATION, this );
        return;
    }

    std::string err;
    auto        dev = m_session.DeviceFor( a->bsdl, &err );

    if( !dev )
    {
        wxMessageBox( fromUtf8( err ), _( "BSDL info" ), wxOK | wxICON_ERROR, this );
        return;
    }

    std::string mapName = a->pinMap.empty() ? dev->DefaultPinMap() : a->pinMap;
    wxString    text;

    text << _( "Device: " ) << fromUtf8( dev->EntityName() ) << "\n"
         << _( "File: " ) << fromUtf8( a->bsdl ) << "\n"
         << _( "TAP: " ) << fromUtf8( a->tap ) << "\n"
         << _( "IDCODE: " ) << fromUtf8( dev->IdcodePattern().empty() ? "-" : dev->IdcodePattern() )
         << "\n"
         << wxString::Format( _( "IR length: %d, boundary length: %d" ), dev->IrLength(),
                              dev->BoundaryLength() )
         << "\n"
         << _( "Pin map: " ) << fromUtf8( mapName ) << "\n";

    size_t warnings = 0, errors = 0;

    for( const auto& d : dev->Diagnostics() )
        ( d.error ? errors : warnings )++;

    if( errors || warnings )
        text << wxString::Format( _( "Parser: %zu error(s), %zu warning(s)" ), errors, warnings )
             << "\n";

    if( shown.status == Status::PadSelected )
    {
        const std::string& pin   = shown.pad.number;
        auto               cells = dev->LookupPin( a->pinMap, pin );

        text << "\n" << wxString::Format( _( "Pad %s: " ), fromUtf8( pin ) );

        if( !cells )
        {
            text << _( "not in pin map" );
        }
        else
        {
            auto cell = []( int c ) { return c >= 0 ? wxString::Format( "%d", c ) : dash(); };

            text << _( "port " ) << fromUtf8( cells->port ) << "\n"
                 << _( "  input cell: " ) << cell( cells->inputCell ) << "\n"
                 << _( "  output cell: " ) << cell( cells->outputCell ) << "\n"
                 << _( "  control cell: " ) << cell( cells->controlCell );

            if( cells->controlCell >= 0 && cells->disableValue >= 0 )
                text << wxString::Format( _( " (output off at %d)" ), cells->disableValue );
        }
    }

    wxMessageBox( text, _( "BSDL info" ), wxOK | wxICON_INFORMATION, this );
}


void PinsPage::onWatchSelection()
{
    std::vector<bscan::WatchEntry> pins = selectedWatchPins();

    if( pins.empty() )
        return;

    // The row the user is on is the selected pad; all selected rows go to KiCad
    long                     focused = m_watchList->GetFocusedItem();
    const bscan::WatchEntry* picked  = &pins.back();

    if( focused >= 0 && focused < static_cast<long>( m_session.config.watch.size() )
        && m_watchList->IsSelected( focused ) )
        picked = &m_session.config.watch[focused];

    kicad::PadInfo pad;
    pad.reference = picked->reference;
    pad.number    = picked->pad;
    pad.net       = picked->net;

    std::vector<Session::PadKey> keys;

    for( const bscan::WatchEntry& w : pins )
        keys.push_back( { w.reference, w.pad } );

    m_session.PickPins( keys, pad );
    ShowBoard();
    SendGuiEvent( this, EVT_WATCH_SELECTION );
}


void PinsPage::SelectWatchPins( const std::vector<Session::PadKey>& aPins )
{
    std::vector<long> rows;

    for( long i = 0; i < static_cast<long>( m_session.config.watch.size() ); ++i )
    {
        const bscan::WatchEntry& w = m_session.config.watch[i];

        if( std::ranges::find( aPins, Session::PadKey{ w.reference, w.pad } ) != aPins.end() )
            rows.push_back( i );
    }

    if( rows.empty() )
        return;

    m_watchSyncing = true;

    for( long i = 0; i < m_watchList->GetItemCount(); ++i )
        m_watchList->Select( i, std::ranges::find( rows, i ) != rows.end() );

    m_watchList->Focus( rows.front() );
    m_watchSyncing = false;
    updateWatchDrive();
}


std::vector<Session::PadKey> PinsPage::SelectedWatchKeys() const
{
    std::vector<Session::PadKey> keys;

    for( const bscan::WatchEntry& w : selectedWatchPins() )
        keys.push_back( { w.reference, w.pad } );

    return keys;
}


std::vector<bscan::WatchEntry> PinsPage::selectedWatchPins() const
{
    std::vector<bscan::WatchEntry> pins;

    for( long i = m_watchList->GetFirstSelected(); i != -1; i = m_watchList->GetNextSelected( i ) )
        if( i < static_cast<long>( m_session.config.watch.size() ) )
            pins.push_back( m_session.config.watch[i] );

    return pins;
}


void PinsPage::updateWatchDrive()
{
    auto drivable = [this]( const bscan::WatchEntry& w, bool& aActive ) -> bool
    {
        if( Session::ActiveExtest* e = m_session.ActiveExtestFor( w.reference ) )
        {
            auto cells = e->device->LookupPin( e->pinMap, w.pad );
            aActive    = true;
            return cells && cells->outputCell >= 0;
        }

        aActive = false;
        return !m_session.ExtestStartable( w.reference, w.pad ).empty();
    };

    // Shown if any pin on the list can be driven (now or by starting EXTEST)
    bool show = false;

    for( const bscan::WatchEntry& w : m_session.config.watch )
    {
        bool active = false;

        if( drivable( w, active ) )
        {
            show = true;
            break;
        }
    }

    bool any = false, anyActive = false;

    for( const bscan::WatchEntry& w : selectedWatchPins() )
    {
        bool active = false;

        if( drivable( w, active ) )
        {
            any = true;
            anyActive |= active;
        }
    }

    bool changed = m_watchDriveLabel->IsShown() != show;
    m_watchDriveLabel->Show( show );

    for( wxButton* b : m_watchDriveButtons )
    {
        b->Show( show );
        b->Enable( any );
    }

    setDriveTips( m_watchDriveButtons,
                  any && !anyActive ? _( "the device of these pins" ) : wxString() );

    if( changed )
        m_watchButtons->Layout();
}


void PinsPage::updateDriveButtons()
{
    bool     drive = false;
    wxString hint;
    wxString startFor;

    Session::ShownPad shown = m_session.SelectedPad();

    if( shown.status == Status::PadSelected )
    {
        const std::string& ref = shown.pad.reference;

        if( Session::ActiveExtest* e = m_session.ActiveExtestFor( ref ) )
        {
            auto cells = e->device->LookupPin( e->pinMap, shown.pad.number );
            drive      = cells && cells->outputCell >= 0;

            if( !drive )
                hint = _( "EXTEST is active, but this pin has no output cell." );
        }
        else if( std::string tap = m_session.ExtestStartable( ref, shown.pad.number );
                 !tap.empty() )
        {
            const TapAssignment* a = m_session.config.Find( tap );
            drive                  = true;
            startFor               = a ? m_session.DeviceLabel( *a ) : fromUtf8( tap );
            hint = wxString::Format( _( "EXTEST is off for %s: the buttons open the setup "
                                        "and review." ),
                                     startFor );
        }
    }

    bool changed = m_driveLabel->IsShown() != drive;
    m_driveLabel->Show( drive );

    for( wxButton* b : m_driveButtons )
        b->Show( drive );

    setDriveTips( m_driveButtons, startFor );

    if( m_driveHint->GetLabel() != hint )
    {
        m_driveHint->SetLabel( hint );
        m_driveHint->SetToolTip( hint );
    }

    if( changed )
        m_driveRow->Layout();

    updateWatchDrive();
}


void PinsPage::onWatchMenu( wxListEvent& aEvent )
{
    long idx = aEvent.GetIndex();

    if( idx < 0 || idx >= static_cast<long>( m_session.config.watch.size() ) )
        return;

    // The clicked row, or all selected rows if it is one of them
    std::vector<bscan::WatchEntry> pins;

    if( m_watchList->IsSelected( idx ) )
        pins = selectedWatchPins();
    else
        pins.push_back( m_session.config.watch[idx] );

    bool active = std::any_of( pins.begin(), pins.end(), [this]( const bscan::WatchEntry& w )
                               { return m_session.ActiveExtestFor( w.reference ) != nullptr; } );

    wxMenu menu;

    if( !active )
    {
        bool startable =
                std::any_of( pins.begin(), pins.end(), [this]( const bscan::WatchEntry& w )
                             { return !m_session.ExtestStartable( w.reference, w.pad ).empty(); } );

        if( !startable )
        {
            menu.Append( wxID_ANY, _( "Cannot be driven via EXTEST" ) )->Enable( false );
            PopupMenu( &menu );
            return;
        }

        menu.Append( wxID_ANY, _( "EXTEST is off: opens setup and review" ) )->Enable( false );
        menu.AppendSeparator();
    }

    wxString what = pins.size() == 1 ? fromUtf8( pins[0].pad )
                                     : wxString::Format( _( "%zu pins" ), pins.size() );

    const int idLow = wxWindow::NewControlId(), idHigh = wxWindow::NewControlId(),
              idZ = wxWindow::NewControlId();

    menu.Append( idLow, wxString::Format( _( "Drive %s to 0" ), what ) );
    menu.Append( idHigh, wxString::Format( _( "Drive %s to 1" ), what ) );
    menu.Append( idZ, wxString::Format( _( "Switch %s off (Z)" ), what ) );

    int sel = GetPopupMenuSelectionFromUser( menu );

    if( sel == idLow )
        m_extest.DrivePins( pins, PinDrive::Low );
    else if( sel == idHigh )
        m_extest.DrivePins( pins, PinDrive::High );
    else if( sel == idZ )
        m_extest.DrivePins( pins, PinDrive::HighZ );
}
