// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/extest.h"

#include <cctype>
#include <cstdio>
#include <set>
#include <stdexcept>

namespace bscan
{

const char* ToString( PinDrive aDrive )
{
    switch( aDrive )
    {
    case PinDrive::Safe:  return "safe";
    case PinDrive::HighZ: return "z";
    case PinDrive::Low:   return "0";
    case PinDrive::High:  return "1";
    case PinDrive::Keep:  return "keep";
    }

    return "safe";
}


std::optional<PinDrive> PinDriveFromString( const std::string& aText )
{
    for( PinDrive d : { PinDrive::Safe, PinDrive::HighZ, PinDrive::Low, PinDrive::High,
                        PinDrive::Keep } )
    {
        if( aText == ToString( d ) )
            return d;
    }

    return std::nullopt;
}


const char* ToString( PinState aState )
{
    switch( aState )
    {
    case PinState::Unknown:  return "unknown";
    case PinState::HighZ:    return "off (high-Z)";
    case PinState::Low:      return "drives 0";
    case PinState::High:     return "drives 1";
    case PinState::Toggling: return "toggling";
    }

    return "?";
}


std::vector<OutputPin> OutputPins( const BsdlDevice& aDevice, const std::string& aPinMap )
{
    return aDevice.OutputPins( aPinMap );
}


static bool bitAt( const Bits& aBits, int aCell )
{
    return aCell >= 0 && static_cast<size_t>( aCell ) < aBits.size()
           && aBits[static_cast<size_t>( aCell )];
}


static void setBit( Bits& aBits, int aCell, bool aValue )
{
    if( aCell >= 0 && static_cast<size_t>( aCell ) < aBits.size() )
        aBits.set( static_cast<size_t>( aCell ), aValue );
}


static PinState fromOutput( PinValue::Output aOut )
{
    switch( aOut )
    {
    case PinValue::Output::HighZ:  return PinState::HighZ;
    case PinValue::Output::Drive0: return PinState::Low;
    case PinValue::Output::Drive1: return PinState::High;
    case PinValue::Output::None:   return PinState::Unknown;
    }

    return PinState::Unknown;
}


PinState StateFromImage( const Bits& aImage, const PinCells& aCells )
{
    return fromOutput( DecodePin( aImage, aCells ).output );
}


// The value of the control cell that enables the output
static bool enableValue( const PinCells& aCells )
{
    return aCells.disableValue == 0;   // disable 0 -> enable 1; disable 1 -> enable 0
}


bool IsPinControlInstruction( const std::string& aName )
{
    std::string n;

    for( char c : aName )
        n += static_cast<char>( std::toupper( static_cast<unsigned char>( c ) ) );

    return n == "EXTEST" || n == "EXTEST_PULSE" || n == "EXTEST_TRAIN" || n == "CLAMP"
           || n == "HIGHZ" || n == "INTEST";
}


std::string PinControlInstruction( const BsdlDevice& aDevice, uint64_t aCurIr )
{
    for( const std::string& name : aDevice.InstructionsFor( aCurIr ) )
        if( IsPinControlInstruction( name ) )
            return name;

    return {};
}


std::string DescribeInstruction( const BsdlDevice& aDevice, uint64_t aValue )
{
    char hex[24];
    std::snprintf( hex, sizeof( hex ), "0x%llx", static_cast<unsigned long long>( aValue ) );

    std::string names;

    for( const std::string& n : aDevice.InstructionsFor( aValue ) )
        names += ( names.empty() ? "" : "/" ) + n;

    return names.empty() ? std::string( hex ) : names + " (" + hex + ")";
}


Result<Bits> TakeOverExtest( openocd::TclClient& aClient, const TapInfo& aTap,
                             const BsdlDevice& aDevice, const Bits& aImage,
                             const std::vector<ScanTap>& aAlong )
{
    using openocd::Error;
    using openocd::ErrorKind;

    auto ir = ReadCurIr( aClient, aTap.name );

    if( !ir )
        return std::unexpected( ir.error() );

    if( !*ir )
        return std::unexpected( Error{ ErrorKind::Command,
                                       "OpenOCD cannot report the current instruction "
                                       "('jtag cur_ir'): taking over is not safe" } );

    if( PinControlInstruction( aDevice, **ir ) != "EXTEST" )
    {
        return std::unexpected( Error{ ErrorKind::Command,
                                       "the device is not in EXTEST (current instruction "
                                               + DescribeInstruction( aDevice, **ir )
                                               + "): nothing to take over" } );
    }

    return ExtestScan( aClient, aTap, aDevice, aImage, aAlong );
}


std::vector<std::string> StaleSettings( const BsdlDevice& aDevice, const std::string& aPinMap,
                                        const std::map<std::string, PinDrive>& aSettings )
{
    std::set<std::string> outputs;

    for( const OutputPin& p : OutputPins( aDevice, aPinMap ) )
        outputs.insert( p.pin );

    std::vector<std::string> stale;

    for( const auto& [pin, drive] : aSettings )
        if( !outputs.contains( pin ) )
            stale.push_back( pin );   // std::map: already sorted

    return stale;
}


ExtestPlan PlanExtest( const BsdlDevice& aDevice, const std::string& aPinMap,
                       const std::vector<Bits>& aSamples,
                       const std::map<std::string, PinDrive>& aSettings )
{
    ExtestPlan plan;
    plan.image = aDevice.SafeBits();

    std::vector<OutputPin> pins = OutputPins( aDevice, aPinMap );

    // Control cell -> (required value, pin that requires it)
    std::map<int, std::pair<bool, std::string>> controlReq;

    auto requireControl = [&]( const OutputPin& aPin, bool aValue )
    {
        int  cell = aPin.cells.controlCell;
        auto it   = controlReq.find( cell );

        if( it != controlReq.end() && it->second.first != aValue )
        {
            plan.conflicts.push_back( "pins " + it->second.second + " and " + aPin.pin
                                      + " share control cell " + std::to_string( cell )
                                      + " but need different output enables" );
            return;
        }

        controlReq[cell] = { aValue, aPin.pin };
        setBit( plan.image, cell, aValue );
    };

    for( const OutputPin& p : pins )
    {
        auto     it    = aSettings.find( p.pin );
        PinDrive drive = it != aSettings.end() ? it->second : PinDrive::Safe;
        bool     hasCtrl = p.cells.controlCell >= 0 && p.cells.disableValue >= 0;

        switch( drive )
        {
        case PinDrive::Safe:
            break;

        case PinDrive::HighZ:
            if( !hasCtrl )
                plan.conflicts.push_back( "pin " + p.pin + " has no output enable and cannot be Z" );
            else
                requireControl( p, !enableValue( p.cells ) );
            break;

        case PinDrive::Low:
        case PinDrive::High:
            if( hasCtrl )
                requireControl( p, enableValue( p.cells ) );

            setBit( plan.image, p.cells.outputCell, drive == PinDrive::High );
            break;

        case PinDrive::Keep:
            if( aSamples.empty() )
            {
                plan.conflicts.push_back( "pin " + p.pin + ": 'keep' needs a SAMPLE first" );
                break;
            }

            if( hasCtrl )
                requireControl( p, bitAt( aSamples.back(), p.cells.controlCell ) );

            setBit( plan.image, p.cells.outputCell, bitAt( aSamples.back(), p.cells.outputCell ) );
            break;
        }
    }

    // Planned and current state of every output pin
    for( const OutputPin& p : pins )
    {
        PinPlan pp;
        pp.pin  = p.pin;
        pp.port = p.cells.port;

        if( auto it = aSettings.find( p.pin ); it != aSettings.end() )
        {
            pp.drive           = it->second;
            pp.explicitSetting = it->second != PinDrive::Safe;
        }

        pp.planned = StateFromImage( plan.image, p.cells );

        for( size_t i = 0; i < aSamples.size(); ++i )
        {
            PinState s = fromOutput( DecodePin( aSamples[i], p.cells ).output );

            if( i == 0 )
                pp.current = s;
            else if( s != pp.current )
                pp.current = PinState::Toggling;
        }

        plan.pins.push_back( std::move( pp ) );
    }

    return plan;
}


std::optional<PinChange> SetPin( const BsdlDevice& aDevice, const std::string& aPinMap,
                                 const Bits& aImage, const std::string& aPin, PinDrive aDrive )
{
    if( aDrive != PinDrive::HighZ && aDrive != PinDrive::Low && aDrive != PinDrive::High )
        return std::nullopt;

    auto cells = aDevice.LookupPin( aPinMap, aPin );

    if( !cells || cells->outputCell < 0 )
        return std::nullopt;

    bool hasCtrl = cells->controlCell >= 0 && cells->disableValue >= 0;

    if( aDrive == PinDrive::HighZ && !hasCtrl )
        return std::nullopt;

    PinChange change;
    change.image = aImage;

    if( aDrive == PinDrive::HighZ )
    {
        setBit( change.image, cells->controlCell, !enableValue( *cells ) );
    }
    else
    {
        if( hasCtrl )
            setBit( change.image, cells->controlCell, enableValue( *cells ) );

        setBit( change.image, cells->outputCell, aDrive == PinDrive::High );
    }

    // Other pins whose output state changes because of a shared control cell
    if( hasCtrl && bitAt( change.image, cells->controlCell )
                           != bitAt( aImage, cells->controlCell ) )
    {
        for( const OutputPin& p : aDevice.PinsWithControlCell( aPinMap, cells->controlCell ) )
        {
            if( p.pin != aPin
                && StateFromImage( change.image, p.cells ) != StateFromImage( aImage, p.cells ) )
            {
                change.alsoAffected.push_back( p.pin );
            }
        }
    }

    return change;
}


PinValue DecodeExtest( const Bits& aCaptured, const Bits& aImage, const PinCells& aCells )
{
    PinValue v = DecodePin( aImage, aCells );          // output: what is driven
    v.input    = DecodePin( aCaptured, aCells ).input; // input: what the pin reads
    return v;
}

DrivePlan PlanDrive( const std::map<std::string, DriveDevice>& aDevices,
                     const std::vector<DriveRequest>& aPins, PinDrive aDrive )
{
    DrivePlan             plan;
    std::set<std::string> requested;   // "tap|pin"
    std::set<std::string> affected;

    for( const DriveRequest& r : aPins )
        if( !r.tap.empty() )
            requested.insert( r.tap + "|" + r.pin );

    for( const DriveRequest& r : aPins )
    {
        auto dev = aDevices.find( r.tap );

        if( r.tap.empty() || dev == aDevices.end() || !dev->second.device )
        {
            plan.skipped.push_back( { r.name, DrivePlan::SkipReason::NotInExtest } );
            continue;
        }

        const DriveDevice& d      = dev->second;
        Bits&              image  = plan.images.try_emplace( r.tap, d.image ).first->second;
        auto               change = SetPin( *d.device, d.pinMap, image, r.pin, aDrive );

        if( !change )
        {
            plan.skipped.push_back( { r.name, aDrive == PinDrive::HighZ
                                                      ? DrivePlan::SkipReason::NoOutputEnable
                                                      : DrivePlan::SkipReason::NoOutput } );
            continue;
        }

        image = change->image;
        ++plan.applied;

        for( const std::string& o : change->alsoAffected )
        {
            std::string key = r.tap + "|" + o;

            if( !requested.contains( key ) && affected.insert( key ).second )
                plan.alsoAffected.push_back( d.label + " " + o );
        }
    }

    // Devices where every requested pin was skipped keep their image
    std::erase_if( plan.images, [&]( const auto& aItem )
                   { return aItem.second == aDevices.at( aItem.first ).image; } );
    return plan;
}

} // namespace bscan

