// SPDX-License-Identifier: GPL-3.0-only
#include "extest_banner.h"

#include <algorithm>

#include <wx/button.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "extest_controller.h"
#include "gui_events.h"
#include "session.h"

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


ExtestBanner::ExtestBanner( wxWindow* aParent, Session& aSession, ExtestController& aController ) :
        wxPanel( aParent ),
        m_session( aSession ),
        m_controller( aController )
{
    SetBackgroundColour( wxColour( 190, 30, 30 ) );

    m_text = new wxStaticText( this, wxID_ANY, wxEmptyString );
    m_text->SetForegroundColour( *wxWHITE );
    m_text->SetFont( m_text->GetFont().Bold() );

    m_leave    = new wxButton( this, wxID_ANY, _( "Leave EXTEST" ) );
    m_takeOver = new wxButton( this, wxID_ANY, _( "Take over..." ) );
    m_takeOver->SetToolTip( _( "Continue driving the devices kept in EXTEST (all of them), "
                               "without returning their pins to the devices' own logic in "
                               "between" ) );
    m_takeOver->Hide();

    auto* buttons = new wxBoxSizer( wxVERTICAL );
    buttons->Add( m_takeOver, 0, wxEXPAND | wxBOTTOM, FromDIP( 4 ) );
    buttons->Add( m_leave, 0, wxEXPAND );

    auto* sizer = new wxBoxSizer( wxHORIZONTAL );
    sizer->Add( m_text, 1, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP( 6 ) );
    sizer->Add( buttons, 0, wxALIGN_CENTER_VERTICAL | wxALL, FromDIP( 6 ) );
    SetSizer( sizer );
    Hide();

    m_leave->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { m_controller.LeaveAll(); } );
    m_takeOver->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { m_controller.TakeOverAnyHeld(); } );

    // Re-wrap the text to the new width (after the layout has settled)
    Bind( wxEVT_SIZE,
          [this]( wxSizeEvent& aEvent )
          {
              aEvent.Skip();
              CallAfter( [this] { wrapText( false ); } );
          } );
}


void ExtestBanner::layoutChanged()
{
    SendGuiEvent( this, EVT_LAYOUT_CHANGED );
}


void ExtestBanner::UpdateState()
{
    const std::vector<std::string> held = m_session.HeldTaps();
    bool show = !m_session.extest.empty() || !m_session.extestPending.empty() || !held.empty();

    if( show )
    {
        auto label = [this]( const std::string& aTap ) { return m_session.DeviceLabel( aTap ); };

        wxString text;

        if( !m_session.extest.empty() )
        {
            wxString devices;

            for( const auto& [tap, e] : m_session.extest )
                devices << ( devices.empty() ? "" : ", " )
                        << ( e.reference.empty() ? fromUtf8( tap ) : fromUtf8( e.reference ) );

            text = wxString::Format( _( "EXTEST active on %s: the device's own logic is "
                                        "disconnected from its pins" ),
                                     devices );
        }
        else if( !m_session.extestPending.empty() )
        {
            text = _( "Entering EXTEST..." );
        }

        for( const std::string& tap : held )
        {
            auto hw = m_session.HardwarePinControl( tap );

            text << ( text.empty() ? "" : "\n" )
                 << ( hw ? wxString::Format( _( "%s is in %s (set before this window was opened, "
                                                "or by another program). Not sampled until it "
                                                "is left%s." ),
                                             label( tap ), fromUtf8( *hw ),
                                             m_session.CanTakeOver( tap ) ? _( " or taken over" )
                                                                          : wxString() )
                         : wxString::Format( _( "%s was probably left in EXTEST when this window "
                                                "was closed. Not sampled until EXTEST is left." ),
                                             label( tap ) ) );
        }

        for( const auto& [tap, line] : m_session.extestInterrupted )
            text << "\n" << line;

        if( !m_session.extestNote.empty() )
            text << "\n" << m_session.extestNote;

        // "Take over..." while (at least) one held device can be taken over
        bool takeOver = std::ranges::any_of( held, [this]( const std::string& t )
                                             { return m_session.CanTakeOver( t ); } );

        if( m_takeOver->IsShown() != takeOver )
        {
            m_takeOver->Show( takeOver );
            m_wrapWidth = -1;   // the button column may be wider now
        }

        m_fullText = text;
        m_text->SetToolTip(
                m_session.extestInterrupted.empty()
                        ? wxString()
                        : _( "Each interruption put the device into another instruction for a "
                             "moment (e.g. BYPASS): its pins were back with the device's own logic "
                             "until the next scan restored EXTEST and your pin states." ) );
        wrapText( true );
    }

    if( IsShown() != show )
    {
        Show( show );
        layoutChanged();
    }
    else if( show )
    {
        Layout();
        layoutChanged();
    }
}


void ExtestBanner::wrapText( bool aForce )
{
    // Space next to the buttons (borders of 6 DIP around text and buttons)
    int buttons = std::max( m_leave->GetBestSize().x,
                            m_takeOver->IsShown() ? m_takeOver->GetBestSize().x : 0 );
    int width   = GetClientSize().x - buttons - FromDIP( 4 * 6 );

    if( width < FromDIP( 150 ) )
        width = FromDIP( 420 );   // not laid out yet; corrected by the size event

    if( !aForce && width == m_wrapWidth )
        return;   // also ends the size event caused by the layout below

    m_wrapWidth = width;
    m_text->SetLabel( m_fullText );
    m_text->Wrap( width );

    // The wrapped text must not keep the window from getting narrower
    m_text->SetMinSize( wxSize( FromDIP( 150 ), m_text->GetBestSize().y ) );

    // The bar keeps its old height otherwise (cached best size)
    InvalidateBestSize();

    if( IsShown() )
        layoutChanged();
}
