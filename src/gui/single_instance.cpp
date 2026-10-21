// SPDX-License-Identifier: GPL-3.0-only
#include "single_instance.h"

#include <wx/app.h>
#include <wx/filename.h>
#include <wx/ipc.h>
#include <wx/log.h>
#include <wx/stdpaths.h>
#include <wx/utils.h>

static const char* TOPIC = "raise";


class RaiseConnection : public wxConnection
{
public:
    explicit RaiseConnection( std::function<void()> aOnRaise ) :
            m_onRaise( std::move( aOnRaise ) )
    {
    }

    bool OnExecute( const wxString&, const void*, size_t, wxIPCFormat ) override
    {
        // Called from the event loop; defer anyway so the connection can finish
        if( m_onRaise )
            wxTheApp->CallAfter( m_onRaise );

        return true;
    }

private:
    std::function<void()> m_onRaise;
};


class RaiseServer : public wxServer
{
public:
    explicit RaiseServer( std::function<void()> aOnRaise ) :
            m_onRaise( std::move( aOnRaise ) )
    {
    }

    wxConnectionBase* OnAcceptConnection( const wxString& aTopic ) override
    {
        if( aTopic != TOPIC )
            return nullptr;

        return new RaiseConnection( m_onRaise );
    }

private:
    std::function<void()> m_onRaise;
};


SingleInstance::SingleInstance()
{
    wxString name = wxString::Format( "kicad-bscan-%s", wxGetUserId() );

#ifdef __WINDOWS__
    m_service = name;   // DDE service name
#else
    // Unix domain socket in the temp dir (wxTCPServer uses a Unix socket for
    // non-numeric service names)
    m_service = wxFileName( wxStandardPaths::Get().GetTempDir(), name + ".ipc" ).GetFullPath();
#endif

    m_checker.Create( name + ".lock", wxStandardPaths::Get().GetTempDir() );
}


SingleInstance::~SingleInstance() = default;


bool SingleInstance::IsFirst()
{
    if( !m_checker.IsAnotherRunning() )
        return true;

    wxLogNull         noLog;   // the other instance may be just starting up / gone
    wxClient          client;
    wxConnectionBase* conn = client.MakeConnection( "localhost", m_service, TOPIC );

    if( conn )
    {
        conn->Execute( TOPIC );
        conn->Disconnect();
        delete conn;
    }

    return false;
}


void SingleInstance::Listen( std::function<void()> aOnRaise )
{
    m_server = std::make_unique<RaiseServer>( std::move( aOnRaise ) );

#ifndef __WINDOWS__
    // We hold the single-instance lock, so a socket file left over from a
    // crashed instance can be removed safely.
    if( wxFileExists( m_service ) )
        wxRemoveFile( m_service );
#endif

    if( !m_server->Create( m_service ) )
    {
        wxLogWarning( "kicad-bscan: could not create IPC server %s; a second start will not "
                      "raise this window",
                      m_service );
        m_server.reset();
    }
}
