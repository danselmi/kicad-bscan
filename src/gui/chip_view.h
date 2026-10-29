// SPDX-License-Identifier: GPL-3.0-only
// The chip view: a separate window that shows one chip of the JTAG chain
// from its BSDL file (package layout guessed from the pin names, see
// bscan/package_layout.h, or set per chip) with the live state of every
// boundary-scan pin (drawn by ChipCanvas). For boards not made in KiCad,
// and for large chips.
//
// A click on a pin makes it the selected pad of the Pins tab (and selects
// the pad in KiCad, if the board has it), Ctrl+click selects several;
// right-click: watch list, drive (all selected pins).
// Talks to the main window with events (gui_events.h): EVT_CHIP_PIN_SELECTED,
// EVT_CHIP_PINS_CHANGED (the pins to sample changed: Session::chipView*),
// EVT_WATCH_LIST_CHANGED.

#pragma once

#include <string>
#include <utility>
#include <vector>

#include <wx/frame.h>

#include "bscan/live_sampler.h"

#include "chip_canvas.h"   // ChipDrawing

class wxChoice;
class wxPanel;
class wxStaticText;
class wxTextCtrl;
class ExtestController;
class Session;

class ChipView : public wxFrame
{
public:
    ChipView( wxWindow* aParent, Session& aSession, ExtestController& aExtest );

    // Chain, settings, board or EXTEST changed: chooser, warning, drawing
    void UpdateState();

    // Sampler results ("chip:" keys) for the shown chip
    void ShowSample( const bscan::LiveSampler::SampleUpdate& aUpdate );

    // Pins (reference, pin) selected elsewhere: those on the shown chip are
    // highlighted (none: no highlight). The chip shown stays.
    void HighlightPins( const std::vector<std::pair<std::string, std::string>>& aPins );

    // The pins selected here (reference, pin)
    std::vector<std::pair<std::string, std::string>> SelectedPins() const;

private:
    void fillChooser();
    void showChip();          // after the chooser or the layout changed
    void updateLayoutControls();
    void updateWarning();
    void publishPins();       // Session::chipView* + EVT_CHIP_PINS_CHANGED
    void onLayoutChanged();
    void onClose( wxCloseEvent& aEvent );

    // Clicks on the canvas (EVT_CHIP_CANVAS_CLICK). aAdd (Ctrl): add the pin
    // to the selection or take it out, like in KiCad and the watch list
    void selectPin( const std::string& aPin, bool aAdd );
    void pinMenu();   // for the selected pins

    std::string currentReference() const;

    Session&          m_session;
    ExtestController& m_extest;

    std::vector<std::string> m_taps;      // chooser entries
    std::string              m_tap;       // shown chip ("" none)
    ChipDrawing              m_drawing;   // what the canvas shows

    wxPanel*      m_panel       = nullptr;
    wxChoice*     m_chooser     = nullptr;
    wxChoice*     m_shape       = nullptr;
    wxTextCtrl*   m_sides       = nullptr;
    wxStaticText* m_shapeNote   = nullptr;
    wxStaticText* m_warning     = nullptr;
    ChipCanvas*   m_canvas      = nullptr;
};
