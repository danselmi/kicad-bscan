// SPDX-License-Identifier: GPL-3.0-only
// The plugin window: the connection line (KiCad board, OpenOCD address and
// status, stay-on-top button), the EXTEST bar and three tabs:
//
//   JTAG chain         ChainPage         TAPs, BSDL files, references, EXTEST setup
//   Pins               PinsPage          selected pad, watch list, live values, drive
//   Interconnect test  InterconnectPage  shorts and opens between the chips
//
// and the chip view (ChipView), a separate window opened from here.
//
// The frame owns the workers (OpenOCD sampler, KiCad board watcher), the
// Session the parts share and the ExtestController, and passes updates and
// changes from one part to the others.

#pragma once

#include <memory>

#include <wx/frame.h>

#include "bscan/live_sampler.h"
#include "kicad/board_watcher.h"

#include "extest_controller.h"
#include "session.h"

class wxBitmapToggleButton;
class wxButton;
class wxNotebook;
class wxPanel;
class wxSizer;
class wxStaticText;
class ChainPage;
class ChipView;
class ExtestBanner;
class InterconnectPage;
class PinsPage;

class MainFrame : public wxFrame
{
public:
    explicit MainFrame( const kicad::Client::Config& aKiCadConfig );
    ~MainFrame() override;

    void BringToFront();

private:
    void buildConnectionLine( wxSizer* aSizer );
    void connectParts();
    void startWorkers( const kicad::Client::Config& aKiCadConfig );
    void stopWorkers();

    // --- events
    void onClose( wxCloseEvent& aEvent );
    void onPinToggled( wxCommandEvent& aEvent );
    void onChangeServer( wxCommandEvent& aEvent );
    void applyStayOnTop( bool aOnTop );

    // --- updates from the workers (called in the GUI thread)
    void showBoardState( const kicad::BoardState& aState );
    void showChain( const bscan::LiveSampler::ChainUpdate& aUpdate );
    void loadProject( const kicad::BoardState& aState );
    void applySettings();               // after other settings were loaded
    void useStandaloneSettings();       // no KiCad project: the last file or the default
    void onSettingsFile();              // menu: open, save as, default
    void showSettingsSource();          // KiCad line and "Settings file..." button

    void updateInterconnect();   // plans the test again if its tab is shown
    void updateChipView();       // if it is open
    void openChipView();
    void ensureFits();

    // --- workers
    std::unique_ptr<kicad::BoardWatcher> m_watcher;
    bscan::LiveSampler                   m_sampler;

    // --- shared with the parts (GUI thread only)
    Session          m_session;
    ExtestController m_extestCtl;

    // --- controls
    wxPanel*              m_panel         = nullptr;
    wxBitmapToggleButton* m_pin           = nullptr;   // keep window on top
    wxStaticText*         m_kicadStatus   = nullptr;
    wxString              m_kicadText;                 // KiCad line without the settings file
    wxButton*             m_settingsFile  = nullptr;   // only without a KiCad project
    bool                  m_standaloneTried = false;   // useStandaloneSettings() once
    wxStaticText*         m_openocdStatus = nullptr;
    ExtestBanner*         m_extestBanner  = nullptr;
    wxNotebook*           m_tabs          = nullptr;
    ChainPage*            m_chainPage     = nullptr;
    PinsPage*             m_pinsPage      = nullptr;
    InterconnectPage*     m_icPage        = nullptr;
    ChipView*             m_chipView      = nullptr;   // created when first opened
};
