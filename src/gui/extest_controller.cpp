// SPDX-License-Identifier: GPL-3.0-only
#include "extest_controller.h"

#include <algorithm>
#include <set>

#include <wx/datetime.h>
#include <wx/msgdlg.h>
#include <wx/window.h>

#include "extest_dialogs.h"

using bscan::LiveSampler;
using bscan::PinDrive;
using bscan::TapAssignment;
using bscan::TapInfo;

static wxString fromUtf8( const std::string& aText )
{
    return wxString::FromUTF8( aText.c_str() );
}


ExtestController::ExtestController( wxWindow* aParent, Session& aSession ) :
        m_parent( aParent ),
        m_session( aSession )
{
}


void ExtestController::changed()
{
    if( OnChanged )
        OnChanged();
}


// ============================================================================
// Setup, review, entering
// ============================================================================

void ExtestController::Setup( const std::string& aTap, const StartStates& aPreset,
                              const std::vector<std::string>& aFocus )
{
    const TapInfo* inChain = m_session.TapInChain( aTap );

    if( !inChain )
        return;

    const TapInfo  tap = *inChain;
    TapAssignment& a   = m_session.AssignmentFor( tap );
    m_preset           = aPreset;
    m_focus            = aFocus;
    auto dev           = m_session.DeviceFor( a.bsdl );

    if( !dev )
        return;

    if( m_session.extest.contains( tap.name ) )
    {
        openSetup( tap.name, {}, true );
        return;
    }

    // Held from an earlier session: take over (with 'jtag scan' together with
    // the other held devices), before the one-device rule below
    if( m_session.IsHeld( tap.name ) )
    {
        if( m_session.CanTakeOver( tap.name ) )
            TakeOver( tap.name );
        else
            wxMessageBox( _( "This device is in a state this window did not set. Taking over needs "
                             "OpenOCD to confirm EXTEST ('jtag cur_ir'); use \"Leave EXTEST\" in "
                             "the bar at the top." ),
                          _( "EXTEST" ), wxOK | wxICON_INFORMATION, m_parent );

        return;
    }

    if( std::string other = m_session.PinControlElsewhere( tap.name ); !other.empty() )
    {
        // With 'jtag scan' only devices held from an earlier session block
        wxMessageBox(
                m_session.chain.jtagScan
                        ? wxString::Format( _( "%s is in EXTEST, set before this window was "
                                               "opened or by another program. Take it over or "
                                               "leave EXTEST first (bar at the top)." ),
                                            fromUtf8( other ) )
                        : wxString::Format( _( "%s is in EXTEST. Only one device can be in "
                                               "EXTEST at a time: OpenOCD puts all other "
                                               "devices into BYPASS for every scan, which "
                                               "would end it." ),
                                            fromUtf8( other ) ),
                _( "EXTEST" ), wxOK | wxICON_INFORMATION, m_parent );
        return;
    }

    // Current state first (a few SAMPLE captures); the dialog opens when they arrive
    if( OnStatus )
        OnStatus( wxString::Format( _( "Reading the current state of %s..." ),
                                    fromUtf8( tap.name ) ) );
    m_session.sampler.RequestSnapshot(
            tap, dev, 5,
            [this, name = tap.name]( const bscan::Result<LiveSampler::Snapshot>& aRes )
            {
                std::vector<bscan::Bits> samples;
                std::string              error;

                if( aRes )
                    samples = aRes->samples;
                else
                    error = aRes.error().message;

                m_parent->CallAfter(
                        [this, name, samples, error]
                        {
                            if( !error.empty() )
                            {
                                m_preset.clear();
                                m_focus.clear();
                                wxMessageBox( wxString::Format( _( "Could not read the current "
                                                                   "state:\n%s" ),
                                                                fromUtf8( error ) ),
                                              _( "EXTEST" ), wxOK | wxICON_ERROR, m_parent );
                                return;
                            }

                            openSetup( name, samples, false );
                        } );
            } );
}


void ExtestController::openSetup( const std::string& aTap, const std::vector<bscan::Bits>& aSamples,
                                  bool aActive )
{
    TapAssignment* a = m_session.config.Find( aTap );

    if( !a )
        return;

    auto dev = m_session.DeviceFor( a->bsdl );

    if( !dev )
        return;

    PinNets nets;

    if( auto it = m_session.board.padNets.find( a->reference );
        it != m_session.board.padNets.end() )
        nets = it->second;

    // Pins the user clicked (0/1/Z) get that starting state in the dialog;
    // saved only if the dialog is confirmed
    auto settings = ToStartStates( a->extestStart );

    for( const auto& [pin, drive] : std::exchange( m_preset, {} ) )
        settings[pin] = drive;

    ExtestSetupDialog dlg( m_parent, m_session.DeviceLabel( *a ), dev, a->pinMap, settings, nets,
                           aSamples, aActive, std::exchange( m_focus, {} ),
                           m_session.TestRolesFor( aTap ) );

    if( dlg.ShowModal() != wxID_OK )
        return;

    a->extestStart = FromStartStates( dlg.GetSettings() );
    m_session.Save();
    // Starting states are the interconnect test's constraints
    if( OnStartStatesChanged )
        OnStartStatesChanged();

    if( dlg.WantsActivation() )
    {
        // Review with fresh captures, not the ones from when the dialog opened
        const TapInfo* tap = nullptr;

        for( const TapInfo& t : m_session.chain.taps )
            if( t.name == aTap )
                tap = &t;

        if( !tap )
            return;

        m_session.sampler.RequestSnapshot(
                *tap, dev, 5,
                [this, name = aTap]( const bscan::Result<LiveSampler::Snapshot>& aRes )
                {
                    std::vector<bscan::Bits> samples;
                    bool                     polling = false;
                    std::string              error;

                    if( aRes )
                    {
                        samples = aRes->samples;
                        polling = aRes->backgroundPolling;
                    }
                    else
                    {
                        error = aRes.error().message;
                    }

                    m_parent->CallAfter( [this, name, samples, polling, error]
                                         { confirm( name, samples, polling, error ); } );
                } );
    }
}


void ExtestController::confirm( const std::string& aTap, const std::vector<bscan::Bits>& aSamples,
                                bool aBackgroundPolling, const std::string& aError )
{
    if( !aError.empty() )
    {
        wxMessageBox( wxString::Format( _( "Could not read the current state:\n%s" ),
                                        fromUtf8( aError ) ),
                      _( "EXTEST" ), wxOK | wxICON_ERROR, m_parent );
        return;
    }

    const TapAssignment* a   = m_session.config.Find( aTap );
    auto                 dev = a ? m_session.DeviceFor( a->bsdl ) : nullptr;
    const TapInfo*       tap = nullptr;

    for( const TapInfo& t : m_session.chain.taps )
        if( t.name == aTap )
            tap = &t;

    if( !a || !dev || !tap || m_session.extest.contains( aTap )
        || m_session.extestPending.contains( aTap ) )
        return;

    PinNets nets;

    if( auto it = m_session.board.padNets.find( a->reference );
        it != m_session.board.padNets.end() )
        nets = it->second;

    auto              drives = ToStartStates( a->extestStart );
    bscan::ExtestPlan plan   = bscan::PlanExtest( *dev, a->pinMap, aSamples, drives );
    auto              stale  = bscan::StaleSettings( *dev, a->pinMap, drives );

    if( !ConfirmExtest( m_parent, m_session.DeviceLabel( *a ), plan, nets, stale, false,
                        aBackgroundPolling ) )
        return;

    m_session.extestPending[aTap] =
            Session::ActiveExtest{ *tap, dev, a->pinMap, plan.image, a->reference };
    m_session.sampler.EnterExtest( *tap, dev, plan.image );
    changed();
}


void ExtestController::WarnStaleSettings( const std::string& aTap )
{
    const TapAssignment* a   = m_session.config.Find( aTap );
    auto                 dev = a && !a->bsdl.empty() ? m_session.DeviceFor( a->bsdl ) : nullptr;

    if( !a || !dev || a->extestStart.empty() )
        return;

    auto stale = bscan::StaleSettings( *dev, a->pinMap, ToStartStates( a->extestStart ) );

    if( stale.empty() )
        return;

    wxMessageBox( wxString::Format( _( "The saved EXTEST starting states of %s include pins that "
                                       "are not output pins of this BSDL file / pin map:\n%s\n\n"
                                       "They are kept but ignored. Check or remove them in the "
                                       "EXTEST setup." ),
                                    m_session.DeviceLabel( *a ), PinList( stale ) ),
                  _( "EXTEST starting states" ), wxOK | wxICON_INFORMATION, m_parent );
}


// ============================================================================
// Updates from the sampler, leaving
// ============================================================================

void ExtestController::OnUpdate( const LiveSampler::ExtestUpdate& aUpdate )
{
    using State = LiveSampler::ExtestUpdate::State;

    switch( aUpdate.state )
    {
    case State::Active:
        if( auto it = m_session.extestPending.find( aUpdate.tap );
            it != m_session.extestPending.end() )
        {
            m_session.extest[aUpdate.tap] = it->second;
            m_session.extestPending.erase( it );
        }

        m_session.extestInterrupted.erase( aUpdate.tap );

        // Taken over (or entered): no longer an EXTEST of an earlier session
        if( std::erase( m_session.config.extestLeft, aUpdate.tap )
            + m_session.config.extestImages.erase( aUpdate.tap ) )
            m_session.Save();

        break;

    case State::Left:
        m_session.extest.erase( aUpdate.tap );
        std::erase_if( m_session.icReleased,
                       [&]( const auto& aItem ) { return aItem.first == aUpdate.tap; } );
        m_session.extestPending.erase( aUpdate.tap );
        m_session.extestInterrupted.erase( aUpdate.tap );

        if( std::erase( m_session.config.extestLeft, aUpdate.tap )
            + m_session.config.extestImages.erase( aUpdate.tap ) )
            m_session.Save();

        // The instruction OpenOCD reports ('jtag cur_ir') has changed
        m_session.sampler.RefreshChain();

        if( !aUpdate.message.empty() )
            wxMessageBox( fromUtf8( aUpdate.tap + ": " + aUpdate.message ), _( "EXTEST" ),
                          wxOK | wxICON_WARNING, m_parent );
        break;

    case State::Failed:
        if( m_session.IsHeld( aUpdate.tap ) && !m_session.extest.contains( aUpdate.tap ) )
        {
            m_session.extestNote = fromUtf8( aUpdate.message );   // leaving an old EXTEST, retrying
        }
        else if( m_session.extestPending.erase( aUpdate.tap )
                 || !m_session.extest.contains( aUpdate.tap ) )
        {
            wxMessageBox( fromUtf8( aUpdate.tap + ": " + aUpdate.message ), _( "EXTEST" ),
                          wxOK | wxICON_ERROR, m_parent );
        }
        else
        {
            m_session.extestNote = fromUtf8( aUpdate.message );   // e.g. leaving, retrying
        }
        break;

    case State::Interrupted:
    {
        const TapAssignment* a     = m_session.config.Find( aUpdate.tap );
        wxString             label = a ? m_session.DeviceLabel( *a ) : fromUtf8( aUpdate.tap );

        wxString time  = wxDateTime::Now().FormatISOTime();
        wxString found = fromUtf8( aUpdate.message );

        m_session.extestInterrupted[aUpdate.tap] =
                ( aUpdate.interruptions == 1
                          ? wxString::Format( _( "Warning: EXTEST on %s was interrupted once (%s, "
                                                 "found %s) and restored." ),
                                              label, time, found )
                          : wxString::Format( _( "Warning: EXTEST on %s was interrupted %d times "
                                                 "(last %s, found %s) and restored each time." ),
                                              label, aUpdate.interruptions, time, found ) )
                + " "
                + _( "Something else uses the JTAG chain: target polling ('poll off'), IPDBG or "
                     "another program." );
        break;
    }
    }

    if( aUpdate.state != State::Failed && aUpdate.state != State::Interrupted )
        m_session.extestNote.clear();

    changed();
}


void ExtestController::LeaveAll()
{
    for( const auto& [tap, e] : m_session.extest )
        m_session.sampler.LeaveExtest( tap );

    for( const auto& [tap, e] : m_session.extestPending )
        m_session.sampler.LeaveExtest( tap );

    // Left in EXTEST by an earlier session: leave explicitly (SAMPLE)
    for( const std::string& name : m_session.HeldTaps() )
    {
        const TapAssignment* a   = m_session.config.Find( name );
        auto                 dev = a ? m_session.DeviceFor( a->bsdl ) : nullptr;
        const TapInfo*       tap = nullptr;

        for( const TapInfo& t : m_session.chain.taps )
            if( t.name == name )
                tap = &t;

        if( tap && dev && m_session.chain.connected )
        {
            m_session.sampler.ForceLeaveExtest( *tap, dev );
        }
        else if( !m_session.chain.connected )
        {
            wxMessageBox( _( "OpenOCD is not connected; EXTEST can be left once it is." ),
                          _( "EXTEST" ), wxOK | wxICON_INFORMATION, m_parent );
            return;
        }
        else
        {
            // Not in the chain or no BSDL file: nothing this plugin can do
            std::erase( m_session.config.extestLeft, name );
            m_session.Save();
        }
    }

    m_session.extestNote = _( "Leaving EXTEST..." );
    changed();
}


// ============================================================================
// Taking over
// ============================================================================

void ExtestController::TakeOverAnyHeld()
{
    for( const std::string& tap : m_session.HeldTaps() )
    {
        if( m_session.CanTakeOver( tap ) )
        {
            TakeOver( tap );   // all held, with 'jtag scan'
            return;
        }
    }
}


std::optional<bscan::Bits> ExtestController::chooseTakeOverImage( const std::string& aTap )
{
    const TapAssignment* a   = m_session.config.Find( aTap );
    auto                 dev = a ? m_session.DeviceFor( a->bsdl ) : nullptr;

    if( !a || !dev )
        return std::nullopt;

    const wxString label = m_session.DeviceLabel( *a );
    PinNets        nets;

    if( auto it = m_session.board.padNets.find( a->reference );
        it != m_session.board.padNets.end() )
        nets = it->second;

    // A) the image saved when the window was closed, if it fits this BSDL file
    if( auto it = m_session.config.extestImages.find( aTap );
        it != m_session.config.extestImages.end() )
    {
        const bscan::SavedExtestImage saved = it->second;   // copy: dialogs run an event loop
        auto                          bits  = bscan::Bits::FromBitString( saved.bits );

        if( bits && saved.bsdl == a->bsdl && saved.pinMap == a->pinMap
            && static_cast<int>( bits->size() ) == dev->BoundaryLength() )
        {
            switch( ConfirmTakeOver( m_parent, label, *dev, a->pinMap, *bits, nets,
                                     fromUtf8( saved.saved ) ) )
            {
            case TakeOverChoice::Cancel:
                return std::nullopt;
            case TakeOverChoice::SavedImage:
                return *bits;
            case TakeOverChoice::NewPlan:
                break;
            }
        }
    }

    // B) a new plan: setup, then review (the current outputs are unknown)
    const std::string pinMap = a->pinMap;   // a may move: dialogs run an event loop
    ExtestSetupDialog dlg( m_parent, label, dev, pinMap, ToStartStates( a->extestStart ), nets, {},
                           false, {}, m_session.TestRolesFor( aTap ) );

    if( dlg.ShowModal() != wxID_OK )
        return std::nullopt;

    if( TapAssignment* edit = m_session.config.Find( aTap ) )
    {
        edit->extestStart = FromStartStates( dlg.GetSettings() );
        m_session.Save();
    }

    if( !dlg.WantsActivation() )
        return std::nullopt;

    auto              drives = dlg.GetSettings();
    bscan::ExtestPlan plan   = bscan::PlanExtest( *dev, pinMap, {}, drives );
    auto              stale  = bscan::StaleSettings( *dev, pinMap, drives );

    if( !ConfirmExtest( m_parent, label, plan, nets, stale, true ) )
        return std::nullopt;

    return plan.image;
}


void ExtestController::TakeOver( const std::string& aTap )
{
    const std::vector<std::string> group = m_session.TakeOverGroup( aTap );

    if( group.size() > 1 )
    {
        wxString others;

        for( size_t i = 1; i < group.size(); ++i )
        {
            const TapAssignment* a = m_session.config.Find( group[i] );
            others << ( others.empty() ? "" : ", " )
                   << ( a ? m_session.DeviceLabel( *a ) : fromUtf8( group[i] ) );
        }

        if( wxMessageBox(
                    wxString::Format( group.size() == 2
                                              ? _( "%s is in EXTEST as well. Both devices are "
                                                   "taken over together (taking over one alone "
                                                   "would end EXTEST on the other): choose the "
                                                   "pin states for each device in turn." )
                                              : _( "%s are in EXTEST as well. All of them are "
                                                   "taken over together (taking over one alone "
                                                   "would end EXTEST on the others): choose the "
                                                   "pin states for each device in turn." ),
                                      others ),
                    _( "Take over EXTEST" ), wxOK | wxCANCEL | wxICON_INFORMATION, m_parent )
            != wxOK )
            return;
    }

    std::vector<LiveSampler::TakeOver> takeOvers;

    for( const std::string& name : group )
    {
        auto image = chooseTakeOverImage( name );

        if( !image )
            return;   // cancelled: none is taken over

        const TapAssignment* a   = m_session.config.Find( name );
        auto                 dev = a ? m_session.DeviceFor( a->bsdl ) : nullptr;
        const TapInfo*       tap = nullptr;

        for( const TapInfo& t : m_session.chain.taps )
            if( t.name == name )
                tap = &t;

        if( !a || !dev || !tap )
            return;

        takeOvers.push_back( { *tap, dev, *image } );
    }

    // Still in EXTEST and not taken over meanwhile? (the sampler checks again)
    if( !m_session.CanTakeOver( aTap ) )
        return;

    for( const LiveSampler::TakeOver& t : takeOvers )
    {
        const TapAssignment* a = m_session.config.Find( t.tap.name );
        m_session.extestPending[t.tap.name] =
                Session::ActiveExtest{ t.tap, t.device, a ? a->pinMap : std::string(), t.image,
                                       a ? a->reference : std::string() };
    }

    if( takeOvers.size() == 1 )
        m_session.sampler.TakeOverExtest( takeOvers[0].tap, takeOvers[0].device,
                                          takeOvers[0].image );
    else
        m_session.sampler.TakeOverExtest( takeOvers );

    changed();
}


// ============================================================================
// Driving pins
// ============================================================================

// Up to 20 entries, then "and N more"
static wxString shortList( const std::vector<wxString>& aList )
{
    wxString text;

    for( size_t i = 0; i < aList.size() && i < 20; ++i )
        text << ( i ? ", " : "" ) << aList[i];

    if( aList.size() > 20 )
        text << wxString::Format( _( " and %zu more" ), aList.size() - 20 );

    return text;
}


static wxString skippedText( const bscan::DrivePlan::Skipped& aSkipped )
{
    using Reason = bscan::DrivePlan::SkipReason;

    switch( aSkipped.reason )
    {
    case Reason::NotInExtest:
        return fromUtf8( aSkipped.name ) + _( " (EXTEST not active)" );
    case Reason::NoOutputEnable:
        return fromUtf8( aSkipped.name ) + _( " (no output enable)" );
    case Reason::NoOutput:
        return fromUtf8( aSkipped.name ) + _( " (no output)" );
    }

    return fromUtf8( aSkipped.name );
}


void ExtestController::DrivePins( const std::vector<bscan::WatchEntry>& aPins, PinDrive aDrive )
{
    // None of the devices in EXTEST yet: start it via setup and review
    if( std::none_of( aPins.begin(), aPins.end(), [this]( const bscan::WatchEntry& w )
                      { return m_session.ActiveExtestFor( w.reference ) != nullptr; } ) )
    {
        startWith( aPins, aDrive );
        return;
    }

    // What would change (nothing is applied before the user confirmed)
    std::map<std::string, bscan::DriveDevice> devices;
    std::vector<bscan::DriveRequest>          requests;

    for( const auto& [tap, e] : m_session.extest )
        devices[tap] = { e.device.get(), e.pinMap, e.image,
                         e.reference.empty() ? tap : e.reference };

    for( const bscan::WatchEntry& w : aPins )
    {
        const Session::ActiveExtest* e = m_session.ActiveExtestFor( w.reference );
        requests.push_back( { e ? e->tap.name : std::string(), w.pad, w.reference + " " + w.pad } );
    }

    bscan::DrivePlan plan = bscan::PlanDrive( devices, requests, aDrive );

    std::vector<wxString> skipped, affected;

    for( const bscan::DrivePlan::Skipped& s : plan.skipped )
        skipped.push_back( skippedText( s ) );

    for( const std::string& a : plan.alsoAffected )
        affected.push_back( fromUtf8( a ) );

    if( plan.applied == 0 )
    {
        wxMessageBox( wxString::Format( _( "No pin can be set to %s:\n%s" ), DriveLabel( aDrive ),
                                        shortList( skipped ) ),
                      _( "EXTEST" ), wxOK | wxICON_INFORMATION, m_parent );
        return;
    }

    if( !confirmDrive( plan.applied, aDrive, affected, skipped ) )
        return;

    for( const auto& [tap, image] : plan.images )
    {
        if( auto it = m_session.extest.find( tap ); it != m_session.extest.end() )
        {
            it->second.image = image;
            m_session.sampler.UpdateExtest( tap, image );
        }
    }
}


// Asks only if something is unexpected: side effects or skipped pins
bool ExtestController::confirmDrive( size_t aApplied, PinDrive aDrive,
                                     const std::vector<wxString>& aAffected,
                                     const std::vector<wxString>& aSkipped )
{
    if( aSkipped.empty() && aAffected.empty() )
        return true;

    wxString msg = wxString::Format( _( "Set %zu pin(s) to %s?" ), aApplied, DriveLabel( aDrive ) );

    if( !aAffected.empty() )
        msg << "\n\n"
            << wxString::Format( _( "These pins share an output enable and change their output "
                                    "state too:\n%s" ),
                                 shortList( aAffected ) );

    if( !aSkipped.empty() )
        msg << "\n\n" << wxString::Format( _( "Skipped:\n%s" ), shortList( aSkipped ) );

    return wxMessageBox( msg, _( "EXTEST" ), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, m_parent )
           == wxYES;
}


void ExtestController::startWith( const std::vector<bscan::WatchEntry>& aPins, PinDrive aDrive )
{
    std::set<std::string>           taps;
    std::map<std::string, PinDrive> preset;
    std::vector<std::string>        focus;

    for( const bscan::WatchEntry& w : aPins )
    {
        if( std::string tap = m_session.ExtestStartable( w.reference, w.pad ); !tap.empty() )
        {
            taps.insert( tap );
            preset[w.pad] = aDrive;
            focus.push_back( w.pad );
        }
    }

    if( taps.empty() )
    {
        wxMessageBox( _( "None of these pins can be driven: no output cell, or the device is not "
                         "ready for EXTEST (see the JTAG chain)." ),
                      _( "EXTEST" ), wxOK | wxICON_INFORMATION, m_parent );
        return;
    }

    if( taps.size() > 1 )
    {
        wxMessageBox( _( "These pins belong to several devices. EXTEST is set up per device: "
                         "select pins of one device." ),
                      _( "EXTEST" ), wxOK | wxICON_INFORMATION, m_parent );
        return;
    }

    Setup( *taps.begin(), preset, focus );
}


// ============================================================================
// Closing the window
// ============================================================================

bool ExtestController::ConfirmClose( bool aCanVeto )
{
    Session& s = m_session;

    if( s.extest.empty() && s.extestPending.empty() )
        return true;

    int answer =
            wxMessageBox( _( "EXTEST is active. Return the device(s) to normal operation "
                             "before closing?\n\nNo: the pins keep the EXTEST state after "
                             "the window is closed." ),
                          _( "Boundary Scan" ), wxYES_NO | wxCANCEL | wxICON_WARNING, m_parent );

    if( answer == wxCANCEL && aCanVeto )
        return false;

    s.sampler.SetLeaveExtestOnStop( answer != wxNO );

    if( answer != wxNO )
        return true;

    // The next session must not end EXTEST silently by sampling
    for( const auto* map : { &s.extest, &s.extestPending } )
        for( const auto& [tap, e] : *map )
            if( !s.IsHeld( tap ) )
                s.config.extestLeft.push_back( tap );

    // What is driven, for "Take over" in the next session
    for( const auto& [tap, e] : s.extest )
    {
        if( const TapAssignment* a = s.config.Find( tap ) )
            s.config.extestImages[tap] = {
                a->bsdl, e.pinMap, e.image.ToBitString(),
                std::string( wxDateTime::Now().Format( "%Y-%m-%d %H:%M" ).utf8_str() )
            };
    }

    s.Save();
    return true;
}
