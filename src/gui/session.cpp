// SPDX-License-Identifier: GPL-3.0-only
#include "session.h"

#include <algorithm>
#include <set>

#include <wx/filename.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/stdpaths.h>

using bscan::BsdlDevice;
using bscan::LiveSampler;
using bscan::PinDrive;
using bscan::TapAssignment;
using bscan::TapInfo;

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


Session::Session( wxWindow* aParent, bscan::LiveSampler& aSampler ) :
        sampler( aSampler ),
        m_parent( aParent )
{
}


// ============================================================================
// Project file
// ============================================================================

bool Session::LoadProject( const kicad::BoardState& aState )
{
    if( aState.projectPath.empty() || aState.projectName.empty() )
        return false;   // keep the current settings (e.g. KiCad briefly unreachable)

    std::string file = bscan::ProjectConfigPath( aState.projectPath, aState.projectName );

    if( file == projectFile )
        return false;

    auto cfg = bscan::LoadProjectConfig( file );

    if( !cfg )
    {
        // Do not overwrite a file we could not read
        projectFile.clear();
        wxMessageBox( wxString::Format( _( "Could not read the project settings:\n%s\n\n"
                                           "Changes will not be saved." ),
                                        fromUtf8( cfg.error() ) ),
                      _( "Boundary Scan" ), wxOK | wxICON_WARNING, m_parent );
        return false;
    }

    projectFile       = file;
    settingsFromKiCad = true;
    config            = *cfg;
    m_devices.clear();
    m_deviceErrors.clear();
    return true;
}


std::string Session::DefaultSettingsFile()
{
    wxFileName file( wxStandardPaths::Get().GetUserConfigDir(),
                     "no-project.bscan.json" );
    file.AppendDir( "kicad-bscan" );
    return std::string( file.GetFullPath().utf8_str() );
}


bool Session::UseSettingsFile( const std::string& aFile )
{
    auto cfg = bscan::LoadProjectConfig( aFile );   // a missing file gives the defaults

    if( !cfg )
    {
        wxMessageBox( wxString::Format( _( "Could not read the settings:\n%s" ),
                                        fromUtf8( cfg.error() ) ),
                      _( "Boundary Scan" ), wxOK | wxICON_WARNING, m_parent );
        return false;
    }

    projectFile       = aFile;
    settingsFromKiCad = false;
    config            = *cfg;
    m_devices.clear();
    m_deviceErrors.clear();
    return true;
}


bool Session::SaveSettingsAs( const std::string& aFile )
{
    std::string previous = projectFile;
    projectFile          = aFile;
    settingsFromKiCad    = false;

    // BSDL paths are stored relative to the new file's folder where possible
    if( auto r = bscan::SaveProjectConfig( projectFile, config ); !r )
    {
        projectFile = previous;
        wxMessageBox( wxString::Format( _( "Could not save the settings:\n%s" ),
                                        fromUtf8( r.error() ) ),
                      _( "Boundary Scan" ), wxOK | wxICON_ERROR, m_parent );
        return false;
    }

    return true;
}


void Session::Save()
{
    if( projectFile.empty() )
    {
        if( OnNotSaved )
            OnNotSaved( _( "Not saved: KiCad did not report the project directory." ) );

        return;
    }

    // The default file's folder may not exist yet
    wxFileName::Mkdir( wxFileName( fromUtf8( projectFile ) ).GetPath(), wxS_DIR_DEFAULT,
                       wxPATH_MKDIR_FULL );

    if( auto r = bscan::SaveProjectConfig( projectFile, config ); !r )
    {
        wxMessageBox( wxString::Format( _( "Could not save the project settings:\n%s" ),
                                        fromUtf8( r.error() ) ),
                      _( "Boundary Scan" ), wxOK | wxICON_ERROR, m_parent );
    }
}


// ============================================================================
// Devices
// ============================================================================

void Session::ForgetDevice( const std::string& aPath )
{
    m_devices.erase( aPath );
    m_deviceErrors.erase( aPath );
}


const TapInfo* Session::TapInChain( const std::string& aTap ) const
{
    if( !chain.connected )
        return nullptr;

    for( const TapInfo& t : chain.taps )
        if( t.name == aTap )
            return &t;

    return nullptr;
}


wxString Session::DeviceLabel( const std::string& aTap ) const
{
    const TapAssignment* a = config.Find( aTap );
    return a ? DeviceLabel( *a ) : fromUtf8( aTap );
}


PinNets Session::NetsOf( const std::string& aReference ) const
{
    auto it = board.padNets.find( aReference );
    return it != board.padNets.end() ? it->second : PinNets();
}


bool Session::InExtest( const std::string& aTap ) const
{
    return extest.contains( aTap ) || extestPending.contains( aTap );
}


std::shared_ptr<const BsdlDevice> Session::DeviceFor( const std::string& aPath,
                                                      std::string*       aError )
{
    if( aPath.empty() )
        return nullptr;

    if( auto it = m_devices.find( aPath ); it != m_devices.end() )
        return it->second;

    if( auto it = m_deviceErrors.find( aPath ); it != m_deviceErrors.end() )
    {
        if( aError )
            *aError = it->second;

        return nullptr;
    }

    auto dev = BsdlDevice::Load( aPath );

    if( !dev )
    {
        m_deviceErrors[aPath] = dev.error();

        if( aError )
            *aError = dev.error();

        return nullptr;
    }

    m_devices[aPath] = *dev;
    return *dev;
}


TapAssignment& Session::AssignmentFor( const TapInfo& aTap )
{
    if( TapAssignment* a = config.Find( aTap.name ) )
        return *a;

    config.taps.push_back( TapAssignment{ aTap.name, aTap.idcode, "", "", "", {}, "" } );
    return config.taps.back();
}


wxString Session::DeviceLabel( const TapAssignment& aAssignment ) const
{
    return aAssignment.reference.empty()
                   ? fromUtf8( aAssignment.tap )
                   : wxString::Format( "%s (%s)", fromUtf8( aAssignment.reference ),
                                       fromUtf8( aAssignment.tap ) );
}


StartStates ToStartStates( const std::map<std::string, std::string>& aIn )
{
    std::map<std::string, PinDrive> out;

    for( const auto& [pin, mode] : aIn )
        if( auto d = bscan::PinDriveFromString( mode ); d && *d != PinDrive::Safe )
            out[pin] = *d;

    return out;
}


std::map<std::string, std::string> FromStartStates( const StartStates& aIn )
{
    std::map<std::string, std::string> out;

    for( const auto& [pin, d] : aIn )
        if( d != PinDrive::Safe )
            out[pin] = bscan::ToString( d );

    return out;
}


std::optional<std::string> Session::HardwarePinControl( const std::string& aTap )
{
    // Needs 'jtag cur_ir' (OpenOCD) and the BSDL file to name the instruction
    const TapInfo* tap = nullptr;

    if( chain.connected )
        for( const TapInfo& t : chain.taps )
            if( t.name == aTap )
                tap = &t;

    const TapAssignment* a = config.Find( aTap );

    if( !tap || !tap->curIr || !a || a->bsdl.empty() )
        return std::nullopt;

    auto dev = DeviceFor( a->bsdl );

    if( !dev || !bscan::CheckTap( *tap, *dev ).ok )
        return std::nullopt;

    return bscan::PinControlInstruction( *dev, *tap->curIr );
}


bool Session::IsHeld( const std::string& aTap )
{
    if( extest.contains( aTap ) || extestPending.contains( aTap ) )
        return false;   // our own EXTEST

    // Known from OpenOCD: in EXTEST (or CLAMP, HIGHZ, ...) or not
    if( auto hw = HardwarePinControl( aTap ) )
        return !hw->empty();

    // Otherwise: what this plugin recorded when it was closed
    return std::ranges::find( config.extestLeft, aTap ) != config.extestLeft.end();
}


std::vector<std::string> Session::HeldTaps()
{
    std::vector<std::string> taps;

    for( const std::string& t : config.extestLeft )
        if( IsHeld( t ) )
            taps.push_back( t );

    for( const TapInfo& t : chain.taps )
        if( std::ranges::find( taps, t.name ) == taps.end() && IsHeld( t.name ) )
            taps.push_back( t.name );

    return taps;
}


void Session::SyncExtestRecord()
{
    // Records that OpenOCD contradicts ('jtag cur_ir': not in EXTEST) are dropped
    size_t before = config.extestLeft.size();

    std::erase_if( config.extestLeft,
                   [this]( const std::string& t )
                   {
                       auto hw = HardwarePinControl( t );
                       return hw && hw->empty();
                   } );

    // Saved images are only useful while OpenOCD confirms EXTEST
    size_t images = std::erase_if( config.extestImages,
                                   [this]( const auto& aItem )
                                   {
                                       auto hw = HardwarePinControl( aItem.first );
                                       return hw && *hw != "EXTEST";
                                   } );

    if( config.extestLeft.size() != before || images )
        Save();
}


std::string Session::PinControlElsewhere( const std::string& aTap )
{
    // Active or being entered by this window. With 'jtag scan' these are
    // scanned along with every other scan and stay in EXTEST.
    if( !chain.jtagScan )
    {
        for( const auto* map : { &extest, &extestPending } )
            for( const auto& [tap, e] : *map )
                if( tap != aTap )
                    return tap;
    }

    // In EXTEST from an earlier session or another program
    for( const std::string& tap : HeldTaps() )
        if( tap != aTap )
            return tap;

    return {};
}


bool Session::CanTakeOver( const std::string& aTap )
{
    auto takeable = [this]( const std::string& aName )
    {
        if( extest.contains( aName ) || extestPending.contains( aName ) )
            return false;

        auto hw = HardwarePinControl( aName );
        return hw && *hw == "EXTEST";
    };

    if( !chain.connected || !takeable( aTap ) )
        return false;

    // Without 'jtag scan' only one device can be in EXTEST
    if( !chain.jtagScan )
        return PinControlElsewhere( aTap ).empty();

    // With it, the other held devices are taken over together: each of them
    // must be confirmed in EXTEST as well
    for( const std::string& other : TakeOverGroup( aTap ) )
        if( !takeable( other ) )
            return false;

    return true;
}


std::vector<std::string> Session::TakeOverGroup( const std::string& aTap )
{
    std::vector<std::string> group{ aTap };

    if( chain.jtagScan )
        for( const std::string& t : HeldTaps() )
            if( t != aTap )
                group.push_back( t );

    return group;
}


Session::ActiveExtest* Session::ActiveExtestFor( const std::string& aReference )
{
    const TapAssignment* a = config.FindByReference( aReference );

    if( !a )
        return nullptr;

    auto it = extest.find( a->tap );
    return it != extest.end() ? &it->second : nullptr;
}


wxString Session::ResolvePin( const std::string& aReference, const std::string& aPad,
                              LiveSampler::Target& aTarget, std::string& aIdentity )
{
    if( aReference.empty() )
        return _( "The footprint of this pad is unknown." );

    const TapAssignment* a = config.FindByReference( aReference );

    if( !a )
        return wxString::Format( _( "%s is not assigned to a TAP of the JTAG chain." ),
                                 fromUtf8( aReference ) );

    if( a->bsdl.empty() )
        return wxString::Format( _( "No BSDL file assigned to %s." ), fromUtf8( a->tap ) );

    if( IsHeld( a->tap ) )
        return wxString::Format( _( "Not sampled: %s is (probably) in EXTEST (see the bar "
                                    "at the top)." ),
                                 fromUtf8( a->tap ) );

    // Scanning this TAP would put the TAP in EXTEST into BYPASS
    if( std::string other = PinControlElsewhere( a->tap ); !other.empty() )
        return wxString::Format( _( "Paused while %s is in EXTEST (scanning another device "
                                    "would end it)." ),
                                 fromUtf8( other ) );

    std::string err;
    auto        dev = DeviceFor( a->bsdl, &err );

    if( !dev )
        return fromUtf8( err );

    const TapInfo* tap = nullptr;

    if( chain.connected )
        for( const TapInfo& t : chain.taps )
            if( t.name == a->tap )
                tap = &t;

    if( !tap )
    {
        return chain.connected ? wxString::Format( _( "TAP %s is not in the OpenOCD chain." ),
                                                   fromUtf8( a->tap ) )
                               : _( "OpenOCD is not connected." );
    }

    if( !bscan::CheckTap( *tap, *dev ).ok )
        return wxString::Format( _( "TAP %s and %s do not match (see JTAG chain)." ),
                                 fromUtf8( tap->name ), fromUtf8( dev->EntityName() ) );

    std::string mapName = a->pinMap.empty() ? dev->DefaultPinMap() : a->pinMap;
    auto        cells   = dev->LookupPin( a->pinMap, aPad );

    if( !cells )
        return wxString::Format( _( "Pad %s is not in pin map %s." ), fromUtf8( aPad ),
                                 fromUtf8( mapName ) );

    aTarget.tap    = *tap;
    aTarget.device = dev;
    aTarget.pin    = aPad;
    aTarget.cells  = *cells;

    if( !cells->HasCells() )
        return wxString::Format( _( "Port %s has no boundary-scan cells." ),
                                 fromUtf8( cells->port ) );

    aIdentity = tap->name + "|" + std::to_string( tap->idcode ) + "|" + a->bsdl + "|" + mapName
                + "|" + aPad;
    return wxEmptyString;
}


std::string Session::ExtestStartable( const std::string& aReference, const std::string& aPad )
{
    const TapAssignment* a = config.FindByReference( aReference );

    if( !a || extest.contains( a->tap ) || extestPending.contains( a->tap )
        || !PinControlElsewhere( a->tap ).empty() )
        return {};

    LiveSampler::Target t;
    std::string         id;

    // ResolvePin: present, BSDL matches, not left in EXTEST earlier
    if( !ResolvePin( aReference, aPad, t, id ).empty() || t.cells.outputCell < 0 || !t.device
        || !t.device->Opcode( "EXTEST" ) )
    {
        return {};
    }

    return a->tap;
}


std::vector<bscan::IcDevice> Session::InterconnectDevices( wxString*          aProblem,
                                                           const std::string& aEditedTap,
                                                           const StartStates* aEdited )
{
    std::vector<bscan::IcDevice> devices;

    for( const TapAssignment& a : config.taps )
    {
        if( a.bsdl.empty() || a.reference.empty() )
            continue;

        auto           dev = DeviceFor( a.bsdl );
        const TapInfo* tap = nullptr;

        if( chain.connected )
            for( const TapInfo& t : chain.taps )
                if( t.name == a.tap )
                    tap = &t;

        if( !dev || !tap || !bscan::CheckTap( *tap, *dev ).ok || !dev->Opcode( "EXTEST" ) )
            continue;   // its pins count as other parts

        bscan::IcDevice d;
        d.tap       = tap->name;
        d.reference = a.reference;
        d.device    = dev;
        d.pinMap    = a.pinMap;

        auto drives = a.tap == aEditedTap && aEdited ? *aEdited : ToStartStates( a.extestStart );

        for( const auto& [pin, drive] : drives )
            if( drive != PinDrive::Safe )
                d.constraints[pin] = drive;

        if( auto it = extest.find( tap->name ); it != extest.end() )
        {
            d.baseImage = it->second.image;   // what is driven now
            addCurrentStateConstraints( d, drives );
        }
        else
        {
            d.baseImage = bscan::PlanExtest( *dev, a.pinMap, {}, drives ).image;

            bool keep = std::ranges::any_of( drives, []( const auto& aItem )
                                             { return aItem.second == PinDrive::Keep; } );

            if( keep && aProblem && aProblem->empty() )
                *aProblem = wxString::Format( _( "%s has \"keep\" starting states, which need the "
                                                 "current state: start EXTEST on it first, or "
                                                 "change them." ),
                                              DeviceLabel( a ) );
        }

        devices.push_back( std::move( d ) );
    }

    return devices;
}


// Pins of a device in EXTEST that drive otherwise than their starting state
// were set by hand (drive buttons): they keep that state like a starting
// state, unless the user released them for the interconnect test.
void Session::addCurrentStateConstraints( bscan::IcDevice& aDevice, const StartStates& aStart )
{
    const bscan::Bits start =
            bscan::PlanExtest( *aDevice.device, aDevice.pinMap, {}, aStart ).image;

    for( const bscan::OutputPin& p : aDevice.device->OutputPins( aDevice.pinMap ) )
    {
        if( aDevice.constraints.contains( p.pin ) || icReleased.contains( { aDevice.tap, p.pin } ) )
            continue;

        bscan::PinState now = bscan::StateFromImage( aDevice.baseImage, p.cells );

        if( now == bscan::StateFromImage( start, p.cells ) )
            continue;

        std::optional<PinDrive> drive;

        switch( now )
        {
        case bscan::PinState::Low:
            drive = PinDrive::Low;
            break;
        case bscan::PinState::High:
            drive = PinDrive::High;
            break;
        case bscan::PinState::HighZ:
            drive = PinDrive::HighZ;
            break;
        default:
            break;
        }

        if( drive )
        {
            aDevice.constraints[p.pin] = *drive;
            aDevice.currentState.insert( p.pin );
        }
    }
}


TestRolesFunction Session::TestRolesFor( const std::string& aTap )
{
    return [this, aTap]( const StartStates& aStates )
    {
        std::set<std::string>   included( config.interconnectInclude.begin(),
                                          config.interconnectInclude.end() );
        bscan::InterconnectPlan plan = bscan::PlanInterconnect(
                InterconnectDevices( nullptr, aTap, &aStates ), board.padNets, included );

        for( size_t d = 0; d < plan.devices.size(); ++d )
            if( plan.devices[d].tap == aTap )
                return bscan::PinTestRoles( plan, static_cast<int>( d ) );

        return std::map<std::string, bscan::PinTestRole>{};   // not part of the test
    };
}


Session::ShownPad Session::SelectedPad() const
{
    ShownPad shown;

    if( pickedPin )
    {
        shown.status       = kicad::BoardState::Status::PadSelected;
        shown.pad          = *pickedPin;
        shown.footprintRef = pickedPin->reference;
        shown.fromChipView = true;
        return shown;
    }

    shown.status       = board.status;
    shown.pad          = board.pad;
    shown.padCount     = board.padCount;
    shown.footprintRef = board.footprintRef;
    shown.detail       = board.detail;
    return shown;
}


void Session::PickPins( const std::vector<PadKey>& aPads, const kicad::PadInfo& aPicked )
{
    pickedPin = aPicked;

    std::vector<PadKey> onBoard;

    for( const PadKey& p : aPads )
        if( auto it = board.padNets.find( p.first );
            it != board.padNets.end() && it->second.contains( p.second ) )
            onBoard.push_back( p );

    m_requestedSelection = std::set<PadKey>( onBoard.begin(), onBoard.end() );

    if( watcher && !onBoard.empty() )
        watcher->SelectPads( onBoard );
}


void Session::ClearPick()
{
    pickedPin.reset();
    m_requestedSelection.clear();

    if( watcher )
        watcher->SelectPads( {} );
}


bool Session::IsOwnSelection( const std::vector<kicad::PadInfo>& aPads )
{
    std::set<PadKey> pads;

    for( const kicad::PadInfo& p : aPads )
        pads.insert( { p.reference, p.number } );

    bool own = !m_requestedSelection.empty() && pads == m_requestedSelection;
    m_requestedSelection.clear();
    return own;
}
