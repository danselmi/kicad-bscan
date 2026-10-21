// SPDX-License-Identifier: GPL-3.0-only
#include "interconnect_page.h"

#include <algorithm>
#include <set>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/listctrl.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "extest_controller.h"
#include "extest_dialogs.h"
#include "gui_events.h"
#include "session.h"

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


InterconnectPage::InterconnectPage( wxWindow* aParent, Session& aSession,
                                    ExtestController& aExtest ) :
        wxPanel( aParent ),
        m_session( aSession ),
        m_extest( aExtest )
{
    auto* top = new wxBoxSizer( wxVERTICAL );

    m_info = new wxStaticText( this, wxID_ANY, wxEmptyString );
    top->Add( m_info, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    m_list = new wxListView( this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                             wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES );
    m_list->AppendColumn( _( "Net" ) );
    m_list->AppendColumn( _( "Boundary-scan pins" ) );
    m_list->AppendColumn( _( "Result / why not tested" ) );
    m_list->SetMinSize( wxSize( FromDIP( 480 ), FromDIP( 160 ) ) );
    m_list->SetToolTip( _( "Click a net to select it in the PCB editor. Right-click (or "
                           "double-click): include or exclude a net with other parts, test a net "
                           "that a starting state keeps out." ) );
    top->Add( m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, FromDIP( 8 ) );

    // Review: what a run drives
    m_reviewText = new wxStaticText( this, wxID_ANY, wxEmptyString );
    top->Add( m_reviewText, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );

    m_confirm = new wxCheckBox( this, wxID_ANY,
                                _( "I have checked the nets above: driving them can do no "
                                   "harm on this board." ) );
    top->Add( m_confirm, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );

    auto* buttons = new wxBoxSizer( wxHORIZONTAL );
    m_run         = new wxButton( this, wxID_ANY, _( "Run" ) );
    m_repeat      = new wxButton( this, wxID_ANY, _( "Repeat" ) );
    m_run->SetToolTip( _( "Apply the test patterns once" ) );
    m_repeat->SetToolTip( _( "Apply the test patterns again and again until stopped; faults that "
                             "come and go are counted per net (press on parts, bend the board)" ) );
    m_counters = new wxStaticText( this, wxID_ANY, wxEmptyString );
    buttons->Add( m_run, 0, wxRIGHT, FromDIP( 4 ) );
    buttons->Add( m_repeat, 0 );
    buttons->Add( m_counters, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 12 ) );
    top->Add( buttons, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    SetSizer( top );

    m_confirm->Bind( wxEVT_CHECKBOX, [this]( wxCommandEvent& ) { updateButtons(); } );
    m_run->Bind( wxEVT_BUTTON, [this]( wxCommandEvent& ) { run( false ); } );
    m_repeat->Bind( wxEVT_BUTTON,
                    [this]( wxCommandEvent& )
                    {
                        if( m_repeating )
                        {
                            stop();
                        }
                        else
                        {
                            run( true );
                        }
                    } );
    // Re-wrap the review text to the new width (after the layout has settled)
    Bind( wxEVT_SIZE,
          [this]( wxSizeEvent& aEvent )
          {
              aEvent.Skip();
              CallAfter( [this]() { wrapReview( false ); } );
          } );
    m_list->Bind( wxEVT_SIZE,
                  [this]( wxSizeEvent& aEvent )
                  {
                      aEvent.Skip();
                      CallAfter( [this]() { fitLastColumn(); } );
                  } );
    m_list->Bind( wxEVT_LIST_COL_END_DRAG,
                  [this]( wxListEvent& aEvent )
                  {
                      aEvent.Skip();
                      CallAfter( [this]() { fitLastColumn(); } );
                  } );
    m_list->Bind( wxEVT_LIST_ITEM_RIGHT_CLICK, &InterconnectPage::onMenu, this );
    m_list->Bind( wxEVT_LIST_ITEM_ACTIVATED, &InterconnectPage::onMenu, this );
    m_list->Bind( wxEVT_LIST_ITEM_SELECTED,
                  [this]( wxListEvent& aEvent )
                  {
                      // Pads and tracks of the net, so KiCad shows where it runs
                      if( const Row* r = rowAt( aEvent.GetIndex() ); r && m_session.watcher )
                          m_session.watcher->SelectNet( r->net );
                  } );

    updateButtons();
}


// Groups in display order
enum class Group
{
    Tested,
    Fixed,
    Mixed,
    Other
};


static Group groupOf( InterconnectPage::Row::Kind aKind )
{
    using Kind = InterconnectPage::Row::Kind;

    switch( aKind )
    {
    case Kind::Tested:
    case Kind::Included:
        return Group::Tested;
    case Kind::Fixed:
        return Group::Fixed;
    case Kind::Mixed:
        return Group::Mixed;
    case Kind::Other:
        return Group::Other;
    }

    return Group::Other;
}


static void groupHeader( Group aGroup, size_t aCount, wxString& aName, wxString& aHint )
{
    switch( aGroup )
    {
    case Group::Tested:
        aName = wxString::Format( _( "Tested (%zu)" ), aCount );
        break;

    case Group::Fixed:
        aName = wxString::Format( _( "Fixed (%zu)" ), aCount );
        aHint = _( "not tested: kept out by starting states; right-click to test them" );
        break;

    case Group::Mixed:
        aName = wxString::Format( _( "Other parts (%zu)" ), aCount );
        aHint = _( "not tested: other parts on the net; right-click to include" );
        break;

    case Group::Other:
        aName = wxString::Format( _( "Not testable (%zu)" ), aCount );
        aHint = _( "the BSDL files or the board do not allow it" );
        break;
    }
}


std::vector<InterconnectPage::Line> InterconnectPage::buildLines() const
{
    const wxColour grey = wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT );
    const wxColour text = wxSystemSettings::GetColour( wxSYS_COLOUR_LISTBOXTEXT );
    const wxColour red( 200, 40, 30 );

    std::vector<Line> lines;

    for( Group g : { Group::Tested, Group::Fixed, Group::Mixed, Group::Other } )
    {
        std::vector<int> members;

        for( size_t i = 0; i < m_rows.size(); ++i )
            if( groupOf( m_rows[i].kind ) == g )
                members.push_back( static_cast<int>( i ) );

        if( members.empty() )
            continue;

        Line header;
        header.colour = text;
        groupHeader( g, members.size(), header.text[0], header.text[2] );
        lines.push_back( header );

        for( int i : members )
        {
            const Row& r = m_rows[i];
            Line       l;
            l.row     = i;
            l.text[0] = "    " + fromUtf8( r.net );
            l.text[1] = r.pins;
            l.text[2] = r.status;
            l.colour  = r.failed ? red : g == Group::Tested ? text : grey;
            lines.push_back( l );
        }
    }

    return lines;
}


void InterconnectPage::setRows( const std::vector<Row>& aRows )
{
    m_rows                  = aRows;
    std::vector<Line> lines = buildLines();

    // Same lines (nets and headers): only update texts, keeps the selection
    bool same = lines.size() == m_lines.size();

    for( size_t i = 0; same && i < lines.size(); ++i )
        same = lines[i].row == m_lines[i].row && lines[i].text[0] == m_lines[i].text[0];

    m_lines = std::move( lines );
    m_list->Freeze();

    if( !same )
        m_list->DeleteAllItems();

    wxFont bold = m_list->GetFont().Bold();

    for( size_t i = 0; i < m_lines.size(); ++i )
    {
        const Line& l = m_lines[i];
        long        n = static_cast<long>( i );

        if( !same )
        {
            m_list->InsertItem( n, l.text[0] );

            if( l.row < 0 )
                m_list->SetItemFont( n, bold );
        }

        for( int c = 1; c < 3; ++c )
            if( m_list->GetItemText( n, c ) != l.text[c] )
                m_list->SetItem( n, c, l.text[c] );

        m_list->SetItemTextColour( n, l.colour );
    }

    if( !same )
    {
        for( int c = 0; c + 1 < m_list->GetColumnCount(); ++c )
            m_list->SetColumnWidth( c, wxLIST_AUTOSIZE_USEHEADER );
    }

    // Last column: wide enough for its longest text (it grows while
    // repeating), otherwise the remaining space
    int        last = m_list->GetColumnCount() - 1;
    wxListItem header;
    header.SetMask( wxLIST_MASK_TEXT );
    m_list->GetColumn( last, header );
    m_lastMin = m_list->GetTextExtent( header.GetText() ).x;

    for( const Line& l : m_lines )
        m_lastMin = std::max( m_lastMin, m_list->GetTextExtent( l.text[2] ).x );

    m_lastMin += FromDIP( 16 );
    fitLastColumn();

    m_list->Thaw();
}


void InterconnectPage::fitLastColumn()
{
    int last = m_list->GetColumnCount() - 1;
    int used = 0;

    for( int c = 0; c < last; ++c )
        used += m_list->GetColumnWidth( c );

    int width = std::max( m_lastMin, m_list->GetClientSize().x - used );

    if( m_list->GetColumnWidth( last ) != width )
        m_list->SetColumnWidth( last, width );
}


const InterconnectPage::Row* InterconnectPage::rowAt( long aLine ) const
{
    if( aLine < 0 || aLine >= static_cast<long>( m_lines.size() ) || m_lines[aLine].row < 0 )
        return nullptr;

    return &m_rows[m_lines[aLine].row];
}


void InterconnectPage::setSummary( const wxString& aInfo, const wxString& aReview,
                                   const wxString& aProblem )
{
    m_info->SetLabel( aInfo );

    if( aReview != m_review )
    {
        // Something else would be driven now: confirm again
        m_review = aReview;
        m_confirm->SetValue( false );
        wrapReview( true );
    }

    m_problem = aProblem;
    showStatusLine();
    updateButtons();
}


void InterconnectPage::wrapReview( bool aForce )
{
    int width = std::max( FromDIP( 300 ), GetClientSize().x - FromDIP( 16 ) );

    if( !aForce && width == m_reviewWrap )
        return;   // also ends the size event caused by the layout below

    m_reviewWrap = width;
    m_reviewText->SetLabel( m_review );
    m_reviewText->Wrap( width );

    // The text wrapped to a wide window must not keep the window from
    // getting narrower again
    m_reviewText->SetMinSize( wxSize( FromDIP( 300 ), m_reviewText->GetBestSize().y ) );
    Layout();
    SendGuiEvent( this, EVT_LAYOUT_CHANGED );   // its height may have changed
}


// Next to the buttons: why a run is not possible, otherwise the result of
// the last run (or nothing)
void InterconnectPage::showStatusLine()
{
    wxString text = m_problem.empty() || m_running
                            ? m_counterText
                            : wxString::Format( _( "Cannot run: %s" ), m_problem );

    if( m_counters->GetLabel() != text )
        m_counters->SetLabel( text );
}


void InterconnectPage::setRunning( bool aRunning, bool aRepeat )
{
    m_running   = aRunning;
    m_repeating = aRunning && aRepeat;
    m_repeat->SetLabel( m_repeating ? _( "Stop" ) : _( "Repeat" ) );
    showStatusLine();
    updateButtons();
}


void InterconnectPage::setCounters( const wxString& aText )
{
    m_counterText = aText;
    showStatusLine();
}


void InterconnectPage::updateButtons()
{
    bool canStart = !m_running && m_problem.empty() && m_confirm->GetValue();

    m_run->Enable( canStart );
    m_repeat->Enable( canStart || m_repeating );
    m_confirm->Enable( !m_running );
}


void InterconnectPage::onMenu( wxListEvent& aEvent )
{
    const Row* found = rowAt( aEvent.GetIndex() );

    if( !found || m_running )
        return;

    const Row row = *found;   // copy: the menu runs an event loop
    wxMenu    menu;
    const int ID_TOGGLE = wxID_HIGHEST + 1;
    const int ID_TEST   = wxID_HIGHEST + 2;
    const int ID_EDIT   = wxID_HIGHEST + 3;

    if( row.kind == Row::Kind::Mixed )
    {
        menu.Append( ID_TOGGLE, _( "Include in the test (drives the other parts on this net)" ) );
    }
    else if( row.kind == Row::Kind::Included )
    {
        menu.Append( ID_TOGGLE, _( "Exclude from the test" ) );
    }
    else if( row.kind == Row::Kind::Fixed )
    {
        menu.Append( ID_TEST,
                     wxString::Format( _( "Test this net (releases %s)..." ), row.fixedBy ) );
        menu.Append( ID_EDIT, _( "Edit starting state..." ) );
    }

    if( menu.GetMenuItemCount() == 0 )
        return;

    // Double-click: the first action directly
    int choice = aEvent.GetEventType() == wxEVT_LIST_ITEM_ACTIVATED
                         ? menu.FindItemByPosition( 0 )->GetId()
                         : GetPopupMenuSelectionFromUser( menu );

    if( choice == ID_TOGGLE )
        toggleInclude( row.net );
    else if( choice == ID_TEST )
        testFixed( row.net );
    else if( choice == ID_EDIT )
        editFixed( row.net );
}


// ============================================================================
// The test: plan, run, results
// ============================================================================

void InterconnectPage::UpdatePlan()
{
    wxString              problem;
    auto                  devices = m_session.InterconnectDevices( &problem );
    std::set<std::string> included( m_session.config.interconnectInclude.begin(),
                                    m_session.config.interconnectInclude.end() );
    m_test.SetPlan(
            bscan::PlanInterconnect( std::move( devices ), m_session.board.padNets, included ) );

    const bscan::InterconnectPlan& plan   = m_test.Plan();
    std::vector<size_t>            used   = bscan::UsedDevices( plan );   // driven by a run
    size_t                         tested = plan.TestedCount();

    if( !m_session.chain.connected )
        problem = _( "OpenOCD is not connected." );
    else if( m_session.board.padNets.empty() )
        problem = _( "No board from KiCad." );
    else if( !m_session.HeldTaps().empty() )
        problem = _( "A device is in EXTEST from an earlier session: take it over or leave "
                     "EXTEST first (bar at the top)." );
    else if( !m_session.extestPending.empty() )
        problem = _( "EXTEST is being entered." );
    else if( tested == 0 )
        problem = _( "No net can be tested." );
    else if( used.size() > 1 && !m_session.chain.jtagScan )
        problem = _( "Testing several chips together needs OpenOCD's 'jtag scan'." );

    // Review: what a run drives
    wxString enter, stay, mixed;

    for( size_t d : used )
    {
        const bscan::IcDevice& dev   = plan.devices[d];
        wxString               label = fromUtf8( dev.reference ) + " (" + fromUtf8( dev.tap ) + ")";
        wxString&              list  = m_session.extest.contains( dev.tap ) ? stay : enter;
        list << ( list.empty() ? "" : ", " ) << label;
    }

    for( const bscan::TestNet& n : plan.nets )
    {
        if( n.status == bscan::TestNet::Status::Tested && n.Mixed() )
        {
            mixed << ( mixed.empty() ? "" : "; " ) << fromUtf8( n.name ) << " (";

            for( size_t i = 0; i < n.otherParts.size(); ++i )
                mixed << ( i ? ", " : "" ) << fromUtf8( n.otherParts[i] );

            mixed << ")";
        }
    }

    wxString review;

    if( tested )
    {
        review = wxString::Format( _( "A run drives %zu nets with test patterns." ), tested );

        if( !enter.empty() )
            review << " "
                   << wxString::Format( _( "%s go into EXTEST for the test and back to "
                                           "normal operation afterwards; their other output "
                                           "pins get their starting state (BSDL safe value "
                                           "unless set)." ),
                                        enter );

        if( !stay.empty() )
            review << " "
                   << wxString::Format( _( "%s stay in EXTEST; their other output pins keep "
                                           "what they drive now, and afterwards the tested "
                                           "pins drive what they drove before." ),
                                        stay );

        if( !mixed.empty() )
            review << "\n"
                   << wxString::Format( _( "Nets with other parts that are driven: %s." ), mixed );
    }

    size_t notTested = plan.nets.size() - tested;
    setSummary( wxString::Format( _( "%zu nets tested, %zu not tested; %zu scans per "
                                     "run." ),
                                  tested, notTested, tested ? plan.patterns.size() + 1 : 0 ),
                review, problem );
    showRows();
}


// Result of a net after aRuns runs: OK, the fault, or how often it failed
static wxString resultText( const bscan::InterconnectSession::NetStat& aStat, int aRuns )
{
    wxString last = fromUtf8( aStat.lastFail );

    if( aStat.fails == 0 )
        return aRuns > 1 ? wxString::Format( _( "OK (%d runs)" ), aRuns ) : _( "OK" );

    if( aRuns == 1 )
        return last;

    if( aStat.failingNow )
        return wxString::Format( _( "%s (%d x in %d runs)" ), last, aStat.fails, aRuns );

    return wxString::Format( _( "%s (%d x in %d runs), now OK" ), last, aStat.fails, aRuns );
}


void InterconnectPage::showRows()
{
    using Kind   = Row::Kind;
    using Status = bscan::TestNet::Status;

    std::vector<Row> rows;

    const int runs = m_test.Runs();

    for( const bscan::TestNet& n : m_test.Plan().nets )
    {
        Row r;
        r.net = n.name;

        for( size_t i = 0; i < n.bsPins.size(); ++i )
            r.pins << ( i ? ", " : "" ) << fromUtf8( n.bsPins[i].reference ) << " "
                   << fromUtf8( n.bsPins[i].pad );

        if( n.status == Status::Tested )
        {
            r.kind = n.Mixed() ? Kind::Included : Kind::Tested;

            if( const auto* s = m_test.Stat( n.name ) )
            {
                r.failed = s->fails > 0;
                r.status = resultText( *s, runs );
            }
            else
            {
                r.status = n.Mixed() ? _( "will be tested (other parts on the net)" )
                                     : _( "will be tested" );
            }
        }
        else
        {
            r.kind   = n.status == Status::OtherParts ? Kind::Mixed
                       : n.status == Status::Fixed    ? Kind::Fixed
                                                      : Kind::Other;
            r.status = fromUtf8( n.reason );

            for( size_t i : n.fixedBy )
            {
                const bscan::NetPin& p = n.bsPins[i];
                r.fixedBy << ( r.fixedBy.empty() ? "" : ", " )
                          << fromUtf8( p.reference + " pin " + p.pad );
            }
        }

        rows.push_back( std::move( r ) );
    }

    setRows( rows );   // grouped by the page; by name within a group
}


void InterconnectPage::run( bool aRepeat )
{
    UpdatePlan();

    if( !m_problem.empty() || m_test.Running() )
        return;

    auto req = m_test.Start( m_session.chain.taps, aRepeat );

    if( !req )
        return;

    setRunning( true, aRepeat );
    setCounters( _( "Running..." ) );
    showRows();

    m_session.sampler.RunInterconnect( std::move( *req ),
                                       [this]( const bscan::Result<bscan::Captures>& aResult )
                                       { CallAfter( [this, aResult] { onResult( aResult ); } ); } );
}


void InterconnectPage::stop()
{
    m_session.sampler.StopInterconnect();
    m_test.Stop();
    setRunning( false, false );
}


void InterconnectPage::onResult( const bscan::Result<bscan::Captures>& aResult )
{
    if( !aResult )
    {
        m_test.Stop();
        setRunning( false, false );
        setCounters( wxString::Format( _( "Stopped: %s" ), fromUtf8( aResult.error().message ) ) );
        return;
    }

    auto sum = m_test.AddRun( *aResult );

    if( !m_test.Running() )
        setRunning( false, false );

    size_t ok = sum.tested - sum.failing;
    setCounters( sum.runs > 1 ? wxString::Format( _( "%d runs: %zu nets OK, %d with "
                                                     "faults (%d now)" ),
                                                  sum.runs, ok, sum.failing, sum.failingNow )
                              : wxString::Format( _( "%zu nets OK, %d with faults" ), ok,
                                                  sum.failing ) );
    showRows();
}


void InterconnectPage::toggleInclude( const std::string& aNet )
{
    auto& list = m_session.config.interconnectInclude;

    if( auto it = std::ranges::find( list, aNet ); it != list.end() )
        list.erase( it );
    else
        list.insert( std::ranges::upper_bound( list, aNet ), aNet );

    m_session.Save();
    UpdatePlan();
}


void InterconnectPage::editFixed( const std::string& aNet )
{
    const bscan::InterconnectPlan& plan = m_test.Plan();

    for( const bscan::TestNet& n : plan.nets )
    {
        if( n.name != aNet )
            continue;

        for( const bscan::NetPin& p : n.bsPins )
        {
            const bscan::IcDevice& dev = plan.devices[p.device];

            if( !dev.constraints.contains( p.pad ) )
                continue;

            m_extest.Setup( dev.tap, {}, { p.pad } );   // the dialog shows this pin
            return;
        }
    }
}


void InterconnectPage::testFixed( const std::string& aNet )
{
    const bscan::InterconnectPlan& plan = m_test.Plan();
    auto                           net  = std::ranges::find_if( plan.nets,
                                                                [&]( const bscan::TestNet& n ) { return n.name == aNet; } );

    if( net == plan.nets.end() || net->fixedBy.empty() )
        return;

    // What keeps the net out: starting states (removed from the settings) and
    // pins set by hand in EXTEST (released for the test); listed for the question
    std::vector<std::pair<std::string, std::string>> starting, current;
    wxString                                         startList, currentList;

    for( size_t i : net->fixedBy )
    {
        const bscan::NetPin&   p    = net->bsPins[i];
        const bscan::IcDevice& dev  = plan.devices[p.device];
        auto                   c    = dev.constraints.find( p.pad );
        bool                   now  = dev.currentState.contains( p.pad );
        wxString               line = "\n    " + fromUtf8( p.reference + " pin " + p.pad ) + " ("
                        + ( c != dev.constraints.end() ? DriveLabel( c->second ) : wxString( "?" ) )
                        + ")";

        ( now ? current : starting ).emplace_back( dev.tap, p.pad );
        ( now ? currentList : startList ) << line;
    }

    wxString text = wxString::Format( _( "Test %s?" ), fromUtf8( aNet ) );

    if( !startList.empty() )
        text << "\n\n"
             << _( "These starting states are removed (the pins then get the BSDL safe value "
                   "when EXTEST starts):" )
             << startList;

    if( !currentList.empty() )
        text << "\n\n"
             << _( "These pins, set by hand in EXTEST, get test patterns during the test and "
                   "their state back afterwards:" )
             << currentList;

    text << "\n\n" << _( "Check that driving them can do no harm." );

    if( wxMessageBox( text, _( "Interconnect test" ), wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION,
                      this )
        != wxYES )
        return;

    for( const auto& [tap, pin] : starting )
        if( bscan::TapAssignment* a = m_session.config.Find( tap ) )
            a->extestStart.erase( pin );

    for( const auto& item : current )
        m_session.icReleased.insert( item );

    if( !starting.empty() )
        m_session.Save();

    UpdatePlan();
}
