// SPDX-License-Identifier: GPL-3.0-only
// The drawing of the chip view: a package (bscan::PackageLayout) with every
// pin coloured by its state, pin and row/column names when there is room,
// a legend and tooltips. A pure drawing widget: it shows a ChipDrawing that
// its owner keeps up to date (then Refresh()), and reports clicks on pins
// with EVT_CHIP_CANVAS_CLICK.

#pragma once

#include <chrono>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

#include <wx/colour.h>
#include <wx/panel.h>

#include "bscan/bsdl_device.h"
#include "bscan/chain.h"
#include "bscan/package_layout.h"

class wxDC;
class wxPen;

// What is known about one pin
struct ChipPinState
{
    bscan::PackagePin                     info;
    std::string                           net;
    bool                                  sampled = false;   // a result arrived
    bool                                  ok      = false;
    bscan::PinValue                       value;
    std::string                           error;
    std::chrono::steady_clock::time_point changed;            // last change of the value
};

// Everything the canvas shows
struct ChipDrawing
{
    bscan::PackageLayout                layout;
    std::map<std::string, ChipPinState> pins;      // by physical pin
    std::set<std::string>               selected;  // pins drawn with a highlight
    std::string                         name;      // in the body (reference)
    bool                                extest = false;   // the chip is in EXTEST
    std::string                         message;   // shown instead of a drawing
};

// A click on a pin: GetString() is the pin; GetInt(): CLICK_RIGHT for the
// right mouse button, CLICK_ADD with Ctrl (Cmd on macOS) held
enum ChipClick
{
    CLICK_RIGHT = 1,
    CLICK_ADD   = 2
};
wxDECLARE_EVENT( EVT_CHIP_CANVAS_CLICK, wxCommandEvent );

class ChipCanvas : public wxPanel
{
public:
    // aDrawing is owned by the caller and must outlive the canvas
    ChipCanvas( wxWindow* aParent, const ChipDrawing& aDrawing );

private:
    using Fill = std::pair<wxColour, std::optional<wxColour>>;   // second: toggling

    // Screen geometry of the current layout
    struct Frame
    {
        double scale = 0, ox = 0, oy = 0, minX = 0, minY = 0;
        bool   labels = false;   // pin names next to / in the pins

        wxPoint at( double aX, double aY ) const;
    };

    Frame                      frame() const;
    int                        legendHeight() const;
    wxRect                     pinRect( const Frame& aFrame, const bscan::PlacedPin& aPin ) const;
    std::optional<std::string> hit( const wxPoint& aPoint ) const;

    static wxColour fill( const ChipPinState& aPin );
    static Fill     fills( const ChipPinState& aPin );
    static void     drawPin( wxDC& aDC, const wxRect& aRect, bool aBall, const Fill& aFill,
                             const wxPen& aOutline );

    void onPaint( wxPaintEvent& aEvent );
    void drawBody( wxDC& aDC, const Frame& aFrame );
    void drawGridNames( wxDC& aDC, const Frame& aFrame );
    void drawPins( wxDC& aDC, const Frame& aFrame );
    void drawLegend( wxDC& aDC );
    void onMotion( wxMouseEvent& aEvent );
    void click( wxMouseEvent& aEvent, int aButton );

    const ChipDrawing& m_drawing;
    std::string        m_hover;   // pin under the mouse (tooltip)
};
