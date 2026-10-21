// SPDX-License-Identifier: GPL-3.0-only
// "JTAG chain" tab: one row per TAP (from OpenOCD, plus TAPs only known from
// the project settings) with IR length, reference, BSDL file, pin map,
// check result and EXTEST button; details of the focused row; the BSDL
// folders that are searched for files matching a TAP's IDCODE.

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <wx/panel.h>

#include "bscan/bsdl_index.h"
#include "bscan/chain.h"
#include "bscan/tap_status.h"

#include "chain_table.h"

class wxCommandEvent;
class wxSizer;
class wxStaticText;
class ExtestController;
class Session;

class ChainPage : public wxPanel
{
public:
    ChainPage( wxWindow* aParent, Session& aSession, ExtestController& aExtest );
    ~ChainPage() override;

    void Rebuild();   // rows from the chain and the settings, then Fill()
    void Fill();      // the table from the rows (check results, EXTEST state)

    void EnableTakeSelection( bool aEnable );
    void ShowNote( const wxString& aText );   // in the details line

    void StartBsdlScan();   // (again) with the current folders
    void StopBsdlScan();

    // Sends EVT_ASSIGNMENTS_CHANGED and EVT_LAYOUT_CHANGED (gui_events.h)

private:
    // One row of the chain table
    struct ChainRow
    {
        bscan::TapInfo tap;
        bool           present = true;   // false: only in the project file (offline)
    };

    wxWindow*       buildChainBox( wxWindow* aParent, wxSizer* aSizer );
    bscan::TapFacts factsFor( const ChainRow& aRow );   // what the status depends on
    ChainTable::Row tableRow( const ChainRow& aRow );
    void            rebuildRows();
    wxString        rowDetails( const ChainRow& aRow );
    void            showRowDetails( int aRow );

    void onRefreshChain( wxCommandEvent& aEvent );
    void onReferenceChanged( int aRow, const std::string& aValue );
    void onPinMapChanged( int aRow, const std::string& aMap );
    void onTakeSelection( int aRow );
    bool onBsdlChosen( int aRow, const std::string& aPath );
    void onForgetTap( int aRow );
    void onBsdlFolders();
    void onSuggestBsdl( int aRow );
    void onBsdlScanDone( std::shared_ptr<const bscan::BsdlIndex> aIndex,
                         const bscan::BsdlIndex::Stats&          aStats );

    void assignmentsChanged();
    void layoutChanged();

    Session&          m_session;
    ExtestController& m_extest;

    std::vector<ChainRow> m_rows;
    int                   m_detailRow = -1;      // row shown below the table
    bool                  m_resample  = false;   // for the next assignmentsChanged()

    std::vector<std::string>                m_userBsdlDirs;
    std::shared_ptr<const bscan::BsdlIndex> m_bsdlIndex;
    std::thread                             m_scanThread;
    std::atomic<bool>                       m_scanCancel{ false };
    bool                                    m_scanRunning = false;
    bool                                    m_scanAgain   = false;
    wxString                                m_scanStatus;

    ChainTable*   m_chainTable = nullptr;
    wxStaticText* m_rowDetails = nullptr;
};
