// SPDX-License-Identifier: GPL-3.0-only
#include "chip_view.h"

#include <algorithm>
#include <optional>
#include <set>

#include <wx/choice.h>
#include <wx/config.h>
#include <wx/menu.h>
#include <wx/panel.h>
#include <wx/persist/toplevel.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "chip_canvas.h"
#include "extest_controller.h"
#include "gui_events.h"
#include "session.h"

using bscan::PackageLayout;
using bscan::PackagePin;
using bscan::PackageShape;
using bscan::PinDrive;
using bscan::PinValue;
using bscan::PlacedPin;
using Status = kicad::BoardState::Status;

static const char* CFG_CHIP_TAP = "ChipViewTap";

// Layout choices, in the order of the choice control
static const PackageShape::Type SHAPES[] = { PackageShape::Type::Auto, PackageShape::Type::Bga,
                                             PackageShape::Type::Quad, PackageShape::Type::Dual };


static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


static std::string toUtf8( const wxString& aText )
{
    return std::string( aText.utf8_str() );
}


static std::string chipKey( const std::string& aReference, const std::string& aPin )
{
    return "chip:" + aReference + "|" + aPin;
}


ChipView::ChipView( wxWindow* aParent, Session& aSession, ExtestController& aExtest ) :
        wxFrame( aParent, wxID_ANY, _( "Chip view" ), wxDefaultPosition, wxDefaultSize,
                 wxDEFAULT_FRAME_STYLE | wxFRAME_FLOAT_ON_PARENT ),
        m_session( aSession ),
        m_extest( aExtest )
{
    m_panel   = new wxPanel( this );
    auto* top = new wxBoxSizer( wxVERTICAL );

    // Chooser and layout
    auto* line = new wxBoxSizer( wxHORIZONTAL );
    m_chooser  = new wxChoice( m_panel, wxID_ANY );
    m_chooser->SetToolTip( _( "Chips with a BSDL file (JTAG chain tab)" ) );
    m_shape = new wxChoice( m_panel, wxID_ANY );
    m_shape->Append( _( "Auto" ) );
    m_shape->Append( _( "BGA" ) );
    m_shape->Append( _( "Quad (QFP, QFN)" ) );
    m_shape->Append( _( "Dual (SOIC, TSSOP)" ) );
    m_shape->SetToolTip( _( "How the pins are arranged. BSDL files have no package geometry: "
                            "Auto guesses it from the pin names." ) );
    m_sides = new wxTextCtrl( m_panel, wxID_ANY, wxEmptyString, wxDefaultPosition,
                              FromDIP( wxSize( 110, -1 ) ), wxTE_PROCESS_ENTER );
    m_sides->SetHint( _( "e.g. 30,20,30,20" ) );
    m_sides->SetToolTip( _( "Quad: pins on the left, bottom, right and top side (from pin 1, "
                            "counter-clockwise). Empty: spread evenly." ) );

    line->Add( new wxStaticText( m_panel, wxID_ANY, _( "Chip:" ) ), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
    line->Add( m_chooser, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 12 ) );
    line->Add( new wxStaticText( m_panel, wxID_ANY, _( "Layout:" ) ), 0,
               wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
    line->Add( m_shape, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP( 4 ) );
    line->Add( m_sides, 0, wxALIGN_CENTER_VERTICAL );
    top->Add( line, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_shapeNote = new wxStaticText( m_panel, wxID_ANY, wxEmptyString );
    m_shapeNote->SetForegroundColour( wxColour( 190, 30, 30 ) );
    top->Add( m_shapeNote, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 8 ) );

    // Without a board the plugin cannot check what the pins drive
    m_warning = new wxStaticText( m_panel, wxID_ANY, wxEmptyString );
    m_warning->SetForegroundColour( wxColour( 190, 30, 30 ) );
    m_warning->SetFont( m_warning->GetFont().Bold() );
    top->Add( m_warning, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );

    m_canvas = new ChipCanvas( m_panel, m_drawing );
    top->Add( m_canvas, 1, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_panel->SetSizer( top );
    auto* frameSizer = new wxBoxSizer( wxVERTICAL );
    frameSizer->Add( m_panel, 1, wxEXPAND );
    SetSizer( frameSizer );
    SetSize( FromDIP( wxSize( 640, 600 ) ) );
    wxPersistentRegisterAndRestore( this, "ChipView" );

    m_chooser->Bind( wxEVT_CHOICE,
                     [this]( wxCommandEvent& )
                     {
                         int sel = m_chooser->GetSelection();
                         m_tap   = sel >= 0 && sel < static_cast<int>( m_taps.size() ) ? m_taps[sel]
                                                                                       : "";
                         wxConfigBase::Get()->Write( CFG_CHIP_TAP, fromUtf8( m_tap ) );
                         showChip();
                     } );
    m_shape->Bind( wxEVT_CHOICE, [this]( wxCommandEvent& ) { onLayoutChanged(); } );
    m_sides->Bind( wxEVT_TEXT_ENTER, [this]( wxCommandEvent& ) { onLayoutChanged(); } );
    m_sides->Bind( wxEVT_KILL_FOCUS,
                   [this]( wxFocusEvent& aEvent )
                   {
                       aEvent.Skip();
                       onLayoutChanged();
                   } );
    Bind( wxEVT_CLOSE_WINDOW, &ChipView::onClose, this );
    m_canvas->Bind( EVT_CHIP_CANVAS_CLICK,
                    [this]( wxCommandEvent& aEvent )
                    {
                        std::string pin   = toUtf8( aEvent.GetString() );
                        bool        right = aEvent.GetInt() & CLICK_RIGHT;
                        bool        add   = aEvent.GetInt() & CLICK_ADD;

                        // Right-click on a selected pin keeps the selection
                        if( !right || add || !m_drawing.selected.contains( pin ) )
                            selectPin( pin, add );

                        if( right )
                            pinMenu();
                    } );

    m_tap = toUtf8( wxConfigBase::Get()->Read( CFG_CHIP_TAP, wxEmptyString ) );
}


void ChipView::onClose( wxCloseEvent& aEvent )
{
    if( !aEvent.CanVeto() )
    {
        aEvent.Skip();
        return;
    }

    // Kept for the next time; nothing is sampled for it while hidden
    Hide();
    publishPins();
}


std::string ChipView::currentReference() const
{
    const bscan::TapAssignment* a = m_session.config.Find( m_tap );
    return a ? a->reference : std::string();
}


void ChipView::UpdateState()
{
    fillChooser();
    showChip();
}


void ChipView::fillChooser()
{
    std::vector<std::string> taps;

    for( const bscan::TapAssignment& a : m_session.config.taps )
        if( !a.bsdl.empty() )
            taps.push_back( a.tap );

    wxArrayString labels;

    for( const std::string& t : taps )
        labels.Add( m_session.DeviceLabel( t ) );

    if( taps != m_taps || static_cast<size_t>( m_chooser->GetCount() ) != labels.size() )
    {
        m_taps = taps;
        m_chooser->Set( labels );
    }
    else
    {
        for( size_t i = 0; i < labels.size(); ++i )
            if( m_chooser->GetString( static_cast<unsigned>( i ) ) != labels[i] )
                m_chooser->SetString( static_cast<unsigned>( i ), labels[i] );
    }

    auto it = std::ranges::find( m_taps, m_tap );

    if( it == m_taps.end() )
        m_tap = m_taps.empty() ? "" : m_taps.front();

    it = std::ranges::find( m_taps, m_tap );
    m_chooser->SetSelection( it == m_taps.end() ? wxNOT_FOUND
                                                : static_cast<int>( it - m_taps.begin() ) );
}


void ChipView::showChip()
{
    const bscan::TapAssignment* a = m_session.config.Find( m_tap );
    m_drawing.message.clear();

    std::shared_ptr<const bscan::BsdlDevice> dev;
    std::string                              err;

    std::string& message = m_drawing.message;

    if( !a )
        message = toUtf8( _( "No chip with a BSDL file: assign one in the JTAG chain tab." ) );
    else if( !( dev = m_session.DeviceFor( a->bsdl, &err ) ) )
        message = err;
    else if( a->reference.empty() )
        message = toUtf8( _( "Give this chip a reference in the JTAG chain tab (any name, "
                             "e.g. U1): pins are identified by reference and pin." ) );

    std::vector<PackagePin> pins;

    if( dev )
        pins = dev->PackagePins( a->pinMap );

    // Keep the states of pins that are still there (same chip, layout changed)
    std::map<std::string, ChipPinState> states;
    PinNets                         nets = a ? m_session.NetsOf( a->reference ) : PinNets();
    std::vector<std::string>        names;

    for( const PackagePin& p : pins )
    {
        ChipPinState s;

        auto it = m_drawing.pins.find( p.pin );

        if( it != m_drawing.pins.end() && it->second.info.cells.port == p.cells.port )
            s = it->second;

        s.info = p;

        if( auto n = nets.find( p.pin ); n != nets.end() )
            s.net = n->second;
        else
            s.net.clear();

        states[p.pin] = s;
        names.push_back( p.pin );
    }

    m_drawing.pins = std::move( states );

    std::erase_if( m_drawing.selected,
                   [this]( const std::string& p ) { return !m_drawing.pins.contains( p ); } );

    auto shape = PackageShape::Parse( a ? a->package : "" ).value_or( PackageShape{} );
    m_drawing.layout   = bscan::LayoutPackage( names, shape );

    if( a && !dev )
        m_drawing.layout = {};

    m_drawing.name   = a ? a->reference : std::string();
    m_drawing.extest = !m_tap.empty() && m_session.InExtest( m_tap );

    updateLayoutControls();
    updateWarning();
    publishPins();
    m_panel->Layout();
    m_canvas->Refresh();
}


void ChipView::updateLayoutControls()
{
    const bscan::TapAssignment* a     = m_session.config.Find( m_tap );
    PackageShape shape = PackageShape::Parse( a ? a->package : "" ).value_or( PackageShape{} );

    int index = static_cast<int>( std::ranges::find( SHAPES, shape.type ) - std::begin( SHAPES ) );
    m_shape->SetSelection( index );
    m_shape->Enable( a != nullptr );

    wxString sides;

    if( shape.sides.size() == 4 )
        sides = wxString::Format( "%d,%d,%d,%d", shape.sides[0], shape.sides[1], shape.sides[2],
                                  shape.sides[3] );

    if( !m_sides->HasFocus() )
        m_sides->ChangeValue( sides );

    m_sides->Enable( a && shape.type == PackageShape::Type::Quad );

    wxString note;

    if( !m_drawing.layout.problem.empty() )
        note = fromUtf8( m_drawing.layout.problem );
    else if( a && shape.type == PackageShape::Type::Auto && !m_drawing.layout.pins.empty() )
        note = m_drawing.layout.type == PackageShape::Type::Bga
                       ? _( "Guessed from the pin names: BGA. Set the layout if it is wrong." )
                       : _( "Guessed from the pin names: quad package, pins evenly on four "
                            "sides. Set the layout if it is wrong." );

    m_shapeNote->SetForegroundColour( m_drawing.layout.problem.empty()
                                              ? wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT )
                                              : wxColour( 190, 30, 30 ) );
    m_shapeNote->SetLabel( note );
}


void ChipView::onLayoutChanged()
{
    bscan::TapAssignment* a = m_session.config.Find( m_tap );

    if( !a )
        return;

    int          sel = m_shape->GetSelection();
    PackageShape shape;
    const int    count = static_cast<int>( std::size( SHAPES ) );
    shape.type = sel >= 0 && sel < count ? SHAPES[sel] : PackageShape::Type::Auto;

    if( shape.type == PackageShape::Type::Quad && !m_sides->GetValue().Trim().empty() )
    {
        auto parsed = PackageShape::Parse( "quad:" + toUtf8( m_sides->GetValue() ) );

        if( !parsed )
        {
            m_shapeNote->SetForegroundColour( wxColour( 190, 30, 30 ) );
            m_shapeNote->SetLabel( _( "Pins per side: four numbers, e.g. 30,20,30,20." ) );
            return;
        }

        shape = *parsed;
    }

    if( shape.ToString() == a->package )
        return;

    a->package = shape.ToString();
    m_session.Save();
    showChip();
}


void ChipView::updateWarning()
{
    const bscan::TapAssignment* a = m_session.config.Find( m_tap );
    wxString                    text;

    if( a && !a->reference.empty() && !m_session.board.padNets.contains( a->reference ) )
        text = wxString::FromUTF8( "⚠ " )
               + wxString::Format( _( "No board data for %s: the plugin cannot know what its pins "
                                      "are connected to. Check the board before you drive pins "
                                      "(EXTEST)." ),
                                   fromUtf8( a->reference ) );

    if( m_warning->GetLabel() != text )
    {
        m_warning->SetLabel( text );
        m_warning->Wrap( std::max( FromDIP( 300 ), m_panel->GetClientSize().x - FromDIP( 16 ) ) );
        m_warning->Show( !text.empty() );
        m_panel->Layout();
    }
}


void ChipView::publishPins()
{
    std::vector<std::string> pins;
    std::string              ref;

    if( IsShown() && m_drawing.message.empty() )
    {
        ref = currentReference();

        for( const auto& [pin, s] : m_drawing.pins )
            if( s.info.kind == PackagePin::Kind::BoundaryScan )
                pins.push_back( pin );
    }

    if( ref == m_session.chipViewReference && pins == m_session.chipViewPins )
        return;

    m_session.chipViewReference = ref;
    m_session.chipViewPins      = std::move( pins );
    SendGuiEvent( this, EVT_CHIP_PINS_CHANGED );
}


void ChipView::ShowSample( const bscan::LiveSampler::SampleUpdate& aUpdate )
{
    const std::string ref    = currentReference();
    const std::string prefix = chipKey( ref, "" );
    const auto        now    = std::chrono::steady_clock::now();
    bool              any    = false;

    for( const bscan::LiveSampler::PinResult& r : aUpdate.pins )
    {
        if( ref.empty() || !r.key.starts_with( prefix ) )
            continue;

        auto it = m_drawing.pins.find( r.key.substr( prefix.size() ) );

        if( it == m_drawing.pins.end() )
            continue;

        ChipPinState& s = it->second;
        bool      changed = s.sampled && s.ok && r.ok
                       && ( s.value.input != r.value.input || s.value.output != r.value.output );

        if( changed )
            s.changed = now;

        s.sampled = true;
        s.ok      = r.ok;
        s.value   = r.value;
        s.error   = r.error;
        any       = true;
    }

    if( any && IsShown() )
        m_canvas->Refresh();
}


void ChipView::selectPin( const std::string& aPin, bool aAdd )
{
    if( !m_drawing.pins.contains( aPin ) )
        return;

    std::set<std::string>& sel = m_drawing.selected;

    if( !aAdd )
        sel = { aPin };
    else if( !sel.erase( aPin ) )
        sel.insert( aPin );

    m_canvas->Refresh();

    const std::string ref = currentReference();

    if( ref.empty() )
        return;

    // The last pin taken out: nothing selected here, nor in KiCad
    if( sel.empty() )
    {
        m_session.ClearPick();
        SendGuiEvent( this, EVT_CHIP_PIN_SELECTED );
        return;
    }

    // The clicked pin is the selected pad of the Pins tab (or another one
    // if it was just taken out); all go to KiCad (those on the board)
    const std::string& picked = sel.contains( aPin ) ? aPin : *sel.begin();

    kicad::PadInfo pad;
    pad.reference = ref;
    pad.number    = picked;
    pad.net       = m_drawing.pins.at( picked ).net;

    m_session.PickPins( SelectedPins(), pad );
    SendGuiEvent( this, EVT_CHIP_PIN_SELECTED );
}


std::vector<std::pair<std::string, std::string>> ChipView::SelectedPins() const
{
    std::vector<std::pair<std::string, std::string>> pins;
    const std::string                                ref = currentReference();

    if( !ref.empty() )
        for( const std::string& p : m_drawing.selected )
            pins.push_back( { ref, p } );

    return pins;
}


void ChipView::HighlightPins( const std::vector<std::pair<std::string, std::string>>& aPins )
{
    const std::string     ref = currentReference();
    std::set<std::string> pins;

    for( const auto& [r, p] : aPins )
        if( r == ref && m_drawing.pins.contains( p ) )
            pins.insert( p );

    if( pins != m_drawing.selected )
    {
        m_drawing.selected = std::move( pins );
        m_canvas->Refresh();
    }
}


void ChipView::pinMenu()
{
    const std::string ref = currentReference();

    if( ref.empty() || m_drawing.selected.empty() )
        return;

    // The selected pins: those that can go on the watch list, be driven
    std::vector<bscan::WatchEntry> toWatch, toDrive;

    for( const std::string& pin : m_drawing.selected )
    {
        const ChipPinState& s = m_drawing.pins.at( pin );

        if( s.info.kind != PackagePin::Kind::BoundaryScan )
            continue;

        bool watched = std::ranges::any_of( m_session.config.watch,
                                            [&]( const bscan::WatchEntry& w )
                                            { return w.reference == ref && w.pad == pin; } );

        if( !watched )
            toWatch.push_back( { ref, pin, s.net } );

        if( s.info.cells.outputCell >= 0 )
            toDrive.push_back( { ref, pin, {} } );
    }

    enum
    {
        ID_WATCH = 1,
        ID_DRIVE0,
        ID_DRIVE1,
        ID_DRIVEZ
    };

    // One pin already on the list: say so
    const std::string& first   = *m_drawing.selected.begin();
    const bool         watched = m_drawing.selected.size() == 1
                         && std::ranges::any_of( m_session.config.watch,
                                                 [&]( const bscan::WatchEntry& w )
                                                 { return w.reference == ref && w.pad == first; } );

    wxMenu menu;
    menu.Append( ID_WATCH, watched ? _( "On the watch list" ) : _( "Add to watch list" ) );
    menu.Enable( ID_WATCH, !toWatch.empty() );
    menu.AppendSeparator();
    menu.Append( ID_DRIVE0, _( "Drive 0" ) );
    menu.Append( ID_DRIVE1, _( "Drive 1" ) );
    menu.Append( ID_DRIVEZ, _( "Drive Z (off)" ) );

    for( int id : { ID_DRIVE0, ID_DRIVE1, ID_DRIVEZ } )
        menu.Enable( id, !toDrive.empty() );

    switch( m_canvas->GetPopupMenuSelectionFromUser( menu ) )
    {
    case ID_WATCH:
        m_session.config.watch.insert( m_session.config.watch.end(), toWatch.begin(), toWatch.end() );
        m_session.Save();
        SendGuiEvent( this, EVT_WATCH_LIST_CHANGED );
        break;

    case ID_DRIVE0: m_extest.DrivePins( toDrive, PinDrive::Low ); break;
    case ID_DRIVE1: m_extest.DrivePins( toDrive, PinDrive::High ); break;
    case ID_DRIVEZ: m_extest.DrivePins( toDrive, PinDrive::HighZ ); break;
    default: break;
    }
}
