// SPDX-License-Identifier: GPL-3.0-only
#include "gui_events.h"

#include <wx/window.h>

wxDEFINE_EVENT( EVT_LAYOUT_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_ASSIGNMENTS_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHIP_PIN_SELECTED, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHIP_PINS_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_WATCH_LIST_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_WATCH_SELECTION, wxCommandEvent );


void SendGuiEvent( wxWindow* aSource, wxEventType aType, int aInt, const wxString& aText )
{
    wxCommandEvent event( aType, aSource->GetId() );
    event.SetEventObject( aSource );
    event.SetInt( aInt );
    event.SetString( aText );
    aSource->ProcessWindowEvent( event );
}
