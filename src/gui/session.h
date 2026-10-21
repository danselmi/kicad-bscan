// SPDX-License-Identifier: GPL-3.0-only
// What the parts of the window share (GUI thread only): the board from
// KiCad, the project settings, OpenOCD's chain, the BSDL files read and the
// EXTEST state of this window, plus the questions several parts ask about
// them (is a TAP held in EXTEST? which target samples this pad? which
// devices take part in the interconnect test?).
//
// Actions with dialogs live in the parts (ExtestController, the pages).

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <wx/string.h>

#include "bscan/extest.h"
#include "bscan/interconnect.h"
#include "bscan/live_sampler.h"
#include "bscan/project_config.h"
#include "kicad/board_watcher.h"

#include "extest_dialogs.h"   // PinNets, StartStates, TestRolesFunction

class wxWindow;

// Starting states <-> their form in the project file
StartStates ToStartStates( const std::map<std::string, std::string>& aConfig );
std::map<std::string, std::string> FromStartStates( const StartStates& aStates );

class Session
{
public:
    // A TAP in EXTEST by this window (or being entered / taken over)
    struct ActiveExtest
    {
        bscan::TapInfo                           tap;
        std::shared_ptr<const bscan::BsdlDevice> device;
        std::string                              pinMap;
        bscan::Bits                              image;   // what is being driven
        std::string                              reference;
    };

    Session( wxWindow* aParent, bscan::LiveSampler& aSampler );

    // --- shared data ---
    kicad::BoardState               board;
    std::string                     projectFile;   // empty: not known (not saved)
    bscan::ProjectConfig            config;
    bscan::LiveSampler::ChainUpdate chain;
    bscan::LiveSampler&             sampler;
    kicad::BoardWatcher*            watcher = nullptr;

    std::map<std::string, ActiveExtest> extest;              // TAP name -> active EXTEST
    std::map<std::string, ActiveExtest> extestPending;       // requested, not yet confirmed
    wxString                            extestNote;          // e.g. "retrying to leave"
    std::map<std::string, wxString>     extestInterrupted;   // TAP -> warning line

    // Called when the settings cannot be saved because the project is unknown
    std::function<void( const wxString& )> OnNotSaved;

    // --- project file ---
    // Reads <project>.bscan.json of the board's project if it is another
    // project than the current one. True if new settings were loaded.
    bool LoadProject( const kicad::BoardState& aState );
    void Save();

    // --- devices ---
    std::shared_ptr<const bscan::BsdlDevice> DeviceFor( const std::string& aPath,
                                                        std::string*       aError = nullptr );
    void                  ForgetDevice( const std::string& aPath );      // read it again next time
    bscan::TapAssignment& AssignmentFor( const bscan::TapInfo& aTap );   // created if missing
    // nullptr: not connected, or not in the chain
    const bscan::TapInfo* TapInChain( const std::string& aTap ) const;
    wxString              DeviceLabel( const bscan::TapAssignment& aAssignment ) const;
    wxString              DeviceLabel( const std::string& aTap ) const;
    PinNets               NetsOf( const std::string& aReference ) const;

    // --- EXTEST state ---
    bool InExtest( const std::string& aTap ) const;   // active or pending (this window)

    // Instruction name if OpenOCD reports a pin-controlling instruction, ""
    // if it reports another one, nullopt if unknown (no cur_ir or no BSDL)
    std::optional<std::string> HardwarePinControl( const std::string& aTap );

    // In EXTEST (or CLAMP, HIGHZ, ...) without this window having entered it:
    // from OpenOCD ('jtag cur_ir') if available, otherwise from the record of
    // an earlier session. Such TAPs are not sampled.
    bool                     IsHeld( const std::string& aTap );
    std::vector<std::string> HeldTaps();
    void                     SyncExtestRecord();   // drops records OpenOCD contradicts

    // Another TAP in EXTEST (this window's, pending, or held), empty if none.
    // Without 'jtag scan' only one at a time: OpenOCD puts all other TAPs
    // into BYPASS per scan.
    std::string PinControlElsewhere( const std::string& aTap );
    bool        CanTakeOver( const std::string& aTap );   // EXTEST confirmed by cur_ir
    // aTap and, with 'jtag scan', all other held TAPs (taken over together)
    std::vector<std::string> TakeOverGroup( const std::string& aTap );
    ActiveExtest*            ActiveExtestFor( const std::string& aReference );

    // --- pins ---
    // Resolves reference + pad to a sampler target. Returns an empty string on
    // success, otherwise the reason (aTarget may still carry the port).
    wxString ResolvePin( const std::string& aReference, const std::string& aPad,
                         bscan::LiveSampler::Target& aTarget, std::string& aIdentity );
    // The TAP whose EXTEST can be started to drive this pin, empty if none
    std::string ExtestStartable( const std::string& aReference, const std::string& aPad );

    // --- interconnect test ---
    // Devices taking part (BSDL, in the chain, EXTEST known). aEditedTap /
    // aEdited: starting states being edited instead of the saved ones.
    // aProblem: set if a device cannot take part as it is ("keep").
    std::vector<bscan::IcDevice> InterconnectDevices( wxString*          aProblem,
                                                      const std::string& aEditedTap = {},
                                                      const StartStates* aEdited    = nullptr );
    // For the setup dialog: test roles of aTap's pins with edited starting states
    TestRolesFunction TestRolesFor( const std::string& aTap );

    // (TAP, pin) set by hand in EXTEST that the user allowed the interconnect
    // test to drive anyway; dropped when the TAP leaves EXTEST
    std::set<std::pair<std::string, std::string>> icReleased;

private:
    void addCurrentStateConstraints( bscan::IcDevice& aDevice, const StartStates& aStart );

    wxWindow*                                                       m_parent;
    std::map<std::string, std::shared_ptr<const bscan::BsdlDevice>> m_devices;        // by path
    std::map<std::string, std::string>                              m_deviceErrors;   // by path
};
