// SPDX-License-Identifier: GPL-3.0-only
// wxWidgets events between the parts of the window. All are wxCommandEvents,
// sent with SendGuiEvent() (processed at once, like a function call); unless
// a handler of the source handles them they travel up to the parent windows.

#pragma once

#include <wx/event.h>
#include <wx/string.h>

class wxWindow;

// A part changed its size or visibility (EXTEST bar, a page): the window
// lays out again. Handled by MainFrame for all its parts.
wxDECLARE_EVENT( EVT_LAYOUT_CHANGED, wxCommandEvent );

// ChainPage: reference, BSDL file or pin map of a TAP changed, or a TAP was
// forgotten. GetInt() != 0: devices were read again, sample them anew.
wxDECLARE_EVENT( EVT_ASSIGNMENTS_CHANGED, wxCommandEvent );

// Chip view (a separate window; MainFrame binds them on it):
// a pin was clicked (Session::pickedPin is set),
wxDECLARE_EVENT( EVT_CHIP_PIN_SELECTED, wxCommandEvent );
// the pins to sample changed (Session::chipViewReference / chipViewPins),
wxDECLARE_EVENT( EVT_CHIP_PINS_CHANGED, wxCommandEvent );
// the watch list in the settings changed.
wxDECLARE_EVENT( EVT_WATCH_LIST_CHANGED, wxCommandEvent );

// PinsPage: rows of the watch list were selected by the user (Session::pickedPin
// is set, KiCad was asked to select them); travels up to MainFrame.
wxDECLARE_EVENT( EVT_WATCH_SELECTION, wxCommandEvent );

// Sends a wxCommandEvent of aType from aSource with an int and a string
// (both optional) and processes it before returning.
void SendGuiEvent( wxWindow* aSource, wxEventType aType, int aInt = 0,
                   const wxString& aText = wxEmptyString );
