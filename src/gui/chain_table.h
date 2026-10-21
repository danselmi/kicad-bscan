// SPDX-License-Identifier: GPL-3.0-only
// JTAG chain table built from standard controls (no wxGrid), one row per TAP:
//
//   TAP | IR len | Reference [text + "take selection" button]
//       | BSDL file [text + "..." button; "..." offers matching files first]
//       | Pin map [choice] | Check | EXTEST [button] (Forget... for a TAP not in the chain)
//
// The table only shows data and reports user input through events (below);
// the owner decides what to store. Controls are re-created only when the set of
// rows changes, so a field the user is editing is not destroyed by a refresh.

#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <wx/event.h>
#include <wx/scrolwin.h>

class wxBitmapButton;
class wxButton;
class wxChoice;
class wxFlexGridSizer;
class wxStaticText;
class wxTextCtrl;

// Events of the table (wxCommandEvent): GetInt() is the row index into the
// last SetRows(), GetString() the new text where there is one
wxDECLARE_EVENT( EVT_CHAIN_REFERENCE_CHANGED, wxCommandEvent );   // new reference
wxDECLARE_EVENT( EVT_CHAIN_TAKE_SELECTION, wxCommandEvent );      // "take selection" button
wxDECLARE_EVENT( EVT_CHAIN_PIN_MAP_CHANGED, wxCommandEvent );     // new pin map
wxDECLARE_EVENT( EVT_CHAIN_ROW_FOCUSED, wxCommandEvent );         // a control of the row got focus
wxDECLARE_EVENT( EVT_CHAIN_EXTEST, wxCommandEvent );              // "EXTEST..." / "Take over..."
wxDECLARE_EVENT( EVT_CHAIN_FORGET, wxCommandEvent );              // "Forget..." (forgettable rows)
// "..." of a row with matching BSDL files: the owner offers them (see
// BrowseFile())
wxDECLARE_EVENT( EVT_CHAIN_SUGGEST, wxCommandEvent );

class ChainTable : public wxScrolledWindow
{
public:
    struct Row
    {
        std::string              tap;
        bool                     present = true;    // false: only in the project file
        wxString                 ir;
        std::string              reference;
        std::string              bsdl;              // full path, empty if none
        std::vector<std::string> pinMaps;           // maps in the BSDL file
        std::string              pinMap;            // effective (shown) pin map
        wxString                 check;
        bool                     problem = false;   // show check in warning colour
        wxString                 details;           // shown by the owner for the focused row
        bool                     extestAllowed = false;   // TAP and BSDL match
        bool                     extestActive = false;
        bool                     extestTakeOver = false;   // in EXTEST from elsewhere: "Take over..."
        wxString                 extestNote;   // why EXTEST is not possible (tooltip)
        bool                     forgettable = false;   // not in the chain: "Forget..." instead
        int                      suggestions = 0;   // matching BSDL files found (0: none)
    };

    explicit ChainTable( wxWindow* aParent );

    void SetRows( const std::vector<Row>& aRows );

    // Enables the "take selection" buttons (something is selected in KiCad)
    void EnableTakeSelection( bool aEnable );

    // Directory the file dialog starts in when the row has no file yet
    void SetInitialDirectory( const wxString& aDir );

    // Opens the file dialog for a row (reports the choice via OnBsdlChosen)
    void BrowseFile( int aRow );

    // A BSDL file was chosen or typed for a row (row index into the last
    // SetRows). Return false to reject it (the field is reset). A callback,
    // not an event, because of the answer.
    std::function<bool( int, const std::string& )> OnBsdlChosen;

private:
    struct Controls
    {
        wxStaticText*     tap = nullptr;
        wxStaticText*     ir = nullptr;
        wxTextCtrl*       reference = nullptr;
        wxBitmapButton*   take = nullptr;
        wxTextCtrl*       bsdl = nullptr;
        wxButton*         browse = nullptr;
        // Typed path being processed (Enter followed by focus loss: once);
        // may be empty (= remove the file)
        std::optional<std::string> submitted;
        wxChoice*         pinMap = nullptr;
        wxStaticText*     check = nullptr;
        wxButton*         extest = nullptr;
    };

    void rebuild();
    void update( int aRow );
    void commitReference( int aRow );
    void commitBsdl( int aRow );
    void chooseBsdl( int aRow, const std::string& aPath );
    void updateMinSize();
    void send( wxEventType aType, int aRow, const wxString& aText = wxEmptyString );

    std::vector<Row>      m_rows;
    std::vector<Controls> m_controls;
    wxFlexGridSizer*      m_sizer = nullptr;
    int                   m_headerCount = 0;
    bool                  m_takeEnabled = false;
    bool                  m_rebuilding = false;
    wxString              m_initialDir;
};
