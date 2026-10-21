// SPDX-License-Identifier: GPL-3.0-only
// The EXTEST actions of the window, wherever they are started (chain table,
// drive buttons, watch list, interconnect tab, EXTEST bar): setup and review
// dialogs, entering, taking over, driving pins, leaving, and the updates the
// sampler reports. The state itself is in the Session.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "session.h"

class wxWindow;

class ExtestController
{
public:
    ExtestController( wxWindow* aParent, Session& aSession );

    // --- callbacks to the window ---
    std::function<void()>                  OnChanged;              // EXTEST state changed
    std::function<void()>                  OnStartStatesChanged;   // saved in the setup dialog
    std::function<void( const wxString& )> OnStatus;               // short progress text

    // Setup of aTap: opens the setup dialog (after reading the current state),
    // or takes over a held TAP. aPreset: starting states for clicked pins
    // (saved only if confirmed); aFocus: pins to show selected.
    void Setup( const std::string& aTap, const StartStates& aPreset = {},
                const std::vector<std::string>& aFocus = {} );

    // Takes over aTap (with 'jtag scan' together with all other held TAPs)
    void TakeOver( const std::string& aTap );
    void TakeOverAnyHeld();   // the first held TAP that can be taken over
    void LeaveAll();

    // Drives pins of devices in EXTEST; if none is in EXTEST yet, starts it
    // for the device of the pins (setup and review first)
    void DrivePins( const std::vector<bscan::WatchEntry>& aPins, bscan::PinDrive aDrive );

    // A sampler update (in the GUI thread)
    void OnUpdate( const bscan::LiveSampler::ExtestUpdate& aUpdate );

    // Saved starting states of aTap that the BSDL file / pin map does not
    // know: tell the user (after the file or the pin map changed)
    void WarnStaleSettings( const std::string& aTap );

    // The window is closing: ask what happens with active EXTEST. False:
    // closing cancelled (only if aCanVeto).
    bool ConfirmClose( bool aCanVeto );

private:
    void openSetup( const std::string& aTap, const std::vector<bscan::Bits>& aSamples,
                    bool aActive );
    void confirm( const std::string& aTap, const std::vector<bscan::Bits>& aSamples,
                  bool aBackgroundPolling, const std::string& aError );
    // Saved image or new plan (setup + review) for one TAP; nullopt: cancelled
    std::optional<bscan::Bits> chooseTakeOverImage( const std::string& aTap );
    void startWith( const std::vector<bscan::WatchEntry>& aPins, bscan::PinDrive aDrive );
    bool confirmDrive( size_t aApplied, bscan::PinDrive aDrive,
                       const std::vector<wxString>& aAffected,
                       const std::vector<wxString>& aSkipped );
    void changed();

    wxWindow* m_parent;
    Session&  m_session;

    StartStates              m_preset;   // for the next setup dialog
    std::vector<std::string> m_focus;
};
