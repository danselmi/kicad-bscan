// SPDX-License-Identifier: GPL-3.0-only
// "Interconnect test" tab: the nets of the test (tested / not tested and
// why, results), the review of what a run drives, Run and Repeat.
//
// The plan comes from the session's devices and board; a run goes through
// the sampler (bscan::InterconnectSession holds plan, run and statistics).

#pragma once

#include <functional>
#include <string>
#include <vector>

#include <wx/colour.h>
#include <wx/string.h>
#include <wx/panel.h>

#include "bscan/interconnect_session.h"

class wxButton;
class wxCheckBox;
class wxListView;
class wxListEvent;
class wxStaticText;
class ExtestController;
class Session;

class InterconnectPage : public wxPanel
{
public:
    // One net of the list
    struct Row
    {
        enum class Kind
        {
            Tested,     // tested (incl. included nets with other parts)
            Included,   // tested, other parts on it: can be excluded again
            Mixed,      // not tested: other parts on it, can be included
            Fixed,      // not tested: a starting state fixes it (editable)
            Other       // not tested for another reason
        };

        std::string net;
        wxString    pins;     // "U1 4, U2 2"
        wxString    status;   // result or reason
        Kind        kind   = Kind::Other;
        bool        failed = false;   // shown in the warning colour
        wxString    fixedBy;          // Fixed: "U2 pin 4 (drives 0)", for the menu
    };

    InterconnectPage( wxWindow* aParent, Session& aSession, ExtestController& aExtest );

    // Plans the test again (board, chain, settings or EXTEST changed)
    void UpdatePlan();

private:
    // Shown in groups (tested, fixed by starting states, other parts, not
    // testable), each with a header line; within a group in the given order
    void setRows( const std::vector<Row>& aRows );

    // aInfo: one line above the list. aReview: what a run does (above the
    // confirmation); a changed review resets the confirmation. aProblem: why
    // a run is not possible (empty: possible).
    void setSummary( const wxString& aInfo, const wxString& aReview, const wxString& aProblem );

    void setRunning( bool aRunning, bool aRepeat );
    void setCounters( const wxString& aText );

    void showRows();
    void run( bool aRepeat );
    void stop();
    void onResult( const bscan::Result<bscan::Captures>& aResult );
    void toggleInclude( const std::string& aNet );
    void editFixed( const std::string& aNet );
    void testFixed( const std::string& aNet );   // removes its starting states

    // One line of the list: a net (row index) or a group header (-1)
    struct Line
    {
        int      row = -1;
        wxString text[3];
        wxColour colour;
    };

    std::vector<Line> buildLines() const;
    const Row*        rowAt( long aLine ) const;
    void              onMenu( wxListEvent& aEvent );
    void              updateButtons();
    void              showStatusLine();
    void              fitLastColumn();
    void              wrapReview( bool aForce );   // to the page width

    Session&                   m_session;
    ExtestController&          m_extest;
    bscan::InterconnectSession m_test;

    std::vector<Row>  m_rows;
    std::vector<Line> m_lines;   // as shown
    wxString          m_review;
    wxString          m_problem;
    wxString          m_counterText;   // result of the last run
    bool              m_running   = false;
    bool              m_repeating = false;
    int               m_lastMin   = 0;   // width the last column needs
    int               m_reviewWrap = -1;   // width the review text is wrapped to

    wxStaticText* m_info       = nullptr;
    wxListView*   m_list       = nullptr;
    wxStaticText* m_reviewText = nullptr;
    wxCheckBox*   m_confirm    = nullptr;
    wxButton*     m_run        = nullptr;
    wxButton*     m_repeat     = nullptr;   // "Repeat", "Stop" while repeating
    wxStaticText* m_counters   = nullptr;
};
