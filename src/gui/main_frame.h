// SPDX-License-Identifier: GPL-3.0-only
// The plugin window: the connection line (KiCad board, OpenOCD address and
// status, stay-on-top button), the EXTEST bar and three tabs:
//
//   JTAG chain         ChainPage         TAPs, BSDL files, references, EXTEST setup
//   Pins               PinsPage          selected pad, watch list, live values, drive
//   Interconnect test  InterconnectPage  shorts and opens between the chips
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
class wxNotebook;
class wxPanel;
class wxSizer;
class wxStaticText;
class ChainPage;
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

    void updateInterconnect();   // plans the test again if its tab is shown
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
    wxStaticText*         m_openocdStatus = nullptr;
    ExtestBanner*         m_extestBanner  = nullptr;
    wxNotebook*           m_tabs          = nullptr;
    ChainPage*            m_chainPage     = nullptr;
    PinsPage*             m_pinsPage      = nullptr;
    InterconnectPage*     m_icPage        = nullptr;
};
