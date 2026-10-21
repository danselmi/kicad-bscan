// SPDX-License-Identifier: GPL-3.0-only
#include "extest_dialogs.h"

#include <algorithm>
#include <set>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/dataview.h>
#include <wx/dc.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/srchctrl.h>
#include <wx/stattext.h>

using bscan::PinDrive;
using bscan::PinState;

// Appended to the planned state of a pin whose state changes
static const wxString CHANGES_MARK = wxString::FromUTF8( "  ← changes" );

static const PinDrive DRIVES[] = { PinDrive::Safe, PinDrive::HighZ, PinDrive::Low,
                                   PinDrive::High, PinDrive::Keep };

enum SetupColumn
{
    SC_PIN = 0,
    SC_PORT,
    SC_NET,
    SC_NOW,
    SC_START,
    SC_PLANNED,
    SC_TEST      // only with a TestRolesFunction
};


static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


// A cell with a choice that shows it is one: its text plus a drop-down
// arrow. A click (or Enter) opens a menu with the choices. Uses the
// framework's means for custom cells (drawing, activation, ChangeValue), so
// it works the same with the native GTK control and the generic one;
// wxDataViewChoiceRenderer shows plain text until it is edited.
class DropDownChoiceRenderer : public wxDataViewCustomRenderer
{
public:
    explicit DropDownChoiceRenderer( const wxArrayString& aChoices ) :
            wxDataViewCustomRenderer( "string", wxDATAVIEW_CELL_ACTIVATABLE ),
            m_choices( aChoices )
    {
    }

    bool SetValue( const wxVariant& aValue ) override
    {
        m_value = aValue.GetString();
        return true;
    }

    bool GetValue( wxVariant& aValue ) const override
    {
        aValue = m_value;
        return true;
    }

    wxSize GetSize() const override
    {
        wxSize size;

        for( const wxString& c : m_choices )
            size.IncTo( GetTextExtent( c ) );

        size.x += arrowWidth();
        return size;
    }

    bool Render( wxRect aRect, wxDC* aDC, int aState ) override
    {
        int    arrow = arrowWidth();
        wxRect text  = aRect;
        text.width   = std::max( 0, aRect.width - arrow );
        RenderText( m_value, 0, text, aDC, aState );

        // Triangle pointing down in the last 'arrow' pixels, in the colour of
        // the text (wxRendererNative::DrawDropArrow() ignores the position of
        // its rectangle on some platforms)
        wxWindow* win  = GetView();
        int       half = std::max( 3, arrow / 4 );
        int       cx   = aRect.GetRight() - arrow / 2;
        int       top  = aRect.y + ( aRect.height - half ) / 2;
        wxColour  colour;

        if( aState & wxDATAVIEW_CELL_SELECTED )
            colour = wxSystemSettings::GetColour( wxSYS_COLOUR_HIGHLIGHTTEXT );
        else if( win )
            colour = win->GetForegroundColour();
        else
            colour = wxSystemSettings::GetColour( wxSYS_COLOUR_LISTBOXTEXT );

        const wxPoint points[] = { { cx - half, top }, { cx + half, top }, { cx, top + half } };

        wxDCPenChanger   pen( *aDC, wxPen( colour ) );
        wxDCBrushChanger brush( *aDC, wxBrush( colour ) );
        aDC->DrawPolygon( 3, points );
        return true;
    }

    bool ActivateCell( const wxRect&, wxDataViewModel* aModel, const wxDataViewItem& aItem,
                       unsigned int aColumn, const wxMouseEvent* ) override
    {
        wxDataViewCtrl* view = GetView();

        if( !view || !aModel )
            return false;

        // The clicked row becomes the selected one first (on GTK the cell is
        // activated before the click selects its row), so the menu belongs
        // visibly to it
        if( !view->IsSelected( aItem ) )
        {
            view->UnselectAll();
            view->Select( aItem );
        }

        view->SetCurrentItem( aItem );

        wxVariant current;
        aModel->GetValue( current, aItem, aColumn );

        wxMenu menu;

        for( size_t i = 0; i < m_choices.size(); ++i )
        {
            menu.AppendRadioItem( static_cast<int>( i ) + 1, m_choices[i] );

            if( m_choices[i] == current.GetString() )
                menu.Check( static_cast<int>( i ) + 1, true );
        }

        // At the mouse pointer
        int id = view->GetPopupMenuSelectionFromUser( menu );

        if( id >= 1 && static_cast<size_t>( id ) <= m_choices.size()
            && m_choices[id - 1] != current.GetString() )
        {
            aModel->ChangeValue( wxVariant( m_choices[id - 1] ), aItem, aColumn );
        }

        return true;
    }

private:
    int arrowWidth() const
    {
        wxWindow* win = GetView();
        return win ? win->FromDIP( 16 ) : 16;
    }

    wxArrayString m_choices;
    wxString      m_value;
};


wxString DriveLabel( PinDrive aDrive )
{
    switch( aDrive )
    {
    case PinDrive::Safe:  return _( "safe (BSDL)" );
    case PinDrive::HighZ: return _( "Z (off)" );
    case PinDrive::Low:   return _( "0" );
    case PinDrive::High:  return _( "1" );
    case PinDrive::Keep:  return _( "keep" );
    }

    return "?";
}


wxString StateLabel( PinState aState )
{
    switch( aState )
    {
    case PinState::Unknown:  return wxString::FromUTF8( "–" );
    case PinState::HighZ:    return _( "off (high-Z)" );
    case PinState::Low:      return _( "drives 0" );
    case PinState::High:     return _( "drives 1" );
    case PinState::Toggling: return _( "toggling" );
    }

    return "?";
}


wxString PinList( const std::vector<std::string>& aPins, size_t aMax )
{
    wxString text;

    for( size_t i = 0; i < aPins.size() && i < aMax; ++i )
        text << ( i ? ", " : "" ) << fromUtf8( aPins[i] );

    if( aPins.size() > aMax )
        text << wxString::Format( _( " and %zu more" ), aPins.size() - aMax );

    return text;
}


static wxString netOf( const PinNets& aNets, const std::string& aPin )
{
    auto it = aNets.find( aPin );
    return it != aNets.end() ? fromUtf8( it->second ) : wxString();
}


// ============================================================================
// Setup dialog
// ============================================================================

ExtestSetupDialog::ExtestSetupDialog( wxWindow* aParent, const wxString& aDeviceLabel,
                                      std::shared_ptr<const bscan::BsdlDevice> aDevice,
                                      const std::string& aPinMap,
                                      const std::map<std::string, PinDrive>& aSettings,
                                      const PinNets& aNets,
                                      const std::vector<bscan::Bits>& aSamples,
                                      bool aExtestActive,
                                      const std::vector<std::string>& aFocusPins,
                                      TestRolesFunction aTestRoles ) :
        wxDialog( aParent, wxID_ANY, wxString::Format( _( "EXTEST setup: %s" ), aDeviceLabel ),
                  wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        m_device( std::move( aDevice ) ),
        m_pinMap( aPinMap ),
        m_settings( aSettings ),
        m_nets( aNets ),
        m_samples( aSamples ),
        m_testRoles( std::move( aTestRoles ) )
{
    m_pins = bscan::OutputPins( *m_device, m_pinMap );

    auto* top = new wxBoxSizer( wxVERTICAL );

    auto* intro = new wxStaticText(
            this, wxID_ANY,
            _( "In EXTEST the boundary-scan register drives ALL pins of this device; its own "
               "logic is disconnected. Choose the state every output pin gets when EXTEST "
               "starts. Pins left at \"safe (BSDL)\" get the vendor's safe value.\n"
               "Check pins that drive power stages, enables or resets in particular: "
               "a wrong static level can damage the board." ) );
    intro->Wrap( FromDIP( 640 ) );
    top->Add( intro, 0, wxALL, FromDIP( 8 ) );

    // Filters (the list keeps its rows while start states are edited; it is
    // filtered again when a filter changes)
    auto* filters = new wxBoxSizer( wxHORIZONTAL );
    m_onlyNets    = new wxCheckBox( this, wxID_ANY, _( "Only pins connected to a net" ) );
    m_onlyNets->SetValue( true );
    m_onlyChanges = new wxCheckBox( this, wxID_ANY, _( "Only pins that change" ) );
    m_onlyChanges->SetToolTip( _( "Pins whose state when EXTEST starts differs from their "
                                  "state now" ) );

    if( aSamples.empty() )
    {
        m_onlyChanges->Disable();
        m_onlyChanges->SetToolTip( _( "The current state of the pins is not known" ) );
    }

    // Explicit width: the default size on GTK leaves no room for the text
    m_search = new wxSearchCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxSize( FromDIP( 200 ), -1 ) );
    m_search->SetDescriptiveText( _( "Pin, port or net" ) );
    m_search->ShowCancelButton( true );
    filters->Add( m_onlyNets, 0, wxALIGN_CENTER_VERTICAL );
    filters->Add( m_onlyChanges, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 16 ) );
    filters->Add( m_search, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 16 ) );
    top->Add( filters, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 8 ) );

    m_list = new wxDataViewListCtrl( this, wxID_ANY, wxDefaultPosition,
                                     wxSize( FromDIP( m_testRoles ? 900 : 720 ), FromDIP( 360 ) ),
                                     wxDV_ROW_LINES | wxDV_VERT_RULES );

    // State columns: as wide as their longest possible text
    auto stateWidth = [this]( const wxString& aHeader, const wxString& aSuffix )
    {
        int width = m_list->GetTextExtent( aHeader ).x;

        for( PinState s : { PinState::Unknown, PinState::HighZ, PinState::Low, PinState::High,
                            PinState::Toggling } )
            width = std::max( width, m_list->GetTextExtent( StateLabel( s ) + aSuffix ).x );

        return width + FromDIP( 24 );
    };

    wxString nowHeader = aExtestActive ? _( "Now" ) : _( "Now (SAMPLE)" );

    m_list->AppendTextColumn( _( "Pin" ) );
    m_list->AppendTextColumn( _( "Port" ) );
    m_list->AppendTextColumn( _( "Net" ) );
    m_list->AppendTextColumn( nowHeader, wxDATAVIEW_CELL_INERT, stateWidth( nowHeader, {} ) );

    wxArrayString choices;

    for( PinDrive d : DRIVES )
        choices.Add( DriveLabel( d ) );

    int startWidth = m_list->GetTextExtent( _( "Start state" ) ).x;

    for( const wxString& c : choices )
        startWidth = std::max( startWidth, m_list->GetTextExtent( c ).x );

    auto* startCol = new wxDataViewColumn( _( "Start state" ),
                                           new DropDownChoiceRenderer( choices ), SC_START,
                                           startWidth + FromDIP( 40 ) );
    m_list->AppendColumn( startCol, "string" );
    m_list->AppendTextColumn( _( "In EXTEST" ), wxDATAVIEW_CELL_INERT,
                              stateWidth( _( "In EXTEST" ), CHANGES_MARK ) );

    if( m_testRoles )
        m_list->AppendTextColumn( _( "Interconnect test" ) );

    top->Add( m_list, 1, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_summary = new wxStaticText( this, wxID_ANY, wxEmptyString );
    top->Add( m_summary, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 8 ) );

    m_testSummary = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_testSummary->SetForegroundColour( wxColour( 200, 40, 30 ) );
    top->Add( m_testSummary, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 4 ) );

    // Saved settings for pins that are not outputs of this BSDL file / pin map
    auto* stale   = new wxBoxSizer( wxHORIZONTAL );
    m_staleText   = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_staleText->SetForegroundColour( wxColour( 200, 40, 30 ) );
    m_staleRemove = new wxButton( this, wxID_ANY, _( "Remove them" ) );
    stale->Add( m_staleText, 1, wxALIGN_CENTER_VERTICAL );
    stale->Add( m_staleRemove, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 8 ) );
    top->Add( stale, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );

    m_staleRemove->Bind( wxEVT_BUTTON,
                         [this]( wxCommandEvent& )
                         {
                             for( const std::string& p :
                                  bscan::StaleSettings( *m_device, m_pinMap, m_settings ) )
                                 m_settings.erase( p );

                             updateStale();
                         } );

    auto* buttons  = new wxBoxSizer( wxHORIZONTAL );
    auto* activate = new wxButton( this, wxID_ANY, _( "Review and activate..." ) );
    auto* save     = new wxButton( this, wxID_OK, _( "Save" ) );
    auto* cancel   = new wxButton( this, wxID_CANCEL );

    if( aExtestActive )
    {
        activate->Disable();
        activate->SetToolTip( _( "EXTEST is already active on this device" ) );
    }

    buttons->Add( activate, 0, wxRIGHT, FromDIP( 8 ) );
    buttons->AddStretchSpacer();
    buttons->Add( save, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->Add( cancel, 0 );
    top->Add( buttons, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    updateStale();
    SetSizerAndFit( top );
    cancel->SetDefault();

    m_onlyNets->Bind( wxEVT_CHECKBOX, [this]( wxCommandEvent& ) { fill(); } );
    m_onlyChanges->Bind( wxEVT_CHECKBOX, [this]( wxCommandEvent& ) { fill(); } );
    m_search->Bind( wxEVT_TEXT, [this]( wxCommandEvent& ) { fill(); } );
    m_search->Bind( wxEVT_SEARCHCTRL_CANCEL_BTN,
                    [this]( wxCommandEvent& )
                    {
                        m_search->ChangeValue( wxEmptyString );
                        fill();
                    } );
    m_list->Bind( wxEVT_DATAVIEW_ITEM_VALUE_CHANGED, &ExtestSetupDialog::onValueChanged, this );
    activate->Bind( wxEVT_BUTTON,
                    [this]( wxCommandEvent& )
                    {
                        m_activate = true;
                        EndModal( wxID_OK );
                    } );

    // Pins the user clicked on: show them even without a net, and select them
    for( const std::string& pin : aFocusPins )
        if( netOf( m_nets, pin ).empty() )
            m_onlyNets->SetValue( false );

    fill();

    for( const std::string& pin : aFocusPins )
    {
        for( size_t r = 0; r < m_rowPin.size(); ++r )
        {
            if( m_pins[m_rowPin[r]].pin != pin )
                continue;

            wxDataViewItem item = m_list->RowToItem( static_cast<int>( r ) );
            m_list->Select( item );

            if( pin == aFocusPins.front() )
                m_list->EnsureVisible( item );
        }
    }
}


void ExtestSetupDialog::fill()
{
    m_list->DeleteAllItems();
    m_rowPin.clear();

    // Pins whose state changes when EXTEST starts (only with samples)
    std::set<std::string> changing;

    if( m_onlyChanges->GetValue() && !m_samples.empty() )
    {
        for( const bscan::PinPlan& p :
             bscan::PlanExtest( *m_device, m_pinMap, m_samples, m_settings ).pins )
        {
            if( p.Changes() )
                changing.insert( p.pin );
        }
    }

    wxString search = m_search->GetValue().Trim().Trim( false ).Lower();

    for( size_t i = 0; i < m_pins.size(); ++i )
    {
        const bscan::OutputPin& p   = m_pins[i];
        wxString                net = netOf( m_nets, p.pin );

        if( m_onlyNets->GetValue() && net.empty() )
            continue;

        if( m_onlyChanges->GetValue() && !m_samples.empty() && !changing.contains( p.pin ) )
            continue;

        if( !search.empty() && !fromUtf8( p.pin ).Lower().Contains( search )
            && !fromUtf8( p.cells.port ).Lower().Contains( search )
            && !net.Lower().Contains( search ) )
        {
            continue;
        }

        auto     it    = m_settings.find( p.pin );
        PinDrive drive = it != m_settings.end() ? it->second : PinDrive::Safe;

        wxVector<wxVariant> row;
        row.push_back( fromUtf8( p.pin ) );
        row.push_back( fromUtf8( p.cells.port ) );
        row.push_back( net );
        row.push_back( wxString() );   // now, filled by updatePlan
        row.push_back( DriveLabel( drive ) );
        row.push_back( wxString() );   // planned

        if( m_testRoles )
            row.push_back( wxString() );   // filled by updateTestRoles

        m_list->AppendItem( row );
        m_rowPin.push_back( i );
    }

    updatePlan();
}


void ExtestSetupDialog::onValueChanged( wxDataViewEvent& aEvent )
{
    if( aEvent.GetColumn() != SC_START )
        return;

    int row = m_list->ItemToRow( aEvent.GetItem() );

    if( row < 0 || static_cast<size_t>( row ) >= m_rowPin.size() )
        return;

    wxString           label = m_list->GetTextValue( row, SC_START );
    const std::string& pin   = m_pins[m_rowPin[row]].pin;

    for( PinDrive d : DRIVES )
    {
        if( DriveLabel( d ) == label )
        {
            if( d == PinDrive::Safe )
                m_settings.erase( pin );
            else
                m_settings[pin] = d;
        }
    }

    // Values of other rows can change too (shared control cells)
    CallAfter( [this] { updatePlan(); } );
}


void ExtestSetupDialog::updateStale()
{
    std::vector<std::string> stale = bscan::StaleSettings( *m_device, m_pinMap, m_settings );
    bool                     show  = !stale.empty();

    if( show )
    {
        wxString text = wxString::Format(
                wxPLURAL( "%zu saved setting refers to a pin that is not an output pin of "
                          "this BSDL file / pin map (ignored): %s",
                          "%zu saved settings refer to pins that are not output pins of this "
                          "BSDL file / pin map (ignored): %s",
                          stale.size() ),
                stale.size(), PinList( stale ) );
        m_staleText->SetLabel( text );
        m_staleText->Wrap( FromDIP( 520 ) );
    }

    m_staleText->Show( show );
    m_staleRemove->Show( show );
    Layout();
}


void ExtestSetupDialog::updatePlan()
{
    bscan::ExtestPlan plan = bscan::PlanExtest( *m_device, m_pinMap, m_samples, m_settings );

    std::map<std::string, const bscan::PinPlan*> byPin;

    for( const bscan::PinPlan& p : plan.pins )
        byPin[p.pin] = &p;

    for( size_t row = 0; row < m_rowPin.size(); ++row )
    {
        const bscan::PinPlan* p = byPin[m_pins[m_rowPin[row]].pin];

        if( !p )
            continue;

        wxString now     = m_samples.empty() ? wxString::FromUTF8( "–" )
                                             : StateLabel( p->current );
        wxString planned = StateLabel( p->planned );

        if( !m_samples.empty() && p->Changes() )
            planned << CHANGES_MARK;

        if( m_list->GetTextValue( row, SC_NOW ) != now )
            m_list->SetTextValue( now, row, SC_NOW );

        if( m_list->GetTextValue( row, SC_PLANNED ) != planned )
            m_list->SetTextValue( planned, row, SC_PLANNED );
    }

    int changes = 0, unset = 0;

    for( const bscan::PinPlan& p : plan.pins )
    {
        changes += ( !m_samples.empty() && p.Changes() ) ? 1 : 0;
        unset += ( !p.explicitSetting && !netOf( m_nets, p.pin ).empty() ) ? 1 : 0;
    }

    wxString text = wxString::Format( _( "%zu output pins; %d change when EXTEST starts; "
                                         "%d pins with a net use the BSDL default." ),
                                      plan.pins.size(), changes, unset );

    for( const std::string& c : plan.conflicts )
        text << "\n" << _( "Conflict: " ) << fromUtf8( c );

    m_summary->SetLabel( text );
    updateTestRoles();
    Layout();
}


// The interconnect test with the starting states as they are now in the
// dialog: which nets of this device it tests, and which the starting states
// set here keep out
void ExtestSetupDialog::updateTestRoles()
{
    m_testSummary->Show( false );

    if( !m_testRoles )
        return;

    std::map<std::string, bscan::PinTestRole> roles = m_testRoles( m_settings );
    std::set<std::string>                     fixedNets;

    for( size_t row = 0; row < m_rowPin.size(); ++row )
    {
        wxString text;
        auto     it = roles.find( m_pins[m_rowPin[row]].pin );

        if( it != roles.end() )
        {
            const bscan::PinTestRole& r = it->second;
            text                        = fromUtf8( r.net ) + ": ";

            switch( r.kind )
            {
            case bscan::PinTestRole::Kind::Tested:
                text << _( "tested" );
                break;

            case bscan::PinTestRole::Kind::FixedHere:
                text = wxString::FromUTF8( "⚠ " ) + text
                       + ( r.currentState ? _( "not tested: its current EXTEST state (set by "
                                               "hand) fixes it" )
                                          : _( "not tested: this starting state fixes it" ) );
                break;

            case bscan::PinTestRole::Kind::NotTested:
                text << _( "not tested" ) << " (" << fromUtf8( r.reason ) << ")";
                break;
            }
        }

        if( m_list->GetTextValue( row, SC_TEST ) != text )
            m_list->SetTextValue( text, row, SC_TEST );
    }

    // Nets kept out by starting states (all pins, also those the filter hides)
    for( const auto& [pin, r] : roles )
        if( r.kind == bscan::PinTestRole::Kind::FixedHere && !r.currentState )
            fixedNets.insert( r.net );

    if( fixedNets.empty() )
        return;

    m_testSummary->SetLabel( wxString::Format(
            wxPLURAL( "%zu net is not tested in the interconnect test because of starting states "
                      "set here (set them to \"safe (BSDL)\" to test it).",
                      "%zu nets are not tested in the interconnect test because of starting states "
                      "set here (set them to \"safe (BSDL)\" to test them).",
                      fixedNets.size() ),
            fixedNets.size() ) );
    m_testSummary->Wrap( FromDIP( 640 ) );
    m_testSummary->Show( true );
}


// ============================================================================
// Review / confirmation
// ============================================================================

static size_t n_changes( const bscan::ExtestPlan& aPlan )
{
    return std::count_if( aPlan.pins.begin(), aPlan.pins.end(),
                          []( const bscan::PinPlan& p ) { return p.Changes(); } );
}


bool ConfirmExtest( wxWindow* aParent, const wxString& aDeviceLabel,
                    const bscan::ExtestPlan& aPlan, const PinNets& aNets,
                    const std::vector<std::string>& aStale, bool aTakeOver,
                    bool aBackgroundPolling )
{
    wxDialog dlg( aParent, wxID_ANY,
                  wxString::Format( aTakeOver ? _( "Take over EXTEST on %s?" )
                                              : _( "Activate EXTEST on %s?" ),
                                    aDeviceLabel ),
                  wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER );

    auto* top = new wxBoxSizer( wxVERTICAL );

    wxString intro;

    if( !aPlan.conflicts.empty() )
    {
        intro = _( "EXTEST cannot be activated with these settings:" );

        for( const std::string& c : aPlan.conflicts )
            intro << "\n  - " << fromUtf8( c );
    }
    else
    {
        intro = aTakeOver
                        ? _( "The device is already in EXTEST; what it drives now cannot be read "
                             "(every scan also sets the outputs). After taking over, the output "
                             "pins have these states; the pins do not return to the device's own "
                             "logic in between.\nPins marked \"BSDL default\" were not set by "
                             "you." )
                : n_changes( aPlan ) == 0
                        ? _( "Compared with the state measured with SAMPLE just now." )
                        : _( "These output pins change their state when EXTEST starts (measured "
                             "with SAMPLE just now). All other output pins keep their current "
                             "state.\nPins marked \"BSDL default\" were not set by you." );
    }

    auto* text = new wxStaticText( &dlg, wxID_ANY, intro );
    text->Wrap( dlg.FromDIP( 640 ) );
    top->Add( text, 0, wxALL, dlg.FromDIP( 8 ) );

    if( !aStale.empty() )
    {
        auto* stale = new wxStaticText(
                &dlg, wxID_ANY,
                wxString::Format( _( "Ignored: saved settings for pins that are not output pins "
                                     "of this BSDL file / pin map: %s. These pins are not "
                                     "set by you; check that this is the right BSDL file and "
                                     "package." ),
                                  PinList( aStale ) ) );
        stale->SetForegroundColour( wxColour( 200, 40, 30 ) );
        stale->Wrap( dlg.FromDIP( 640 ) );
        top->Add( stale, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP( 8 ) );
    }

    if( aBackgroundPolling )
    {
        auto* polling = new wxStaticText(
                &dlg, wxID_ANY,
                _( "OpenOCD polls its targets in the background. Every poll puts this device "
                   "into BYPASS for a moment, and its pins return to the device's own logic until "
                   "the next scan restores EXTEST. Turn polling off first: 'poll off' in "
                   "OpenOCD." ) );
        polling->SetForegroundColour( wxColour( 200, 40, 30 ) );
        polling->Wrap( dlg.FromDIP( 640 ) );
        top->Add( polling, 0, wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP( 8 ) );
    }

    auto* list = new wxListView( &dlg, wxID_ANY, wxDefaultPosition,
                                 wxSize( dlg.FromDIP( 680 ), dlg.FromDIP( 300 ) ),
                                 wxLC_REPORT | wxLC_HRULES );
    list->AppendColumn( _( "Pin" ) );
    list->AppendColumn( _( "Port" ) );
    list->AppendColumn( _( "Net" ) );
    list->AppendColumn( _( "Now" ) );
    list->AppendColumn( _( "In EXTEST" ) );
    list->AppendColumn( _( "Setting" ) );

    long n      = 0;
    int  marked = 0;   // red rows

    for( const bscan::PinPlan& p : aPlan.pins )
    {
        if( !p.Changes() )
            continue;

        list->InsertItem( n, fromUtf8( p.pin ) );
        list->SetItem( n, 1, fromUtf8( p.port ) );
        list->SetItem( n, 2, netOf( aNets, p.pin ) );
        list->SetItem( n, 3, StateLabel( p.current ) );
        list->SetItem( n, 4, StateLabel( p.planned ) );
        list->SetItem( n, 5, p.explicitSetting ? DriveLabel( p.drive ) : _( "BSDL default" ) );

        if( !p.explicitSetting && !netOf( aNets, p.pin ).empty() )
        {
            list->SetItemTextColour( n, wxColour( 200, 40, 30 ) );
            ++marked;
        }

        ++n;
    }

    for( int c = 0; c < list->GetColumnCount(); ++c )
        list->SetColumnWidth( c, wxLIST_AUTOSIZE_USEHEADER );

    top->Add( list, 1, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP( 8 ) );

    if( marked > 0 )
    {
        auto* legend = new wxStaticText(
                &dlg, wxID_ANY,
                wxString::Format( _( "Red (%d): connected to a net on this board, but left at the "
                                     "BSDL default instead of a state chosen by you. Check these "
                                     "in particular." ),
                                  marked ) );
        legend->SetForegroundColour( wxColour( 200, 40, 30 ) );
        legend->Wrap( dlg.FromDIP( 640 ) );
        top->Add( legend, 0, wxLEFT | wxRIGHT | wxTOP, dlg.FromDIP( 8 ) );
    }

    if( n == 0 && aPlan.conflicts.empty() )
    {
        // An empty list looks like an error: say explicitly that nothing changes
        list->Hide();

        auto* none = new wxStaticText(
                &dlg, wxID_ANY,
                wxString::Format( _( "No output pin changes its state.\n\nEXTEST will drive all "
                                     "%zu output pins with the levels measured just now. From "
                                     "then on the device's own logic no longer controls them." ),
                                  aPlan.pins.size() ) );
        none->SetFont( none->GetFont().Bold() );
        none->Wrap( dlg.FromDIP( 640 ) );
        top->Add( none, 1, wxEXPAND | wxALL, dlg.FromDIP( 16 ) );
    }

    auto* summary = new wxStaticText(
            &dlg, wxID_ANY,
            wxString::Format( _( "%ld of %zu output pins change." ), n, aPlan.pins.size() ) );
    top->Add( summary, 0, wxALL, dlg.FromDIP( 8 ) );

    auto* checked = new wxCheckBox( &dlg, wxID_ANY,
                                    _( "I have checked these changes for this board" ) );
    top->Add( checked, 0, wxLEFT | wxRIGHT, dlg.FromDIP( 8 ) );

    auto* buttons  = new wxBoxSizer( wxHORIZONTAL );
    auto* activate = new wxButton( &dlg, wxID_OK,
                                   aTakeOver ? _( "Take over" ) : _( "Activate EXTEST" ) );
    auto* cancel   = new wxButton( &dlg, wxID_CANCEL );
    activate->Disable();
    buttons->AddStretchSpacer();
    buttons->Add( activate, 0, wxRIGHT, dlg.FromDIP( 4 ) );
    buttons->Add( cancel, 0 );
    top->Add( buttons, 0, wxEXPAND | wxALL, dlg.FromDIP( 8 ) );

    if( !aPlan.conflicts.empty() )
        checked->Disable();

    checked->Bind( wxEVT_CHECKBOX,
                   [activate]( wxCommandEvent& aEvent ) { activate->Enable( aEvent.IsChecked() ); } );

    dlg.SetSizerAndFit( top );
    cancel->SetDefault();
    cancel->SetFocus();

    return dlg.ShowModal() == wxID_OK && aPlan.conflicts.empty() && checked->GetValue();
}


// ============================================================================
// Take over with the saved image
// ============================================================================

TakeOverChoice ConfirmTakeOver( wxWindow* aParent, const wxString& aDeviceLabel,
                                const bscan::BsdlDevice& aDevice, const std::string& aPinMap,
                                const bscan::Bits& aImage, const PinNets& aNets,
                                const wxString& aSavedWhen )
{
    wxDialog dlg( aParent, wxID_ANY, wxString::Format( _( "Take over EXTEST on %s?" ), aDeviceLabel ),
                  wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER );

    auto* top   = new wxBoxSizer( wxVERTICAL );
    auto* intro = new wxStaticText(
            &dlg, wxID_ANY,
            wxString::Format(
                    _( "OpenOCD reports that the device is in EXTEST. When this window was closed "
                       "(%s), it drove the states below; taking over continues with exactly "
                       "these.\n"
                       "What the device drives now cannot be read. If another program changed "
                       "the outputs since, the pins switch back to these states.\n"
                       "\"New plan...\" chooses the states in the setup instead." ),
                    aSavedWhen ) );
    intro->Wrap( dlg.FromDIP( 640 ) );
    top->Add( intro, 0, wxALL, dlg.FromDIP( 8 ) );

    auto* list = new wxListView( &dlg, wxID_ANY, wxDefaultPosition,
                                 wxSize( dlg.FromDIP( 640 ), dlg.FromDIP( 280 ) ),
                                 wxLC_REPORT | wxLC_HRULES );
    list->AppendColumn( _( "Pin" ) );
    list->AppendColumn( _( "Port" ) );
    list->AppendColumn( _( "Net" ) );
    list->AppendColumn( _( "Driven" ) );

    // Pins with a net first: those are the ones that matter on this board
    std::vector<bscan::OutputPin> pins = bscan::OutputPins( aDevice, aPinMap );
    std::stable_partition( pins.begin(), pins.end(), [&]( const bscan::OutputPin& p )
                           { return !netOf( aNets, p.pin ).empty(); } );

    long n = 0;

    for( const bscan::OutputPin& p : pins )
    {
        list->InsertItem( n, fromUtf8( p.pin ) );
        list->SetItem( n, 1, fromUtf8( p.cells.port ) );
        list->SetItem( n, 2, netOf( aNets, p.pin ) );
        list->SetItem( n, 3, StateLabel( bscan::StateFromImage( aImage, p.cells ) ) );
        ++n;
    }

    for( int c = 0; c < list->GetColumnCount(); ++c )
        list->SetColumnWidth( c, wxLIST_AUTOSIZE_USEHEADER );

    top->Add( list, 1, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP( 8 ) );

    auto* checked = new wxCheckBox( &dlg, wxID_ANY,
                                    _( "I have checked these states for this board" ) );
    top->Add( checked, 0, wxALL, dlg.FromDIP( 8 ) );

    auto* buttons  = new wxBoxSizer( wxHORIZONTAL );
    auto* newPlan  = new wxButton( &dlg, wxID_ANY, _( "New plan..." ) );
    auto* takeOver = new wxButton( &dlg, wxID_OK, _( "Take over" ) );
    auto* cancel   = new wxButton( &dlg, wxID_CANCEL );
    takeOver->Disable();
    buttons->Add( newPlan, 0 );
    buttons->AddStretchSpacer();
    buttons->Add( takeOver, 0, wxRIGHT, dlg.FromDIP( 4 ) );
    buttons->Add( cancel, 0 );
    top->Add( buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, dlg.FromDIP( 8 ) );

    checked->Bind( wxEVT_CHECKBOX,
                   [takeOver]( wxCommandEvent& aEvent ) { takeOver->Enable( aEvent.IsChecked() ); } );
    newPlan->Bind( wxEVT_BUTTON, [&dlg]( wxCommandEvent& ) { dlg.EndModal( wxID_NEW ); } );

    dlg.SetSizerAndFit( top );
    cancel->SetDefault();
    cancel->SetFocus();

    int result = dlg.ShowModal();

    if( result == wxID_NEW )
        return TakeOverChoice::NewPlan;

    if( result == wxID_OK && checked->GetValue() )
        return TakeOverChoice::SavedImage;

    return TakeOverChoice::Cancel;
}
