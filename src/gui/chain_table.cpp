// SPDX-License-Identifier: GPL-3.0-only
#include "chain_table.h"

#include <algorithm>

#include <wx/artprov.h>
#include <wx/bmpbuttn.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/filename.h>
#include <wx/filedlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include "gui_events.h"

static constexpr int COLUMNS      = 7;
static constexpr int VISIBLE_ROWS = 6;   // more rows scroll

static const wxString BSDL_WILDCARD =
        _( "BSDL files (*.bsd;*.bsdl)|*.bsd;*.bsdl;*.BSD;*.BSDL|All files (*)|*" );


static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}



wxDEFINE_EVENT( EVT_CHAIN_REFERENCE_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHAIN_TAKE_SELECTION, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHAIN_PIN_MAP_CHANGED, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHAIN_ROW_FOCUSED, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHAIN_EXTEST, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHAIN_FORGET, wxCommandEvent );
wxDEFINE_EVENT( EVT_CHAIN_SUGGEST, wxCommandEvent );

ChainTable::ChainTable( wxWindow* aParent ) :
        wxScrolledWindow( aParent, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                          wxVSCROLL | wxHSCROLL )
{
    SetScrollRate( FromDIP( 8 ), FromDIP( 8 ) );

    m_sizer = new wxFlexGridSizer( COLUMNS, FromDIP( 4 ), FromDIP( 8 ) );
    m_sizer->AddGrowableCol( 3 );   // BSDL file

    const std::pair<wxString, wxString> headers[] = {
        { _( "TAP" ), _( "TAPs in scan chain order, as configured in OpenOCD" ) },
        { _( "IR length" ), _( "Length of the instruction register in bits, as reported by "
                               "OpenOCD" ) },
        { _( "Reference" ), _( "Reference designator of the device in the layout" ) },
        { _( "BSDL file" ), _( "Boundary-scan description of the device. TAPs without BSDL "
                               "file stay in BYPASS." ) },
        { _( "Pin map" ), _( "Package variant in the BSDL file (pin numbering). Only "
                             "selectable if the file contains more than one." ) },
        { _( "Check" ), _( "Does the BSDL file match the TAP (IR length, IDCODE)?" ) },
        { _( "Drive pins" ), _( "EXTEST: drive output pins via boundary scan" ) },
    };

    for( const auto& [title, tip] : headers )
    {
        auto* label = new wxStaticText( this, wxID_ANY, title );
        label->SetFont( label->GetFont().Bold() );
        label->SetToolTip( tip );
        m_sizer->Add( label, 0, wxALIGN_CENTER_VERTICAL );
    }

    m_headerCount = COLUMNS;
    SetSizer( m_sizer );
    updateMinSize();
}


void ChainTable::SetInitialDirectory( const wxString& aDir )
{
    m_initialDir = aDir;
}


void ChainTable::EnableTakeSelection( bool aEnable )
{
    m_takeEnabled = aEnable;

    for( Controls& c : m_controls )
        if( c.take )
            c.take->Enable( aEnable );
}


void ChainTable::SetRows( const std::vector<Row>& aRows )
{
    bool sameShape = aRows.size() == m_rows.size();

    for( size_t i = 0; sameShape && i < aRows.size(); ++i )
        sameShape = aRows[i].tap == m_rows[i].tap && aRows[i].present == m_rows[i].present;

    m_rows = aRows;

    if( !sameShape )
    {
        rebuild();
        return;
    }

    for( int r = 0; r < static_cast<int>( m_rows.size() ); ++r )
        update( r );
}


void ChainTable::rebuild()
{
    // Destroying a focused text field sends a kill-focus event; ignore it
    m_rebuilding = true;
    Freeze();

    // Remove all rows below the header
    while( m_sizer->GetItemCount() > static_cast<size_t>( m_headerCount ) )
    {
        wxSizerItem* item = m_sizer->GetItem( m_headerCount );

        if( wxWindow* w = item->GetWindow() )
        {
            m_sizer->Detach( m_headerCount );
            w->Destroy();
        }
        else if( wxSizer* s = item->GetSizer() )
        {
            s->Clear( true );
            m_sizer->Remove( m_headerCount );
        }
        else
        {
            m_sizer->Remove( m_headerCount );
        }
    }

    m_controls.assign( m_rows.size(), Controls{} );

    for( int r = 0; r < static_cast<int>( m_rows.size() ); ++r )
    {
        Controls& c = m_controls[r];

        c.tap = new wxStaticText( this, wxID_ANY, fromUtf8( m_rows[r].tap ) );
        c.ir  = new wxStaticText( this, wxID_ANY, wxEmptyString );

        // Reference: text field plus "take selection" button
        auto* refSizer = new wxBoxSizer( wxHORIZONTAL );
        c.reference    = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                         wxSize( FromDIP( 70 ), -1 ), wxTE_PROCESS_ENTER );
        c.reference->SetToolTip( _( "Reference designator of this device in the layout" ) );
        c.take = new wxBitmapButton( this, wxID_ANY,
                                     wxArtProvider::GetBitmapBundle( wxART_GO_BACK,
                                                                     wxART_BUTTON ) );
        c.take->SetToolTip( _( "Take the footprint selected in the PCB editor (or the "
                               "footprint of the selected pad)" ) );
        c.take->Enable( m_takeEnabled );
        refSizer->Add( c.reference, 1, wxALIGN_CENTER_VERTICAL );
        refSizer->Add( c.take, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 2 ) );

        // BSDL file: path + "..." (opens the matching files first, if any)
        auto* bsdlSizer = new wxBoxSizer( wxHORIZONTAL );
        c.bsdl = new wxTextCtrl( this, wxID_ANY, wxEmptyString, wxDefaultPosition,
                                 wxSize( FromDIP( 200 ), -1 ), wxTE_PROCESS_ENTER );
        c.browse = new wxButton( this, wxID_ANY, "...", wxDefaultPosition, wxDefaultSize,
                                 wxBU_EXACTFIT );
        bsdlSizer->Add( c.bsdl, 1, wxALIGN_CENTER_VERTICAL );
        bsdlSizer->Add( c.browse, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP( 2 ) );

        c.pinMap = new wxChoice( this, wxID_ANY );
        c.pinMap->SetToolTip( _( "Package variant in the BSDL file (pin numbering)" ) );
        c.check  = new wxStaticText( this, wxID_ANY, wxEmptyString );

        c.extest = new wxButton( this, wxID_ANY, _( "EXTEST..." ), wxDefaultPosition,
                                 wxDefaultSize, wxBU_EXACTFIT );

        // One width for all labels the button can show: the column does not
        // change when the label does (no relayout needed, no cut-off text,
        // all rows alike)
        int extestWidth = 0;

        for( const wxString& label : { _( "EXTEST..." ), _( "EXTEST active..." ),
                                       _( "Take over..." ), _( "Forget..." ) } )
        {
            c.extest->SetLabel( label );
            c.extest->InvalidateBestSize();
            extestWidth = std::max( extestWidth, c.extest->GetBestSize().x );
        }

        c.extest->SetLabel( _( "EXTEST..." ) );
        c.extest->SetMinSize( wxSize( extestWidth, -1 ) );

        m_sizer->Add( c.tap, 0, wxALIGN_CENTER_VERTICAL );
        m_sizer->Add( c.ir, 0, wxALIGN_CENTER_VERTICAL | wxALIGN_RIGHT );
        m_sizer->Add( refSizer, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL );
        m_sizer->Add( bsdlSizer, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL );
        m_sizer->Add( c.pinMap, 0, wxALIGN_CENTER_VERTICAL );
        m_sizer->Add( c.check, 0, wxALIGN_CENTER_VERTICAL );
        m_sizer->Add( c.extest, 0, wxALIGN_CENTER_VERTICAL );

        c.extest->Bind( wxEVT_BUTTON,
                        [this, r]( wxCommandEvent& )
                        {
                            bool forget = r < static_cast<int>( m_rows.size() )
                                          && m_rows[r].forgettable;
                            send( forget ? EVT_CHAIN_FORGET : EVT_CHAIN_EXTEST, r );
                        } );

        // --- events; the row index is captured, controls are re-created when
        //     the rows change, so it stays valid
        c.reference->Bind( wxEVT_TEXT_ENTER, [this, r]( wxCommandEvent& ) { commitReference( r ); } );
        c.reference->Bind( wxEVT_KILL_FOCUS,
                           [this, r]( wxFocusEvent& aEvent )
                           {
                               commitReference( r );
                               aEvent.Skip();
                           } );

        c.take->Bind( wxEVT_BUTTON,
                      [this, r]( wxCommandEvent& ) { send( EVT_CHAIN_TAKE_SELECTION, r ); } );

        c.bsdl->Bind( wxEVT_TEXT_ENTER, [this, r]( wxCommandEvent& ) { commitBsdl( r ); } );
        c.bsdl->Bind( wxEVT_KILL_FOCUS,
                      [this, r]( wxFocusEvent& aEvent )
                      {
                          commitBsdl( r );
                          aEvent.Skip();
                      } );

        // "...": the matching files first if there are any, otherwise the file dialog
        c.browse->Bind( wxEVT_BUTTON,
                        [this, r]( wxCommandEvent& )
                        {
                            if( m_rows[r].suggestions > 0 )
                                send( EVT_CHAIN_SUGGEST, r );
                            else
                                BrowseFile( r );
                        } );

        c.pinMap->Bind( wxEVT_CHOICE,
                        [this, r]( wxCommandEvent& aEvent )
                        { send( EVT_CHAIN_PIN_MAP_CHANGED, r, aEvent.GetString() ); } );

        // Any control of the row taking the focus selects the row's details
        auto focus = [this, r]( wxFocusEvent& aEvent )
        {
            send( EVT_CHAIN_ROW_FOCUSED, r );
            aEvent.Skip();
        };

        for( wxWindow* w : std::initializer_list<wxWindow*>{ c.reference, c.take, c.pinMap } )
            w->Bind( wxEVT_SET_FOCUS, focus );

        for( wxWindow* w : std::initializer_list<wxWindow*>{ c.bsdl, c.browse } )
            w->Bind( wxEVT_SET_FOCUS, focus );

        update( r );
    }

    m_rebuilding = false;
    updateMinSize();
    Thaw();
    Layout();
    GetParent()->Layout();
}


void ChainTable::update( int aRow )
{
    const Row& row = m_rows[aRow];
    Controls&  c   = m_controls[aRow];

    wxColour fg = row.present ? wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOWTEXT )
                              : wxSystemSettings::GetColour( wxSYS_COLOUR_GRAYTEXT );
    c.tap->SetForegroundColour( fg );
    c.ir->SetLabel( row.ir );

    // Do not overwrite what the user is typing
    if( !c.reference->HasFocus() && c.reference->GetValue() != fromUtf8( row.reference ) )
        c.reference->ChangeValue( fromUtf8( row.reference ) );

    wxString path = fromUtf8( row.bsdl );

    if( !c.bsdl->HasFocus() && c.bsdl->GetValue() != path )
    {
        c.bsdl->ChangeValue( path );
        c.bsdl->SetInsertionPointEnd();   // show the file name of long paths
    }

    // Matching files: a hint in the empty field; "..." opens them
    bool suggest = row.suggestions > 0;

    c.bsdl->SetHint( !suggest ? wxString()
                     : row.bsdl.empty()
                             ? wxString::Format( wxPLURAL( "%d matching BSDL file found: click "
                                                           "\"...\"",
                                                           "%d matching BSDL files found: click "
                                                           "\"...\"",
                                                           row.suggestions ),
                                                 row.suggestions )
                             : wxString() );


    c.bsdl->SetToolTip( path.empty() ? _( "No BSDL file: the TAP stays in BYPASS. Type or paste "
                                          "a path, or use \"...\"." )
                                     : path + _( "\n(Clear the field to remove the assignment.)" ) );
    c.browse->SetToolTip( suggest ? _( "Choose one of the matching BSDL files (or another file)" )
                                  : _( "Choose a BSDL file" ) );

    // Pin maps: refill only if the list changed
    wxArrayString maps;

    for( const std::string& m : row.pinMaps )
        maps.Add( fromUtf8( m ) );

    if( c.pinMap->GetStrings() != maps )
        c.pinMap->Set( maps );

    c.pinMap->SetStringSelection( fromUtf8( row.pinMap ) );
    c.pinMap->Enable( maps.size() > 1 );

    c.extest->Enable( row.extestAllowed || row.extestActive || row.forgettable );
    c.extest->SetLabel( row.forgettable      ? _( "Forget..." )
                        : row.extestActive   ? _( "EXTEST active..." )
                        : row.extestTakeOver ? _( "Take over..." )
                                             : _( "EXTEST..." ) );
    c.extest->SetToolTip( row.forgettable
                                  ? _( "This TAP is not in OpenOCD's chain: remove its settings "
                                       "(reference, BSDL file, EXTEST starting states) from the "
                                       "project" )
                          : !row.extestNote.empty() ? row.extestNote
                          : row.extestActive
                                  ? _( "EXTEST is active: edit the starting states. Leave it "
                                       "with the red bar at the top." )
                          : row.extestTakeOver
                                  ? _( "The device is in EXTEST (from an earlier session or another "
                                       "program): continue controlling it, without returning the "
                                       "pins to the device's own logic in between" )
                                  : _( "Drive pins via boundary scan: choose the starting state "
                                       "of the output pins, review the changes, then activate. "
                                       "Can damage hardware if used carelessly." ) );

    c.check->SetLabel( row.check );
    c.check->SetForegroundColour( row.problem ? wxColour( 200, 40, 30 ) : fg );
    c.check->SetToolTip( row.details );
}


void ChainTable::commitReference( int aRow )
{
    if( m_rebuilding || aRow >= static_cast<int>( m_controls.size() ) )
        return;

    wxString    value = m_controls[aRow].reference->GetValue().Trim().Trim( false );
    std::string ref( value.utf8_str() );

    if( ref != m_rows[aRow].reference )
    {
        m_rows[aRow].reference = ref;   // avoid a second commit on focus loss
        send( EVT_CHAIN_REFERENCE_CHANGED, aRow, value );
    }
}


void ChainTable::commitBsdl( int aRow )
{
    if( m_rebuilding || aRow >= static_cast<int>( m_controls.size() ) )
        return;

    wxString    value = m_controls[aRow].bsdl->GetValue().Trim().Trim( false );
    std::string path( value.utf8_str() );

    Controls& c = m_controls[aRow];

    // Enter followed by focus loss: commit once
    if( path == m_rows[aRow].bsdl || ( c.submitted && path == *c.submitted ) )
        return;

    // An empty field removes the assignment (the TAP stays in BYPASS)
    c.submitted = path;
    chooseBsdl( aRow, path );
}


void ChainTable::BrowseFile( int aRow )
{
    if( aRow < 0 || aRow >= static_cast<int>( m_controls.size() ) )
        return;

    const std::string& current = m_rows[aRow].bsdl;
    wxString           dir     = current.empty() ? m_initialDir
                                                 : wxFileName( fromUtf8( current ) ).GetPath();
    wxString           name    = current.empty() ? wxString()
                                                 : wxFileName( fromUtf8( current ) ).GetFullName();

    wxFileDialog dlg( this, wxString::Format( _( "BSDL file for %s" ), fromUtf8( m_rows[aRow].tap ) ),
                      dir, name, BSDL_WILDCARD, wxFD_OPEN | wxFD_FILE_MUST_EXIST );

    if( dlg.ShowModal() != wxID_OK )
        return;

    // The same file again is passed on as well: it is re-read
    // (BSDL files are often regenerated by the FPGA tools)
    chooseBsdl( aRow, std::string( dlg.GetPath().utf8_str() ) );
}


void ChainTable::chooseBsdl( int aRow, const std::string& aPath )
{
    // The callback may show dialogs; run it after the current event
    CallAfter(
            [this, aRow, aPath]
            {
                if( aRow >= static_cast<int>( m_controls.size() ) )
                    return;

                bool ok = !OnBsdlChosen || OnBsdlChosen( aRow, aPath );

                if( aRow >= static_cast<int>( m_controls.size() ) )
                    return;

                m_controls[aRow].submitted.reset();

                // Refused: show the file that is still assigned
                if( !ok )
                    m_controls[aRow].bsdl->ChangeValue( fromUtf8( m_rows[aRow].bsdl ) );
            } );
}


void ChainTable::updateMinSize()
{
    // Height for the header and up to VISIBLE_ROWS rows; the rest scrolls.
    // Narrower than the columns need, the table scrolls sideways (so the
    // window can be narrow while the other tabs are shown); the height keeps
    // room for that scroll bar.
    wxSize best    = m_sizer->CalcMin();
    int    rows    = static_cast<int>( m_rows.size() ) + 1;
    int    visible = std::min( rows, VISIBLE_ROWS + 1 );
    int    height  = rows > 0 ? best.y * visible / rows : best.y;
    int    hscroll = wxSystemSettings::GetMetric( wxSYS_HSCROLL_Y, this );

    SetMinSize( wxSize( FromDIP( 480 ), height + std::max( hscroll, 0 ) + FromDIP( 4 ) ) );
    FitInside();
}


void ChainTable::send( wxEventType aType, int aRow, const wxString& aText )
{
    SendGuiEvent( this, aType, aRow, aText );
}
