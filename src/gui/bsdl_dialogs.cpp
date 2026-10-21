// SPDX-License-Identifier: GPL-3.0-only
#include "bsdl_dialogs.h"

#include <wx/button.h>
#include <wx/dirdlg.h>
#include <wx/listbox.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


static std::vector<std::string> itemsOf( const wxListBox* aList )
{
    std::vector<std::string> out;

    for( unsigned i = 0; aList && i < aList->GetCount(); ++i )
        out.emplace_back( aList->GetString( i ).utf8_str() );

    return out;
}


// ============================================================================
// Folders
// ============================================================================

BsdlFoldersDialog::BsdlFoldersDialog( wxWindow* aParent, const std::vector<std::string>& aUserDirs,
                                      const std::vector<std::string>& aProjectDirs,
                                      const std::string& aProjectDir, const wxString& aStatus ) :
        wxDialog( aParent, wxID_ANY, _( "BSDL folders" ), wxDefaultPosition, wxDefaultSize,
                  wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER ),
        m_projectDir( aProjectDir )
{
    auto* top = new wxBoxSizer( wxVERTICAL );

    auto* intro = new wxStaticText(
            this, wxID_ANY,
            _( "These folders (and their subfolders) are searched for *.bsd, *.bsdl and *.bsm "
               "files. For a TAP without BSDL file, files whose IDCODE and IR length match are "
               "offered in the chain table; you always choose." ) );
    intro->Wrap( FromDIP( 520 ) );
    top->Add( intro, 0, wxALL, FromDIP( 8 ) );

    m_user = addList( top, _( "Your folders (all projects)" ),
                      _( "E.g. your collection of vendor BSDL files." ), aUserDirs, true );

    m_project = addList( top, _( "Project folders (stored in the project file)" ),
                         aProjectDir.empty()
                                 ? _( "No KiCad project known: project folders cannot be set." )
                                 : _( "E.g. the folder the FPGA tool writes its BSDL file to. "
                                      "Files from these folders are listed first." ),
                         aProjectDirs, !aProjectDir.empty() );

    if( !aStatus.empty() )
        top->Add( new wxStaticText( this, wxID_ANY, aStatus ), 0, wxLEFT | wxRIGHT | wxTOP,
                  FromDIP( 8 ) );

    auto* buttons = CreateStdDialogButtonSizer( wxOK | wxCANCEL );
    top->Add( buttons, 0, wxEXPAND | wxALL, FromDIP( 8 ) );

    SetSizerAndFit( top );
}


wxListBox* BsdlFoldersDialog::addList( wxSizer* aSizer, const wxString& aTitle,
                                       const wxString& aHelp, const std::vector<std::string>& aDirs,
                                       bool aEnabled )
{
    auto* title = new wxStaticText( this, wxID_ANY, aTitle );
    title->SetFont( title->GetFont().Bold() );
    aSizer->Add( title, 0, wxLEFT | wxRIGHT | wxTOP, FromDIP( 8 ) );
    aSizer->Add( new wxStaticText( this, wxID_ANY, aHelp ), 0, wxLEFT | wxRIGHT, FromDIP( 8 ) );

    auto* row  = new wxBoxSizer( wxHORIZONTAL );
    auto* list = new wxListBox( this, wxID_ANY, wxDefaultPosition,
                                wxSize( FromDIP( 420 ), FromDIP( 90 ) ) );

    for( const std::string& d : aDirs )
        list->Append( fromUtf8( d ) );

    auto* col    = new wxBoxSizer( wxVERTICAL );
    auto* add    = new wxButton( this, wxID_ANY, _( "Add..." ) );
    auto* remove = new wxButton( this, wxID_ANY, _( "Remove" ) );
    col->Add( add, 0, wxEXPAND | wxBOTTOM, FromDIP( 4 ) );
    col->Add( remove, 0, wxEXPAND );

    row->Add( list, 1, wxEXPAND );
    row->Add( col, 0, wxLEFT, FromDIP( 8 ) );
    aSizer->Add( row, 1, wxEXPAND | wxALL, FromDIP( 8 ) );

    list->Enable( aEnabled );
    add->Enable( aEnabled );
    remove->Enable( aEnabled );

    add->Bind( wxEVT_BUTTON,
               [this, list]( wxCommandEvent& )
               {
                   wxString start = list == m_project ? fromUtf8( m_projectDir ) : wxString();
                   wxDirDialog dlg( this, _( "Folder with BSDL files" ), start,
                                    wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST );

                   if( dlg.ShowModal() == wxID_OK && list->FindString( dlg.GetPath() ) == wxNOT_FOUND )
                       list->Append( dlg.GetPath() );
               } );

    remove->Bind( wxEVT_BUTTON,
                  [list]( wxCommandEvent& )
                  {
                      if( int sel = list->GetSelection(); sel != wxNOT_FOUND )
                          list->Delete( sel );
                  } );

    return list;
}


std::vector<std::string> BsdlFoldersDialog::GetUserDirs() const
{
    return itemsOf( m_user );
}


std::vector<std::string> BsdlFoldersDialog::GetProjectDirs() const
{
    return itemsOf( m_project );
}


// ============================================================================
// Choose a matching file
// ============================================================================

BsdlChoice ChooseBsdl( wxWindow* aParent, const wxString& aTapLabel, uint32_t aIdcode,
                       const std::vector<bscan::BsdlSuggestion>& aList )
{
    wxDialog dlg( aParent, wxID_ANY,
                  wxString::Format( _( "BSDL file for %s" ), aTapLabel ), wxDefaultPosition,
                  wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER );

    auto* top   = new wxBoxSizer( wxVERTICAL );
    auto* intro = new wxStaticText(
            &dlg, wxID_ANY,
            wxString::Format( _( "These BSDL files match IDCODE 0x%08x and the IR length. One "
                                 "die is often available in several packages: choose the file "
                                 "and pin map of the package on this board.\n"
                                 "\"Pads found\": pads of the footprint that exist in the pin "
                                 "map (best fit first)." ),
                              aIdcode ) );
    intro->Wrap( dlg.FromDIP( 640 ) );
    top->Add( intro, 0, wxALL, dlg.FromDIP( 8 ) );

    auto* list = new wxListView( &dlg, wxID_ANY, wxDefaultPosition,
                                 wxSize( dlg.FromDIP( 700 ), dlg.FromDIP( 220 ) ),
                                 wxLC_REPORT | wxLC_SINGLE_SEL | wxLC_HRULES );
    list->AppendColumn( _( "Device" ) );
    list->AppendColumn( _( "Pin map" ) );
    list->AppendColumn( _( "Pads found" ) );
    list->AppendColumn( _( "Pins" ) );
    list->AppendColumn( _( "Folder" ) );
    list->AppendColumn( _( "File" ) );

    for( size_t i = 0; i < aList.size(); ++i )
    {
        const bscan::BsdlSuggestion& s = aList[i];
        long                          n = static_cast<long>( i );

        list->InsertItem( n, fromUtf8( s.entity ) );
        list->SetItem( n, 1, s.pinMap.empty() ? wxString::FromUTF8( "–" ) : fromUtf8( s.pinMap ) );
        list->SetItem( n, 2, s.padsTotal ? wxString::Format( "%zu / %zu", s.padsFound, s.padsTotal )
                                         : _( "(no footprint)" ) );
        list->SetItem( n, 3, wxString::Format( "%zu", s.mapPins ) );
        list->SetItem( n, 4, s.preferred ? _( "project" ) : _( "yours" ) );
        list->SetItem( n, 5, fromUtf8( s.path ) );
    }

    for( int c = 0; c < list->GetColumnCount(); ++c )
        list->SetColumnWidth( c, wxLIST_AUTOSIZE_USEHEADER );

    if( !aList.empty() )
        list->Select( 0 );

    top->Add( list, 1, wxEXPAND | wxLEFT | wxRIGHT, dlg.FromDIP( 8 ) );

    auto* buttons = new wxBoxSizer( wxHORIZONTAL );
    auto* other   = new wxButton( &dlg, wxID_ANY, _( "Other file..." ) );
    auto* use     = new wxButton( &dlg, wxID_OK, _( "Use this file" ) );
    auto* cancel  = new wxButton( &dlg, wxID_CANCEL );
    other->SetToolTip( _( "Choose a BSDL file in the file dialog instead" ) );
    other->Bind( wxEVT_BUTTON, [&dlg]( wxCommandEvent& ) { dlg.EndModal( wxID_OPEN ); } );
    buttons->Add( other, 0 );
    buttons->AddStretchSpacer();
    buttons->Add( use, 0, wxRIGHT, dlg.FromDIP( 4 ) );
    buttons->Add( cancel, 0 );
    top->Add( buttons, 0, wxEXPAND | wxALL, dlg.FromDIP( 8 ) );

    list->Bind( wxEVT_LIST_ITEM_ACTIVATED, [&dlg]( wxListEvent& ) { dlg.EndModal( wxID_OK ); } );
    list->Bind( wxEVT_LIST_ITEM_SELECTED, [use]( wxListEvent& ) { use->Enable(); } );
    list->Bind( wxEVT_LIST_ITEM_DESELECTED, [use]( wxListEvent& ) { use->Disable(); } );

    dlg.SetSizerAndFit( top );
    use->SetDefault();

    BsdlChoice choice;
    int        result = dlg.ShowModal();

    if( result == wxID_OPEN )
    {
        choice.kind = BsdlChoice::Kind::OtherFile;
        return choice;
    }

    long sel = list->GetFirstSelected();

    if( result != wxID_OK || sel < 0 || sel >= static_cast<long>( aList.size() ) )
        return choice;

    choice.kind       = BsdlChoice::Kind::Chosen;
    choice.suggestion = aList[sel];
    return choice;
}
