// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/chain.h"

#include <cstdio>
#include <sstream>

namespace bscan
{

using openocd::Error;
using openocd::ErrorKind;


static std::optional<uint32_t> parseHex32( const std::string& aText )
{
    try
    {
        size_t             used = 0;
        unsigned long long v    = std::stoull( aText, &used, 16 );

        if( used != aText.size() || v > 0xffffffffULL )
            return std::nullopt;

        return static_cast<uint32_t>( v );
    }
    catch( ... )
    {
        return std::nullopt;
    }
}


// Table printed by OpenOCD 'scan_chain' (src/jtag/tcl.c):
//
//    TapName             Enabled  IdCode     Expected   IrLen IrCap IrMask
// -- ------------------- -------- ---------- ---------- ----- ----- ------
//  0 simchip.tap            Y     0x1b5c30ef 0x1b5c30ef     4 0x01  0x03
//
// Further expected IDs of a TAP follow on extra lines with only one column.
std::optional<std::vector<TapInfo>> ParseScanChain( const std::string& aText )
{
    std::vector<TapInfo> taps;
    std::istringstream   lines( aText );
    std::string          line;
    bool                 header = false;

    while( std::getline( lines, line ) )
    {
        if( line.find( "TapName" ) != std::string::npos )
        {
            header = true;
            continue;
        }

        std::istringstream f( line );
        std::string        idx, name, enabled, id, expected, irlen;

        if( !( f >> idx ) || idx.starts_with( "--" ) )
            continue;

        if( !( f >> name >> enabled >> id >> expected >> irlen ) )
        {
            // Continuation line: one more -expected-id of the previous TAP
            std::string extra = idx;

            if( !taps.empty() && extra.starts_with( "0x" ) )
            {
                if( extra.size() > 2 && extra[2] == '*' )
                    extra[2] = '0';

                if( auto v = parseHex32( extra ) )
                    taps.back().expectedIds.push_back( *v );
            }

            continue;
        }

        TapInfo t;

        try
        {
            t.index    = std::stoi( idx );
            t.irLength = std::stoi( irlen );
        }
        catch( ... )
        {
            return std::nullopt;
        }

        auto idv = parseHex32( id );

        if( !idv )
            return std::nullopt;

        t.name    = name;
        t.enabled = ( enabled == "Y" );
        t.idcode  = *idv;

        // "0x*" marks -ignore-version; "0x00000000" means no expected ID
        if( expected.size() > 2 && expected[2] == '*' )
        {
            expected[2]     = '0';
            t.ignoreVersion = true;
        }

        if( uint32_t e = parseHex32( expected ).value_or( 0 ) )
            t.expectedIds.push_back( e );

        taps.push_back( t );
    }

    if( !header )
        return std::nullopt;

    return taps;
}


bool TapInfo::MatchesExpected( uint32_t aIdcode ) const
{
    if( expectedIds.empty() )
        return true;

    uint32_t mask = ignoreVersion ? 0x0fffffffu : 0xffffffffu;

    for( uint32_t e : expectedIds )
        if( ( e & mask ) == ( aIdcode & mask ) )
            return true;

    return false;
}


Result<std::vector<TapInfo>> ReadChain( openocd::TclClient& aClient )
{
    auto text = aClient.Run( "scan_chain" );

    if( !text )
        return std::unexpected( text.error() );

    auto taps = ParseScanChain( *text );

    if( !taps )
        return std::unexpected( Error{ ErrorKind::Protocol, "cannot parse scan_chain output" } );

    // Current instruction per TAP (newer/patched OpenOCD only)
    auto hasCurIr = aClient.HasCommand( "jtag cur_ir" );

    if( !hasCurIr )
        return std::unexpected( hasCurIr.error() );

    for( TapInfo& t : *taps )
    {
        if( !*hasCurIr )
            break;   // this OpenOCD does not have it: leave all nullopt

        auto ir = ReadCurIr( aClient, t.name );

        if( ir )
            t.curIr = *ir;
        else if( ir.error().kind != ErrorKind::Command && ir.error().kind != ErrorKind::Protocol )
            return std::unexpected( ir.error() );
        // else: e.g. disabled TAP, leave nullopt
    }

    return *taps;
}


Result<std::optional<uint64_t>> ReadCurIr( openocd::TclClient& aClient, const std::string& aTap )
{
    auto has = aClient.HasCommand( "jtag cur_ir" );

    if( !has )
        return std::unexpected( has.error() );

    if( !*has )
        return std::optional<uint64_t>{};

    auto ir = aClient.Run( "jtag cur_ir " + aTap );

    if( !ir )
        return std::unexpected( ir.error() );

    std::string v = *ir;
    v.erase( 0, v.find_first_not_of( " \t\r\n" ) );
    v.erase( v.find_last_not_of( " \t\r\n" ) + 1 );

    try
    {
        size_t pos = 0;

        if( v.size() > 2 && v.size() <= 18 && ( v.starts_with( "0x" ) || v.starts_with( "0X" ) ) )
        {
            uint64_t value = std::stoull( v, &pos, 16 );

            if( pos == v.size() )
                return std::optional<uint64_t>( value );
        }
    }
    catch( const std::exception& )
    {
    }

    return std::unexpected( Error{ ErrorKind::Protocol, "unexpected 'jtag cur_ir' reply: " + v } );
}


Result<bool> BackgroundPolling( openocd::TclClient& aClient )
{
    auto names = aClient.Run( "target names" );

    if( !names )
        return std::unexpected( names.error() );

    if( names->find_first_not_of( " \t\r\n" ) == std::string::npos )
        return false;   // no target: nothing is polled

    // Prints "background polling: on|off" first; polling the current target
    // may fail afterwards (e.g. not examined), the line is in the reply anyway
    auto r = aClient.Run( "poll" );

    if( !r && r.error().kind != ErrorKind::Command )
        return std::unexpected( r.error() );

    const std::string& text = r ? *r : r.error().message;
    return text.find( "background polling: on" ) != std::string::npos;
}


static std::string hex32( uint32_t v )
{
    char buf[16];
    std::snprintf( buf, sizeof( buf ), "0x%08x", v );
    return buf;
}


TapCheck CheckTap( const TapInfo& aTap, const BsdlDevice& aDevice )
{
    TapCheck c;

    auto fail = [&]( std::string aMsg )
    {
        c.ok = false;
        c.messages.push_back( std::move( aMsg ) );
    };

    if( !aTap.enabled )
        fail( "TAP " + aTap.name + " is disabled in OpenOCD" );

    if( aDevice.IrLength() <= 0 )
        fail( "BSDL: INSTRUCTION_LENGTH missing" );
    else if( aDevice.IrLength() != aTap.irLength )
        fail( "IR length: OpenOCD " + std::to_string( aTap.irLength ) + ", BSDL "
              + std::to_string( aDevice.IrLength() ) );
    else if( aDevice.IrLength() > 64 )
        fail( "IR length > 64 bits is not supported by OpenOCD's irscan" );

    if( aTap.idcode == 0 )
    {
        c.messages.push_back( "device reported no IDCODE (not checked)" );
    }
    else
    {
        auto match = aDevice.IdcodeMatches( aTap.idcode );

        if( !match )
            c.messages.push_back( "BSDL has no IDCODE_REGISTER (not checked)" );
        else if( !*match )
            fail( "IDCODE: device " + hex32( aTap.idcode ) + ", BSDL "
                  + aDevice.IdcodePattern() );
        else
            c.messages.push_back( "IDCODE " + hex32( aTap.idcode ) + " matches" );

        if( !aTap.MatchesExpected( aTap.idcode ) )
        {
            c.messages.push_back( "note: " + hex32( aTap.idcode )
                                  + " is not among the -expected-id values of the OpenOCD "
                                    "configuration" );
        }
    }

    if( !aDevice.Opcode( "SAMPLE" ) )
        fail( "BSDL: no SAMPLE instruction" );

    if( aDevice.BoundaryLength() <= 0 )
        fail( "BSDL: boundary register length unknown" );

    return c;
}


// Tcl for one IR scan: run inside 'capture' (irscan reports a failing JTAG
// queue only through the OpenOCD log, with an empty result; capture turns
// that log text into the error message) and prefix errors with the command.
static std::string irStep( const TapInfo& aTap, uint64_t aOpcode )
{
    const std::string ir = "irscan " + aTap.name + " " + std::to_string( aOpcode );
    return "if {[catch {capture {" + ir + "}} _m]} {error \"" + ir + ": $_m\"}; ";
}


// Tcl for one DR scan of the boundary register; leaves the result in _m.
// drscan input needs the 0x prefix, otherwise OpenOCD parses decimal.
static std::string drStep( const TapInfo& aTap, const Bits& aData )
{
    const std::string dr = "drscan " + aTap.name + " " + std::to_string( aData.size() ) + " 0x"
                           + aData.ToHex();
    return "if {[catch {" + dr + "} _m]} {error \"drscan: $_m\"}; ";
}


// Tcl for selecting an instruction and scanning the boundary register; leaves
// the captured data in _m. With 'jtag scan' (newer/patched OpenOCD) both are
// one operation; with separate irscan/drscan, OpenOCD may run timer callbacks
// (target polling, IPDBG) in between, which can change the instruction.
// The instruction is always shifted (-force-ir): OpenOCD's record of it is
// wrong after a reset or power cycle of the board.
// "tap opcode bits 0xdata" of one TAP in a 'jtag scan'
static std::string scanGroup( const ScanTap& aTap )
{
    return aTap.tap + " " + std::to_string( aTap.opcode ) + " " + std::to_string( aTap.data.size() )
           + " 0x" + aTap.data.ToHex();
}


// Several TAPs are only possible with 'jtag scan'; the result in _m is then
// one captured value per TAP, aTap's first.
static std::string scanStep( bool aJtagScan, const TapInfo& aTap, uint64_t aOpcode,
                             const Bits& aData, const std::vector<ScanTap>& aAlong = {} )
{
    if( !aJtagScan )
        return irStep( aTap, aOpcode ) + drStep( aTap, aData );

    std::string scan = "jtag scan " + scanGroup( ScanTap{ aTap.name, aOpcode, aData } );

    for( const ScanTap& t : aAlong )
        scan += " " + scanGroup( t );

    scan += " -force-ir";
    return "if {[catch {" + scan + "} _m]} {error \"jtag scan: $_m\"}; ";
}


// Error unless 'jtag scan' is there or nothing has to be scanned along
static Result<bool> jtagScanFor( openocd::TclClient& aClient, const std::vector<ScanTap>& aAlong )
{
    auto jtagScan = aClient.HasCommand( "jtag scan" );

    if( !jtagScan )
        return std::unexpected( jtagScan.error() );

    if( !*jtagScan && !aAlong.empty() )
        return std::unexpected( Error{ ErrorKind::Command,
                                       "scanning several devices together needs OpenOCD's "
                                       "'jtag scan'" } );

    return *jtagScan;
}


static Result<std::string> runSteps( openocd::TclClient& aClient, const std::string& aSteps )
{
    auto reply = aClient.Run( aSteps + "set _m" );

    if( !reply )
    {
        Error e = reply.error();

        // e.g. "irscan xc7.tap 1: " -> OpenOCD gave no reason
        if( e.kind == ErrorKind::Command && !e.message.empty() && e.message.back() == ' ' )
            e.message += "(no details from OpenOCD; check its console output)";

        return std::unexpected( e );
    }

    return reply;
}


static Result<Bits> toBits( const Result<std::string>& aReply, size_t aLength )
{
    if( !aReply )
        return std::unexpected( aReply.error() );

    // With TAPs scanned along: "own other ..." (space separated); ours first
    std::string first = *aReply;
    first.erase( 0, first.find_first_not_of( " \t\r\n" ) );

    if( size_t end = first.find_first_of( " \t\r\n" ); end != std::string::npos )
        first.erase( end );

    auto bits = Bits::FromHex( first, aLength );

    if( !bits )
        return std::unexpected( Error{ ErrorKind::Protocol, "unexpected drscan reply: " + *aReply } );

    return *bits;
}


// Checks shared by all boundary operations; returns the boundary length
static Result<size_t> checkDevice( const TapInfo& aTap, const BsdlDevice& aDevice,
                                   std::initializer_list<const char*> aInstructions )
{
    int length = aDevice.BoundaryLength();

    if( length <= 0 )
        return std::unexpected( Error{ ErrorKind::Command, "BSDL: boundary length unknown" } );

    if( aDevice.IrLength() != aTap.irLength )
        return std::unexpected( Error{ ErrorKind::Command, "IR length of BSDL and TAP differ" } );

    for( const char* ins : aInstructions )
    {
        if( !aDevice.Opcode( ins ) )
            return std::unexpected( Error{ ErrorKind::Command,
                                           std::string( "BSDL: no " ) + ins + " instruction" } );
    }

    return static_cast<size_t>( length );
}


// PRELOAD if the BSDL defines it, otherwise SAMPLE (IEEE 1149.1-1990 had a
// combined SAMPLE/PRELOAD instruction)
static uint64_t preloadOpcode( const BsdlDevice& aDevice )
{
    if( auto op = aDevice.Opcode( "PRELOAD" ) )
        return *op;

    return *aDevice.Opcode( "SAMPLE" );
}


Result<Bits> Sample( openocd::TclClient& aClient, const TapInfo& aTap, const BsdlDevice& aDevice,
                     const std::vector<ScanTap>& aAlong )
{
    auto length = checkDevice( aTap, aDevice, { "SAMPLE" } );

    if( !length )
        return std::unexpected( length.error() );

    auto jtagScan = jtagScanFor( aClient, aAlong );

    if( !jtagScan )
        return std::unexpected( jtagScan.error() );

    return toBits( runSteps( aClient, scanStep( *jtagScan, aTap, *aDevice.Opcode( "SAMPLE" ),
                                                aDevice.SafeBits(), aAlong ) ),
                   *length );
}


Result<Bits> EnterExtest( openocd::TclClient& aClient, const TapInfo& aTap,
                          const BsdlDevice& aDevice, const Bits& aImage,
                          const std::vector<ScanTap>& aAlong )
{
    auto length = checkDevice( aTap, aDevice, { "SAMPLE", "EXTEST" } );

    if( !length )
        return std::unexpected( length.error() );

    if( aImage.size() != *length )
        return std::unexpected( Error{ ErrorKind::Command, "EXTEST image has the wrong length" } );

    auto jtagScan = jtagScanFor( aClient, aAlong );

    if( !jtagScan )
        return std::unexpected( jtagScan.error() );

    // Load the image into the update latches (PRELOAD), then switch to EXTEST
    // (the pins take the image at Update-IR) and shift the image again to
    // read the pins. Other JTAG activity between the two scans (OpenOCD timer
    // callbacks) does not change the latches: it scans another instruction's
    // register, or BYPASS on this TAP.
    return toBits( runSteps( aClient,
                             scanStep( *jtagScan, aTap, preloadOpcode( aDevice ), aImage, aAlong )
                                     + scanStep( *jtagScan, aTap, *aDevice.Opcode( "EXTEST" ),
                                                 aImage, aAlong ) ),
                   *length );
}


Result<Bits> ExtestScan( openocd::TclClient& aClient, const TapInfo& aTap,
                         const BsdlDevice& aDevice, const Bits& aImage,
                         const std::vector<ScanTap>& aAlong )
{
    auto length = checkDevice( aTap, aDevice, { "EXTEST" } );

    if( !length )
        return std::unexpected( length.error() );

    if( aImage.size() != *length )
        return std::unexpected( Error{ ErrorKind::Command, "EXTEST image has the wrong length" } );

    auto jtagScan = jtagScanFor( aClient, aAlong );

    if( !jtagScan )
        return std::unexpected( jtagScan.error() );

    // EXTEST is set again before each scan (another client may have changed
    // the IR); re-selecting EXTEST does not change the update latches.
    return toBits( runSteps( aClient, scanStep( *jtagScan, aTap, *aDevice.Opcode( "EXTEST" ),
                                                aImage, aAlong ) ),
                   *length );
}


Result<void> LeaveExtest( openocd::TclClient& aClient, const TapInfo& aTap,
                          const BsdlDevice& aDevice, const std::vector<ScanTap>& aAlong )
{
    auto length = checkDevice( aTap, aDevice, { "SAMPLE" } );

    if( !length )
        return std::unexpected( length.error() );

    auto jtagScan = jtagScanFor( aClient, aAlong );

    if( !jtagScan )
        return std::unexpected( jtagScan.error() );

    // Any instruction other than EXTEST returns the pins to the device's own
    // logic at Update-IR. SAMPLE keeps the boundary register observable.
    // With other devices kept in EXTEST, their image is shifted in the same
    // 'jtag scan' (which needs a DR scan: the safe values, under SAMPLE they
    // only reach the update latches).
    auto r = aAlong.empty()
                     ? runSteps( aClient, irStep( aTap, *aDevice.Opcode( "SAMPLE" ) ) )
                     : runSteps( aClient, scanStep( true, aTap, *aDevice.Opcode( "SAMPLE" ),
                                                    aDevice.SafeBits(), aAlong ) );

    if( !r )
        return std::unexpected( r.error() );

    return {};
}


Result<ScanTap> SampleScanTap( const TapInfo& aTap, const BsdlDevice& aDevice )
{
    if( auto length = checkDevice( aTap, aDevice, { "SAMPLE" } ); !length )
        return std::unexpected( length.error() );

    return ScanTap{ aTap.name, *aDevice.Opcode( "SAMPLE" ), aDevice.SafeBits() };
}


Result<ScanTap> ExtestScanTap( const TapInfo& aTap, const BsdlDevice& aDevice, const Bits& aImage )
{
    auto length = checkDevice( aTap, aDevice, { "EXTEST" } );

    if( !length )
        return std::unexpected( length.error() );

    if( aImage.size() != *length )
        return std::unexpected( Error{ ErrorKind::Command, "EXTEST image has the wrong length" } );

    return ScanTap{ aTap.name, *aDevice.Opcode( "EXTEST" ), aImage };
}


Result<std::vector<Bits>> ScanTogether( openocd::TclClient& aClient,
                                        const std::vector<ScanTap>& aTaps )
{
    if( aTaps.empty() )
        return std::vector<Bits>{};

    auto jtagScan = aClient.HasCommand( "jtag scan" );

    if( !jtagScan )
        return std::unexpected( jtagScan.error() );

    if( !*jtagScan )
        return std::unexpected( Error{ ErrorKind::Command, "OpenOCD has no 'jtag scan'" } );

    std::string scan = "jtag scan";

    for( const ScanTap& t : aTaps )
        scan += " " + scanGroup( t );

    scan += " -force-ir";

    auto reply = runSteps( aClient, "if {[catch {" + scan + "} _m]} {error \"jtag scan: $_m\"}; " );

    if( !reply )
        return std::unexpected( reply.error() );

    // One hex value per TAP, separated by a space, in the order given
    std::vector<Bits> out;
    size_t            pos = 0;

    for( const ScanTap& t : aTaps )
    {
        pos       = reply->find_first_not_of( " \t\r\n", pos );
        size_t end = pos == std::string::npos ? pos : reply->find_first_of( " \t\r\n", pos );
        auto   bits = pos == std::string::npos
                              ? std::nullopt
                              : Bits::FromHex( reply->substr( pos, end - pos ), t.data.size() );

        if( !bits )
            return std::unexpected( Error{ ErrorKind::Protocol,
                                           "unexpected jtag scan reply: " + *reply } );

        out.push_back( std::move( *bits ) );
        pos = end;
    }

    return out;
}


PinValue DecodePin( const Bits& aBoundary, const PinCells& aCells )
{
    auto bit = [&]( int aCell ) -> std::optional<bool>
    {
        if( aCell < 0 || static_cast<size_t>( aCell ) >= aBoundary.size() )
            return std::nullopt;

        return aBoundary[static_cast<size_t>( aCell )];
    };

    PinValue v;
    v.input = bit( aCells.inputCell );

    if( auto out = bit( aCells.outputCell ) )
    {
        auto ctrl = bit( aCells.controlCell );

        if( ctrl && aCells.disableValue >= 0 && *ctrl == ( aCells.disableValue == 1 ) )
            v.output = PinValue::Output::HighZ;
        else
            v.output = *out ? PinValue::Output::Drive1 : PinValue::Output::Drive0;
    }

    return v;
}


const char* ToString( PinValue::Output aOutput )
{
    switch( aOutput )
    {
    case PinValue::Output::None:   return "-";
    case PinValue::Output::HighZ:  return "Z";
    case PinValue::Output::Drive0: return "drives 0";
    case PinValue::Output::Drive1: return "drives 1";
    }

    return "?";
}

} // namespace bscan
