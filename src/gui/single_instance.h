// SPDX-License-Identifier: GPL-3.0-only
// Ensures only one plugin window per user.
//
// KiCad starts a new process every time the toolbar button is clicked. The
// second process asks the first one to bring its window to the front and exits.

#pragma once

#include <functional>
#include <memory>

// wx/defs.h must come first: wx/snglinst.h tests wxUSE_SNGLINST_CHECKER
// without including it. wxServer is a macro (wxTCPServer / wxDDEServer), so it
// cannot be forward-declared; include wx/ipc.h instead.
#include <wx/defs.h>
#include <wx/ipc.h>
#include <wx/snglinst.h>
#include <wx/string.h>

class SingleInstance
{
public:
    SingleInstance();
    ~SingleInstance();

    // True if this is the first instance. Otherwise the running instance has
    // been asked to raise its window, and this process should exit.
    bool IsFirst();

    // First instance only: aOnRaise is called (in the GUI thread) whenever a
    // later instance is started.
    void Listen( std::function<void()> aOnRaise );

private:
    wxString                  m_service;
    wxSingleInstanceChecker   m_checker;
    std::unique_ptr<wxServer> m_server;
};
