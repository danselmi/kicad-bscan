// SPDX-License-Identifier: GPL-3.0-only
// "Pins" tab: the pad selected in the PCB editor with its live value and
// drive buttons, the watch list (pins sampled continuously) and the sampling
// interval. Works out which pins the sampler reads and shows the results.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <wx/panel.h>

#include "bscan/live_sampler.h"
#include "bscan/project_config.h"
#include "kicad/board_watcher.h"

#include "session.h"   // Session::ShownPad

class wxButton;
class wxBoxSizer;
class wxChoice;
class wxFlexGridSizer;
class wxListEvent;
class wxListView;
class wxSizer;
class wxStaticText;
class ExtestController;
class Session;

class PinsPage : public wxPanel
{
public:
    PinsPage( wxWindow* aParent, Session& aSession, ExtestController& aExtest );

    // The board from KiCad changed (Session::board): pad, buttons, targets
    void ShowBoard();

    // Works out which pins to sample (selected pad + watch list) and gives
    // them to the sampler if they changed
    void UpdateTargets();

    // The devices were read again: give the targets to the sampler again
    // with the next UpdateTargets()
    void ResampleAll() { m_targetsIdentity.reset(); }

    void ShowSample( const bscan::LiveSampler::SampleUpdate& aUpdate );
    void FillWatchList();                      // the watch list in the settings changed

    // Pins selected elsewhere (KiCad, chip view): the rows of those on the
    // watch list are selected; none on the list: the selection stays
    void SelectWatchPins( const std::vector<Session::PadKey>& aPins );
    std::vector<Session::PadKey> SelectedWatchKeys() const;   // selected rows
    void SetStatus( const wxString& aText );   // sampling status line

    // Sends EVT_LAYOUT_CHANGED (gui_events.h) when its size changes

private:
    void      buildLiveLine( wxWindow* aParent, wxSizer* aSizer );   // sampling status + interval
    wxWindow* buildPadBox( wxWindow* aParent, wxSizer* aSizer );
    wxWindow* buildWatchBox( wxWindow* aParent, wxSizer* aSizer );

    void onRateChanged( wxCommandEvent& aEvent );
    void onAddSelection( wxCommandEvent& aEvent );
    void onRemoveWatch( wxCommandEvent& aEvent );
    void onClearWatch( wxCommandEvent& aEvent );
    void onBsdlInfo( wxCommandEvent& aEvent );
    void onWatchMenu( wxListEvent& aEvent );

    void                           showPadState( const Session::ShownPad& aState );
    void                           showLiveMessage( const wxString& aMessage );
    void                           updatePadHint();
    void                           updateWatchValues();
    void                           fitWatchColumns();   // all columns (content changed)
    void                           fitLastWatchColumn();
    void                           updateDriveButtons();
    void                           updateWatchDrive();
    std::vector<bscan::WatchEntry> selectedWatchPins() const;
    void                           layoutChanged();
    void                           onWatchSelection();   // by the user: pick, KiCad, chip view

    static std::string watchKey( const bscan::WatchEntry& aEntry );

    Session&          m_session;
    ExtestController& m_extest;

    // What the sampler was given; reset() forces SetTargets() (an empty
    // string is a valid identity: nothing to sample)
    std::optional<std::string>                           m_targetsIdentity;
    unsigned                                             m_targetsVersion = 0;
    size_t                                               m_targetsCount   = 0;
    std::map<std::string, bscan::LiveSampler::PinResult> m_watchValues;   // by watch key
    std::map<std::string, wxString>                      m_watchNotes;    // why not sampled
    std::map<std::string, std::string>                   m_watchPorts;
    wxString m_boardHint;     // selection problems (from KiCad)
    wxString m_padLiveNote;   // problems with the live value of the pad

    // --- controls ---
    wxStaticText* m_liveStatus = nullptr;
    wxChoice*     m_rate       = nullptr;

    wxFlexGridSizer*       m_padGrid    = nullptr;
    wxStaticText*          m_padHint    = nullptr;
    wxStaticText*          m_reference  = nullptr;
    wxStaticText*          m_padNumber  = nullptr;
    wxStaticText*          m_net        = nullptr;
    wxButton*              m_bsdlInfo   = nullptr;
    wxStaticText*          m_livePort   = nullptr;
    wxStaticText*          m_liveInput  = nullptr;
    wxStaticText*          m_liveOutput = nullptr;
    wxBoxSizer*            m_driveRow   = nullptr;
    wxStaticText*          m_driveLabel = nullptr;
    wxStaticText*          m_driveHint  = nullptr;
    std::vector<wxButton*> m_driveButtons;

    wxListView*            m_watchList       = nullptr;
    bool                   m_watchSyncing    = false;   // selecting rows from elsewhere
    bool                   m_watchPending    = false;   // onWatchSelection() queued
    wxButton*              m_addSelection    = nullptr;
    wxBoxSizer*            m_watchButtons    = nullptr;
    wxStaticText*          m_watchDriveLabel = nullptr;
    std::vector<wxButton*> m_watchDriveButtons;
};
