// SPDX-License-Identifier: GPL-3.0-only
// Dialogs for EXTEST:
//
//  ExtestSetupDialog  - starting state per output pin (safe / Z / 0 / 1 / keep),
//                       with net names and the current state from SAMPLE
//  ConfirmExtest()    - the mandatory review before activation: every output
//                       pin whose state changes, conflicts block activation

#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <wx/dialog.h>

#include "bscan/extest.h"
#include "bscan/interconnect.h"

class wxButton;
class wxCheckBox;
class wxDataViewEvent;
class wxDataViewListCtrl;
class wxStaticText;

using PinNets = std::map<std::string, std::string>;   // pad -> net

// Starting states of one device: pin -> state (Safe: not listed)
using StartStates = std::map<std::string, bscan::PinDrive>;

// What the interconnect test would do with the device's pins if it had
// these starting states (pad -> role); empty function: not shown
using TestRolesFunction =
        std::function<std::map<std::string, bscan::PinTestRole>( const StartStates& )>;

class wxSearchCtrl;

class ExtestSetupDialog : public wxDialog
{
public:
    ExtestSetupDialog( wxWindow* aParent, const wxString& aDeviceLabel,
                       std::shared_ptr<const bscan::BsdlDevice> aDevice,
                       const std::string& aPinMap,
                       const std::map<std::string, bscan::PinDrive>& aSettings,
                       const PinNets& aNets, const std::vector<bscan::Bits>& aSamples,
                       bool aExtestActive, const std::vector<std::string>& aFocusPins = {},
                       TestRolesFunction aTestRoles = {} );

    const std::map<std::string, bscan::PinDrive>& GetSettings() const { return m_settings; }

    // True if the dialog was closed with "Review and activate..."
    bool WantsActivation() const { return m_activate; }

private:
    void fill();
    void updatePlan();
    void updateTestRoles();
    void onValueChanged( wxDataViewEvent& aEvent );
    void updateStale();

    std::shared_ptr<const bscan::BsdlDevice> m_device;
    std::string                              m_pinMap;
    std::map<std::string, bscan::PinDrive>   m_settings;
    PinNets                                  m_nets;
    std::vector<bscan::Bits>                 m_samples;
    std::vector<bscan::OutputPin>            m_pins;
    std::vector<size_t>                      m_rowPin;   // list row -> index in m_pins
    bool                                     m_activate = false;
    TestRolesFunction                        m_testRoles;

    wxDataViewListCtrl* m_list = nullptr;
    wxCheckBox*         m_onlyNets = nullptr;
    wxCheckBox*         m_onlyChanges = nullptr;   // pins whose state changes when EXTEST starts
    wxSearchCtrl*       m_search = nullptr;        // text in pin, port or net
    wxStaticText*       m_summary = nullptr;
    wxStaticText*       m_testSummary = nullptr;
    wxStaticText*       m_staleText = nullptr;
    wxButton*           m_staleRemove = nullptr;
};

// Shows the changes EXTEST would make and asks for confirmation.
// Returns true only if the user explicitly confirmed and there are no conflicts.
// aStale: saved settings for pins that are not output pins (ignored).
// aTakeOver: the device is already in EXTEST, the current outputs cannot be
// read; all pins are listed with the state they will get.
bool ConfirmExtest( wxWindow* aParent, const wxString& aDeviceLabel,
                    const bscan::ExtestPlan& aPlan, const PinNets& aNets,
                    const std::vector<std::string>& aStale = {}, bool aTakeOver = false,
                    bool aBackgroundPolling = false );

// "Take over" with the image saved when the window was closed: shows the
// states it drives. Returns what the user chose.
enum class TakeOverChoice
{
    Cancel,
    SavedImage,   // continue driving the saved image
    NewPlan       // go through setup and review instead
};

TakeOverChoice ConfirmTakeOver( wxWindow* aParent, const wxString& aDeviceLabel,
                                const bscan::BsdlDevice& aDevice, const std::string& aPinMap,
                                const bscan::Bits& aImage, const PinNets& aNets,
                                const wxString& aSavedWhen );

// "H4, H5, J7" (at most aMax names, then "and N more")
wxString PinList( const std::vector<std::string>& aPins, size_t aMax = 12 );

// Display texts
wxString DriveLabel( bscan::PinDrive aDrive );
wxString StateLabel( bscan::PinState aState );
