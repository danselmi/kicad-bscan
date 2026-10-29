// SPDX-License-Identifier: GPL-3.0-only
#include "chip_canvas.h"

#include <algorithm>
#include <cmath>

#include <wx/dcbuffer.h>
#include <wx/settings.h>

#include "gui_events.h"

using bscan::PackageLayout;
using bscan::PackagePin;
using bscan::PackageShape;
using bscan::PinValue;
using bscan::PlacedPin;

wxDEFINE_EVENT( EVT_CHIP_CANVAS_CLICK, wxCommandEvent );

// A pin counts as toggling while its value changed within this time
static constexpr std::chrono::milliseconds TOGGLE_HOLD{ 1000 };


static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


// --- colours (the legend below the drawing explains them); functions, as
//     wxColour objects must not be created before wxWidgets is initialised

namespace colour
{
static wxColour Drive1() { return wxColour( 217, 58, 58 ); }
static wxColour Drive0() { return wxColour( 47, 111, 220 ); }
static wxColour Input1() { return wxColour( 243, 166, 166 ); }
static wxColour Input0() { return wxColour( 169, 196, 242 ); }
static wxColour Unknown() { return wxColour( 255, 255, 255 ); }
static wxColour NoCells() { return wxColour( 200, 200, 200 ); }
static wxColour Jtag() { return wxColour( 154, 106, 208 ); }
static wxColour Linkage() { return wxColour( 112, 112, 112 ); }
static wxColour Body() { return wxColour( 228, 228, 228 ); }
static wxColour BodyExtest() { return wxColour( 246, 214, 214 ); }
static wxColour Outline() { return wxColour( 48, 48, 48 ); }
static wxColour Extest() { return wxColour( 190, 30, 30 ); }
} // namespace colour


ChipCanvas::ChipCanvas( wxWindow* aParent, const ChipDrawing& aDrawing ) :
        wxPanel( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE ),
        m_drawing( aDrawing )
{
    SetBackgroundStyle( wxBG_STYLE_PAINT );
    SetMinSize( FromDIP( wxSize( 360, 300 ) ) );

    Bind( wxEVT_PAINT, &ChipCanvas::onPaint, this );
    Bind( wxEVT_MOTION, &ChipCanvas::onMotion, this );
    Bind( wxEVT_LEFT_DOWN, [this]( wxMouseEvent& aEvent ) { click( aEvent, 0 ); } );
    Bind( wxEVT_RIGHT_DOWN, [this]( wxMouseEvent& aEvent ) { click( aEvent, 1 ); } );
}


// ============================================================================
// Geometry
// ============================================================================

wxPoint ChipCanvas::Frame::at( double aX, double aY ) const
{
    return wxPoint( static_cast<int>( std::lround( ox + ( aX - minX ) * scale ) ),
                    static_cast<int>( std::lround( oy + ( aY - minY ) * scale ) ) );
}


int ChipCanvas::legendHeight() const
{
    return 2 * GetCharHeight() + FromDIP( 8 );
}


ChipCanvas::Frame ChipCanvas::frame() const
{
    const PackageLayout& l   = m_drawing.layout;
    const bool           bga = l.type == PackageShape::Type::Bga;

    // Room around the body: pins (quad) and pin names, or the row and
    // column names (BGA)
    double margin = bga ? 1.2 : 2.4;
    double minX = -margin, minY = -margin;
    double maxX = l.width + ( bga ? 0.3 : margin ), maxY = l.height + ( bga ? 0.3 : margin );

    // Pins outside the body (e.g. many pins without a place) stay visible
    for( const PlacedPin& p : l.pins )
    {
        minX = std::min( minX, p.x - 1.0 );
        minY = std::min( minY, p.y - 1.0 );
        maxX = std::max( maxX, p.x + 1.0 );
        maxY = std::max( maxY, p.y + 1.0 );
    }

    wxSize size = GetClientSize();
    int    pad  = FromDIP( 8 );
    double w    = std::max( 1, size.x - 2 * pad );
    double h    = std::max( 1, size.y - 2 * pad - legendHeight() );

    Frame f;
    f.minX   = minX;
    f.minY   = minY;
    f.scale  = std::min( w / ( maxX - minX ), h / ( maxY - minY ) );
    f.ox     = pad + ( w - ( maxX - minX ) * f.scale ) / 2;
    f.oy     = pad + ( h - ( maxY - minY ) * f.scale ) / 2;
    f.labels = f.scale >= FromDIP( bga ? 26 : 14 );
    return f;
}


wxRect ChipCanvas::pinRect( const Frame& aFrame, const PlacedPin& aPin ) const
{
    double w = 0.8, h = 0.8;

    switch( aPin.side )
    {
    case PlacedPin::Side::Left:
    case PlacedPin::Side::Right:
        w = 1.0;
        h = 0.6;
        break;
    case PlacedPin::Side::Top:
    case PlacedPin::Side::Bottom:
        w = 0.6;
        h = 1.0;
        break;
    case PlacedPin::Side::Grid:
        w = h = 0.84;
        break;
    case PlacedPin::Side::Centre:
        break;
    }

    wxPoint a = aFrame.at( aPin.x - w / 2, aPin.y - h / 2 );
    wxPoint b = aFrame.at( aPin.x + w / 2, aPin.y + h / 2 );
    return wxRect( a, b );
}


std::optional<std::string> ChipCanvas::hit( const wxPoint& aPoint ) const
{
    Frame f = frame();

    for( const PlacedPin& p : m_drawing.layout.pins )
        if( pinRect( f, p ).Inflate( 1 ).Contains( aPoint ) )
            return p.pin;

    return std::nullopt;
}


// ============================================================================
// Colours
// ============================================================================

wxColour ChipCanvas::fill( const ChipPinState& aPin )
{
    switch( aPin.info.kind )
    {
    case PackagePin::Kind::Linkage: return colour::Linkage();
    case PackagePin::Kind::Jtag:    return colour::Jtag();
    case PackagePin::Kind::NoCells: return colour::NoCells();
    case PackagePin::Kind::BoundaryScan: break;
    }

    if( !aPin.sampled || !aPin.ok )
        return colour::Unknown();

    if( aPin.value.output == PinValue::Output::Drive1 )
        return colour::Drive1();

    if( aPin.value.output == PinValue::Output::Drive0 )
        return colour::Drive0();

    if( aPin.value.input )
        return *aPin.value.input ? colour::Input1() : colour::Input0();

    return colour::Unknown();
}


// The colours of a pin: one, or two while it toggles (both values were seen
// within TOGGLE_HOLD; drawn as a split pin)
ChipCanvas::Fill ChipCanvas::fills( const ChipPinState& aPin )
{
    bool toggling = aPin.info.kind == PackagePin::Kind::BoundaryScan && aPin.sampled && aPin.ok
                    && std::chrono::steady_clock::now() - aPin.changed < TOGGLE_HOLD;

    if( !toggling )
        return { fill( aPin ), std::nullopt };

    bool driving = aPin.value.output == PinValue::Output::Drive0
                   || aPin.value.output == PinValue::Output::Drive1;

    if( driving )
        return { colour::Drive1(), colour::Drive0() };

    return { colour::Input1(), colour::Input0() };
}


// A pin (rectangle or ball) in one colour or split in two
void ChipCanvas::drawPin( wxDC& aDC, const wxRect& aRect, bool aBall, const Fill& aFill,
                          const wxPen& aOutline )
{
    aDC.SetPen( *wxTRANSPARENT_PEN );

    if( !aFill.second )
    {
        aDC.SetBrush( wxBrush( aFill.first ) );

        if( aBall )
            aDC.DrawEllipse( aRect );
        else
            aDC.DrawRectangle( aRect );
    }
    else if( aBall )
    {
        aDC.SetBrush( wxBrush( aFill.first ) );
        aDC.DrawEllipticArc( aRect.x, aRect.y, aRect.width, aRect.height, 90, 270 );
        aDC.SetBrush( wxBrush( *aFill.second ) );
        aDC.DrawEllipticArc( aRect.x, aRect.y, aRect.width, aRect.height, 270, 450 );
    }
    else
    {
        int half = aRect.width / 2;
        aDC.SetBrush( wxBrush( aFill.first ) );
        aDC.DrawRectangle( aRect.x, aRect.y, half, aRect.height );
        aDC.SetBrush( wxBrush( *aFill.second ) );
        aDC.DrawRectangle( aRect.x + half, aRect.y, aRect.width - half, aRect.height );
    }

    aDC.SetPen( aOutline );
    aDC.SetBrush( *wxTRANSPARENT_BRUSH );

    if( aBall )
        aDC.DrawEllipse( aRect );
    else
        aDC.DrawRectangle( aRect );
}


// ============================================================================
// Painting
// ============================================================================

void ChipCanvas::onPaint( wxPaintEvent& )
{
    wxAutoBufferedPaintDC dc( this );
    dc.SetBackground( wxBrush( wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOW ) ) );
    dc.Clear();
    dc.SetTextForeground( wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT ) );

    if( !m_drawing.message.empty() || m_drawing.layout.pins.empty() )
    {
        wxString text = m_drawing.message.empty() ? _( "No pins to show." )
                                                  : fromUtf8( m_drawing.message );
        dc.DrawLabel( text, wxRect( GetClientSize() ), wxALIGN_CENTER );
        return;
    }

    const Frame f = frame();
    drawBody( dc, f );
    drawGridNames( dc, f );
    drawPins( dc, f );

    dc.SetTextForeground( wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT ) );
    drawLegend( dc );
}


// Body with pin 1 mark and the chip's name (quad, dual)
void ChipCanvas::drawBody( wxDC& aDC, const Frame& aFrame )
{
    const PackageLayout& l      = m_drawing.layout;
    const bool           extest = m_drawing.extest;

    wxRect body( aFrame.at( 0, 0 ), aFrame.at( l.width, l.height ) );
    aDC.SetPen( wxPen( extest ? colour::Extest() : colour::Outline(), extest ? FromDIP( 3 ) : 1 ) );
    aDC.SetBrush( wxBrush( extest ? colour::BodyExtest() : colour::Body() ) );
    aDC.DrawRectangle( body );

    if( l.type == PackageShape::Type::Bga )
        return;

    aDC.SetBrush( wxBrush( colour::Outline() ) );
    aDC.DrawCircle( aFrame.at( 0.6, 0.6 ), std::max( 2, static_cast<int>( aFrame.scale * 0.2 ) ) );

    wxString name = fromUtf8( m_drawing.name );

    if( extest )
        name << "\n" << _( "EXTEST" );

    aDC.DrawLabel( name, body, wxALIGN_CENTER );
}


// Row and column names of a BGA
void ChipCanvas::drawGridNames( wxDC& aDC, const Frame& aFrame )
{
    const PackageLayout& l = m_drawing.layout;

    if( l.type != PackageShape::Type::Bga || aFrame.scale < FromDIP( 10 ) )
        return;

    wxFont small = GetFont();
    small.SetPointSize( std::max( 6, small.GetPointSize() - 1 ) );
    aDC.SetFont( small );

    for( size_t r = 0; r < l.rowNames.size(); ++r )
        aDC.DrawLabel( fromUtf8( l.rowNames[r] ),
                       wxRect( aFrame.at( -1.2, r ), aFrame.at( -0.1, r + 1.0 ) ), wxALIGN_CENTER );

    for( int c = 0; c < l.columns; ++c )
        aDC.DrawLabel( wxString::Format( "%d", c + 1 ),
                       wxRect( aFrame.at( c, -1.2 ), aFrame.at( c + 1.0, -0.1 ) ), wxALIGN_CENTER );
}


void ChipCanvas::drawPins( wxDC& aDC, const Frame& aFrame )
{
    wxFont pinFont = GetFont();
    pinFont.SetPixelSize( wxSize( 0, std::max( 6, static_cast<int>( aFrame.scale * 0.42 ) ) ) );
    aDC.SetFont( pinFont );

    for( const PlacedPin& p : m_drawing.layout.pins )
    {
        auto it = m_drawing.pins.find( p.pin );

        if( it == m_drawing.pins.end() )
            continue;

        wxRect r        = pinRect( aFrame, p );
        bool   selected = m_drawing.selected.contains( p.pin );

        drawPin( aDC, r, p.side == PlacedPin::Side::Grid, fills( it->second ),
                 selected ? wxPen( wxSystemSettings::GetColour( wxSYS_COLOUR_HIGHLIGHT ),
                                   FromDIP( 3 ) )
                          : wxPen( colour::Outline(), 1 ) );

        if( !aFrame.labels )
            continue;

        // Pin names: in BGA balls, outside the pins of a quad
        wxString name = fromUtf8( p.pin );
        wxRect   label;
        int      align = wxALIGN_CENTER;
        int      gap   = FromDIP( 3 );

        switch( p.side )
        {
        case PlacedPin::Side::Left:
            label = wxRect( r.GetLeft() - FromDIP( 60 ) - gap, r.y, FromDIP( 60 ), r.height );
            align = wxALIGN_RIGHT | wxALIGN_CENTER_VERTICAL;
            break;
        case PlacedPin::Side::Right:
            label = wxRect( r.GetRight() + gap, r.y, FromDIP( 60 ), r.height );
            align = wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL;
            break;
        case PlacedPin::Side::Top:
            label = wxRect( r.x - FromDIP( 20 ), r.GetTop() - GetCharHeight() - gap,
                            r.width + FromDIP( 40 ), GetCharHeight() );
            break;
        case PlacedPin::Side::Bottom:
            label = wxRect( r.x - FromDIP( 20 ), r.GetBottom() + gap, r.width + FromDIP( 40 ),
                            GetCharHeight() );
            break;
        case PlacedPin::Side::Grid:
        case PlacedPin::Side::Centre:
            label = r;
            break;
        }

        // Inside a pin: dark or light text, whichever is readable
        if( label == r )
        {
            wxColour c = fills( it->second ).first;
            int      y = ( 299 * c.Red() + 587 * c.Green() + 114 * c.Blue() ) / 1000;
            aDC.SetTextForeground( y < 128 ? *wxWHITE : *wxBLACK );
        }
        else
        {
            aDC.SetTextForeground( wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT ) );
        }

        aDC.DrawLabel( name, label, align );
    }
}


void ChipCanvas::drawLegend( wxDC& aDC )
{
    aDC.SetFont( GetFont() );

    const std::pair<Fill, wxString> items[] = {
        { { colour::Drive1(), {} }, _( "drives 1" ) },
        { { colour::Drive0(), {} }, _( "drives 0" ) },
        { { colour::Input1(), {} }, _( "reads 1" ) },
        { { colour::Input0(), {} }, _( "reads 0" ) },
        { { colour::Drive1(), colour::Drive0() }, _( "toggling" ) },
        { { colour::Unknown(), {} }, _( "not sampled" ) },
        { { colour::NoCells(), {} }, _( "no cells" ) },
        { { colour::Jtag(), {} }, _( "JTAG" ) },
        { { colour::Linkage(), {} }, _( "power / NC" ) },
    };

    wxSize size = GetClientSize();
    int    box  = GetCharHeight() - FromDIP( 2 );
    int    x    = FromDIP( 8 );
    int    y    = size.y - legendHeight() + FromDIP( 4 );

    for( const auto& [c, text] : items )
    {
        int w = box + FromDIP( 4 ) + aDC.GetTextExtent( text ).x + FromDIP( 12 );

        if( x + w > size.x && x > FromDIP( 8 ) )
        {
            x = FromDIP( 8 );
            y += GetCharHeight();
        }

        drawPin( aDC, wxRect( x, y + 1, box, box ), false, c, wxPen( colour::Outline(), 1 ) );
        aDC.DrawText( text, x + box + FromDIP( 4 ), y );
        x += w;
    }
}


// ============================================================================
// Mouse
// ============================================================================

void ChipCanvas::onMotion( wxMouseEvent& aEvent )
{
    auto pin = hit( aEvent.GetPosition() );

    if( pin.value_or( "" ) == m_hover )
        return;

    m_hover = pin.value_or( "" );
    auto it = pin ? m_drawing.pins.find( *pin ) : m_drawing.pins.end();

    if( it == m_drawing.pins.end() )
    {
        UnsetToolTip();
        return;
    }

    const ChipPinState& s = it->second;
    wxString            tip;
    tip << _( "Pin " ) << fromUtf8( *pin ) << ": " << fromUtf8( s.info.cells.port );

    if( !s.net.empty() )
        tip << "\n" << _( "Net: " ) << fromUtf8( s.net );

    switch( s.info.kind )
    {
    case PackagePin::Kind::Linkage: tip << "\n" << _( "power / ground / not connected" ); break;
    case PackagePin::Kind::Jtag:    tip << "\n" << _( "JTAG" ); break;
    case PackagePin::Kind::NoCells: tip << "\n" << _( "no boundary-scan cells" ); break;
    case PackagePin::Kind::BoundaryScan:
        if( s.sampled && !s.ok )
            tip << "\n" << fromUtf8( s.error );
        break;
    }

    SetToolTip( tip );
}


void ChipCanvas::click( wxMouseEvent& aEvent, int aButton )
{
    auto pin = hit( aEvent.GetPosition() );

    if( !pin )
        return;

    int flags = ( aButton == 1 ? CLICK_RIGHT : 0 ) | ( aEvent.CmdDown() ? CLICK_ADD : 0 );
    SendGuiEvent( this, EVT_CHIP_CANVAS_CLICK, flags, fromUtf8( *pin ) );
}
