// SPDX-License-Identifier: GPL-3.0-only
// The red bar at the top of the window while a device is in EXTEST (by this
// window, being entered, or held from an earlier session): what is driven,
// interruptions and notes, "Leave EXTEST" and "Take over...". Hidden
// otherwise.

#pragma once


#include <wx/panel.h>

class wxButton;
class wxStaticText;
class ExtestController;
class Session;

class ExtestBanner : public wxPanel
{
public:
    ExtestBanner( wxWindow* aParent, Session& aSession, ExtestController& aController );

    // Text, buttons and visibility from the session's EXTEST state
    void UpdateState();

    // Sends EVT_LAYOUT_CHANGED (gui_events.h) when its size or visibility changes

private:
    void wrapText( bool aForce );   // to the width next to the buttons
    void layoutChanged();

    Session&          m_session;
    ExtestController& m_controller;

    wxStaticText* m_text     = nullptr;
    wxButton*     m_leave    = nullptr;
    wxButton*     m_takeOver = nullptr;   // shown while held devices can be taken over
    wxString      m_fullText;             // unwrapped
    int           m_wrapWidth = -1;       // width the text was wrapped to
};
