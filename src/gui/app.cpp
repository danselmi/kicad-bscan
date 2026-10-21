// SPDX-License-Identifier: GPL-3.0-only
// Entry point of the KiCad plugin process.
//
// KiCad (>= 10.0.6) starts this executable when the toolbar button is pressed
// and passes KICAD_API_SOCKET / KICAD_API_TOKEN in the environment. The process
// runs independently of KiCad; its window is therefore never modal to KiCad.

#include <csignal>

#include <wx/app.h>
#include <wx/bitmap.h>
#include <wx/config.h>
#include <wx/iconbndl.h>
#include <wx/image.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/weakref.h>

#include "app_icons.h"   // generated from icons/*.png
#include "main_frame.h"
#include "single_instance.h"


// Window/taskbar icon in all sizes, from the PNGs embedded at build time
static wxIconBundle appIcons()
{
    if( !wxImage::FindHandler( wxBITMAP_TYPE_PNG ) )
        wxImage::AddHandler( new wxPNGHandler );

    wxIconBundle bundle;

    for( const AppIcon& icon : s_appIcons )
    {
        wxBitmap bmp = wxBitmap::NewFromPNGData( icon.data, icon.size );

        if( bmp.IsOk() )
        {
            wxIcon ico;
            ico.CopyFromBitmap( bmp );
            bundle.AddIcon( ico );
        }
    }

    return bundle;
}

class BscanApp : public wxApp
{
public:
    bool OnInit() override
    {
#ifndef __WINDOWS__
        // KiCad reads our stdout/stderr through a pipe. If KiCad exits first,
        // writing would raise SIGPIPE and kill this process.
        std::signal( SIGPIPE, SIG_IGN );

        // Settings in ~/.config/kicad-bscan instead of ~/.kicad-bscan
        wxStandardPaths::Get().SetFileLayout( wxStandardPaths::FileLayout_XDG );
#endif
        SetAppName( "kicad-bscan" );
        SetAppDisplayName( "Boundary Scan" );

#if wxCHECK_VERSION( 3, 3, 0 )
        // Follow the system's light/dark setting. GTK and macOS do that
        // anyway; Windows stays light unless asked, and only before the
        // first window is created. Not available with wxWidgets 3.2.
        SetAppearance( Appearance::System );
#endif

        // Log to stderr instead of message boxes. KiCad shows a plugin's
        // stderr in its status bar messages. Keep output small: KiCad only
        // drains the pipe when the process exits.
        delete wxLog::SetActiveTarget( new wxLogStderr() );

        m_instance = std::make_unique<SingleInstance>();

        if( !m_instance->IsFirst() )
            return false;   // the running instance raises its window

        auto* frame = new MainFrame(
                kicad::Client::ConfigFromEnvironment(
                        wxString::Format( "kicad-bscan-%lu", wxGetProcessId() ).ToStdString() ) );

        // Weak reference: a late "raise" request may arrive while the frame
        // is already being destroyed
        wxWeakRef<MainFrame> weakFrame( frame );
        m_instance->Listen(
                [weakFrame]
                {
                    if( weakFrame )
                        weakFrame->BringToFront();
                } );

        frame->SetIcons( appIcons() );
        frame->Show();
        return true;
    }

    int OnExit() override
    {
        m_instance.reset();
        delete wxConfigBase::Set( nullptr );   // flush settings
        return wxApp::OnExit();
    }

private:
    std::unique_ptr<SingleInstance> m_instance;
};

wxIMPLEMENT_APP( BscanApp );
