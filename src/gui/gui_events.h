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

// Sends a wxCommandEvent of aType from aSource with an int and a string
// (both optional) and processes it before returning.
void SendGuiEvent( wxWindow* aSource, wxEventType aType, int aInt = 0,
                   const wxString& aText = wxEmptyString );
