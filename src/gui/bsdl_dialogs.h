// SPDX-License-Identifier: GPL-3.0-only
// Dialogs for finding BSDL files:
//
//  BsdlFoldersDialog - the folders searched for BSDL files: the user's own
//                      (all projects) and the project's (in the project file)
//  ChooseBsdl()      - pick one of the files whose IDCODE matches a TAP

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <wx/dialog.h>

#include "bscan/bsdl_index.h"

class wxListBox;

class BsdlFoldersDialog : public wxDialog
{
public:
    // aProjectDir empty: no project known, project folders cannot be edited
    BsdlFoldersDialog( wxWindow* aParent, const std::vector<std::string>& aUserDirs,
                       const std::vector<std::string>& aProjectDirs,
                       const std::string& aProjectDir, const wxString& aStatus );

    std::vector<std::string> GetUserDirs() const;
    std::vector<std::string> GetProjectDirs() const;

private:
    wxListBox* addList( wxSizer* aSizer, const wxString& aTitle, const wxString& aHelp,
                        const std::vector<std::string>& aDirs, bool aEnabled );

    std::string m_projectDir;
    wxListBox*  m_user = nullptr;
    wxListBox*  m_project = nullptr;
};

struct BsdlChoice
{
    enum class Kind
    {
        Cancel,
        Chosen,      // suggestion is set
        OtherFile    // the user wants the file dialog
    };

    Kind                  kind = Kind::Cancel;
    bscan::BsdlSuggestion suggestion;
};

BsdlChoice ChooseBsdl( wxWindow* aParent, const wxString& aTapLabel, uint32_t aIdcode,
                       const std::vector<bscan::BsdlSuggestion>& aList );
