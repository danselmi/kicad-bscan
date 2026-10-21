// SPDX-License-Identifier: GPL-3.0-only
// Unit tests for the boundary-scan logic (no OpenOCD needed).

#include <algorithm>
#include <cstdio>
#include <string>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "bscan/bsdl_index.h"
#include "bscan/chain.h"
#include "bscan/extest.h"
#include "bscan/interconnect.h"
#include "bscan/interconnect_session.h"
#include "bscan/project_config.h"
#include "bscan/tap_status.h"

using namespace bscan;

static int g_failed = 0;

#define CHECK( cond )                                                                      \
    do                                                                                     \
    {                                                                                      \
        if( !( cond ) )                                                                    \
        {                                                                                  \
            std::printf( "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond );                  \
            ++g_failed;                                                                    \
        }                                                                                  \
    } while( 0 )


static void testBits()
{
    auto b = Bits::FromHex( "2e", 8 );
    CHECK( b && b->size() == 8 );
    CHECK( b && !( *b )[0] && ( *b )[1] && ( *b )[2] && ( *b )[3] && !( *b )[4] && ( *b )[5] );
    CHECK( b && b->ToHex() == "2e" );

    // OpenOCD pads to whole bytes: 10 bits -> 4 hex digits
    auto c = Bits::FromHex( "0201", 10 );
    CHECK( c && ( *c )[0] && ( *c )[9] && c->ToHex() == "201" );

    CHECK( !Bits::FromHex( "400", 10 ) );    // bit 10 set: too wide
    CHECK( !Bits::FromHex( "zz", 8 ) );
    CHECK( !Bits::FromHex( "", 8 ) );
    CHECK( Bits::FromHex( "0x2e\n", 8 ) == Bits::FromHex( "2e", 8 ) );

    auto s = Bits::FromBitString( "10X1", false );
    CHECK( s && s->ToU64() == 0b1001 );
    CHECK( Bits::FromBitString( "10X1", true )->ToU64() == 0b1011 );
    CHECK( !Bits::FromBitString( "102", false ) );
}


// Captured from OpenOCD 21f88d78 with tests/sim/openocd-sim.cfg
static const char* SCAN_CHAIN =
        "   TapName             Enabled  IdCode     Expected   IrLen IrCap IrMask\n"
        "-- ------------------- -------- ---------- ---------- ----- ----- ------\n"
        " 0 simchip.tap            Y     0x1b5c30ef 0x1b5c30ef     4 0x01  0x03\n"
        " 1 other.tap              Y     0x4ba00477 0x4ba00477     5 0x01  0x03\n"
        "                                           0x5ba00477\n";

static void testScanChain()
{
    auto taps = ParseScanChain( SCAN_CHAIN );
    CHECK( taps && taps->size() == 2 );

    if( !taps || taps->size() != 2 )
        return;

    CHECK( ( *taps )[0].name == "simchip.tap" && ( *taps )[0].irLength == 4 );
    CHECK( ( *taps )[0].idcode == 0x1b5c30ef && ( *taps )[0].enabled );
    CHECK( ( *taps )[1].name == "other.tap" && ( *taps )[1].irLength == 5 );
    CHECK( ( *taps )[1].index == 1 && ( *taps )[1].expectedIds.size() == 2 );
    CHECK( ( *taps )[1].MatchesExpected( 0x5ba00477 ) && !( *taps )[1].MatchesExpected( 0x6ba00477 ) );
    CHECK( ( *taps )[0].expectedIds.size() == 1 );

    // -ignore-version: "0x*" and version bits masked; format as printed by OpenOCD
    const char* xc7 =
            "   TapName             Enabled  IdCode     Expected   IrLen IrCap IrMask\n"
            "-- ------------------- -------- ---------- ---------- ----- ----- ------\n"
            " 0 xc7.tap                Y     0x037c4093 0x*3622093     6 0x01  0x03\n"
            "                                           0x*3620093\n"
            "                                           0x*37c4093\n";
    auto x = ParseScanChain( xc7 );
    CHECK( x && x->size() == 1 );

    if( x && x->size() == 1 )
    {
        const TapInfo& t = ( *x )[0];
        CHECK( t.ignoreVersion && t.expectedIds.size() == 3 );
        CHECK( t.MatchesExpected( 0x037c4093 ) );
        CHECK( t.MatchesExpected( 0x737c4093 ) );    // other version
        CHECK( !t.MatchesExpected( 0x037c5093 ) );
    }

    CHECK( !ParseScanChain( "garbage" ) );
    CHECK( ParseScanChain( "   TapName  Enabled\n" )->empty() );
}


static void testBsdl()
{
    auto dev = BsdlDevice::Load( SIMCHIP_BSD );
    CHECK( dev.has_value() );

    if( !dev )
    {
        std::printf( "  %s\n", dev.error().c_str() );
        return;
    }

    const BsdlDevice& d = **dev;

    for( const auto& g : d.Diagnostics() )
        std::printf( "  diag line %d: %s\n", g.line, g.message.c_str() );

    CHECK( d.EntityName() == "SIMCHIP" );
    CHECK( d.IrLength() == 4 );
    CHECK( d.BoundaryLength() == 8 );
    CHECK( d.Opcode( "SAMPLE" ) == 1u );
    CHECK( d.Opcode( "sample" ) == 1u );
    CHECK( d.Opcode( "EXTEST" ) == 0u );
    CHECK( !d.Opcode( "HIGHZ" ) );
    CHECK( d.IdcodeMatches( 0x1b5c30ef ) == true );
    CHECK( d.IdcodeMatches( 0x2b5c30ef ) == false );     // other version: not masked here
    CHECK( d.IdcodeMatches( 0x1b5c30ee ) == false );
    CHECK( d.DefaultPinMap() == "QFN16" );
    CHECK( d.PinMapNames().size() == 2 );
    CHECK( d.SafeBits().ToHex() == "00" );

    // QFN16 (default)
    auto p1 = d.LookupPin( "", "1" );
    CHECK( p1 && p1->port == "IN0" && p1->inputCell == 0 && p1->outputCell == -1 );

    auto p3 = d.LookupPin( "QFN16", "3" );
    CHECK( p3 && p3->port == "OUT0" && p3->outputCell == 3 && p3->controlCell == 2
           && p3->disableValue == 0 );

    auto p4 = d.LookupPin( "", "4" );
    CHECK( p4 && p4->port == "IO0" && p4->inputCell == 5 && p4->outputCell == 5
           && p4->controlCell == 4 );

    auto vcc = d.LookupPin( "", "13" );
    CHECK( vcc && vcc->port == "VCC(0)" && !vcc->HasCells() );

    CHECK( !d.LookupPin( "", "99" ) );
    CHECK( !d.LookupPin( "NOPE", "1" ) );

    // BGA9: ball "A1" is port IN0, the port named "A1" sits on ball C3
    auto a1 = d.LookupPin( "BGA9", "A1" );
    CHECK( a1 && a1->port == "IN0" && a1->inputCell == 0 );

    auto c3 = d.LookupPin( "bga9", "c3" );
    CHECK( c3 && c3->port == "A1" && c3->inputCell == 6 );

    // Decoding the capture "2e" read from the simulator:
    // IN0=0 IN1=1 OUT0 enabled driving 1, IO0 disabled reading 1 (pull-up), A1=0
    auto bits = *Bits::FromHex( "2e", 8 );

    PinValue in0 = DecodePin( bits, *p1 );
    CHECK( in0.input == false && in0.output == PinValue::Output::None );

    PinValue out0 = DecodePin( bits, *p3 );
    CHECK( !out0.input && out0.output == PinValue::Output::Drive1 );

    PinValue io0 = DecodePin( bits, *p4 );
    CHECK( io0.input == true && io0.output == PinValue::Output::HighZ );

    // Output enabled (control cell 4 = 1), IO0 cell = 0
    auto bits2 = *Bits::FromHex( "10", 8 );
    PinValue io0b = DecodePin( bits2, *p4 );
    CHECK( io0b.input == false && io0b.output == PinValue::Output::Drive0 );

    // Tap check
    TapInfo tap{ 0, "simchip.tap", true, 0x1b5c30ef, 4, { 0x1b5c30ef }, false };
    CHECK( CheckTap( tap, d ).ok );

    TapInfo wrongIr = tap;
    wrongIr.irLength = 5;
    CHECK( !CheckTap( wrongIr, d ).ok );

    TapInfo wrongId = tap;
    wrongId.idcode = 0x4ba00477;
    CHECK( !CheckTap( wrongId, d ).ok );

    TapInfo noId = tap;
    noId.idcode = 0;
    CHECK( CheckTap( noId, d ).ok );

    CHECK( !BsdlDevice::Load( "/nonexistent.bsd" ) );
}


static void testProjectConfig()
{
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path() / "bscan-test-project";
    fs::remove_all( dir );
    fs::create_directories( dir / "bsdl" );

    std::string file = ProjectConfigPath( dir.string(), "demo" );
    CHECK( file == ( dir / "demo.bscan.json" ).string() );

    // Missing file: defaults
    auto empty = LoadProjectConfig( file );
    CHECK( empty && empty->host == "127.0.0.1" && empty->port == 6666 && empty->taps.empty() );

    ProjectConfig cfg;
    cfg.host = "localhost";
    cfg.port = 7777;
    cfg.taps.push_back( { "xc7.tap", 0x037c4093, "U1", ( dir / "bsdl" / "chip.bsd" ).string(),
                          "CSGA225", { { "D5", "z" }, { "E1", "0" } } } );
    cfg.taps.push_back( { "other.tap", 0, "", "/opt/bsdl/other.bsd", "", {} } );
    cfg.watch.push_back( { "U1", "D1", "BTN0" } );
    cfg.watch.push_back( { "U1", "D2", "" } );
    cfg.extestLeft.push_back( "xc7.tap" );
    cfg.extestImages["xc7.tap"] = { ( dir / "bsdl" / "chip.bsd" ).string(), "CSGA225", "0101",
                                    "2026-10-16 22:50:00" };
    cfg.bsdlDirs.push_back( ( dir / "bsdl" ).string() );
    cfg.bsdlDirs.push_back( "/opt/bsdl" );
    cfg.interconnectInclude = { "SCL", "SDA" };

    CHECK( SaveProjectConfig( file, cfg ).has_value() );

    // The file stores the first path relative, the second absolute
    std::ifstream in( file );
    std::string   text( ( std::istreambuf_iterator<char>( in ) ), std::istreambuf_iterator<char>() );
    CHECK( text.find( "\"bsdl/chip.bsd\"" ) != std::string::npos );
    CHECK( text.find( "\"/opt/bsdl/other.bsd\"" ) != std::string::npos );
    CHECK( text.find( "\"0x037c4093\"" ) != std::string::npos );

    auto loaded = LoadProjectConfig( file );
    CHECK( loaded.has_value() );

    if( loaded )
    {
        CHECK( *loaded == cfg );
        CHECK( loaded->Find( "xc7.tap" ) && loaded->Find( "xc7.tap" )->reference == "U1" );
        CHECK( loaded->FindByReference( "u1" ) && !loaded->FindByReference( "U2" ) );
        CHECK( !loaded->FindByReference( "" ) );
    }

    std::ofstream( file ) << "{ not json";
    CHECK( !LoadProjectConfig( file ) );

    fs::remove_all( dir );
}


static void testExtest()
{
    auto dev = BsdlDevice::Load( SHARED_BSD );
    CHECK( dev.has_value() );

    if( !dev )
    {
        std::printf( "  %s\n", dev.error().c_str() );
        return;
    }

    const BsdlDevice& d = **dev;

    auto outs = OutputPins( d, "" );
    CHECK( outs.size() == 4 );

    // Settings for pins that are not outputs here (TCK, unknown pin)
    auto stale = StaleSettings( d, "", { { "1", PinDrive::Low }, { "5", PinDrive::High },
                                         { "Z9", PinDrive::HighZ } } );
    CHECK( ( stale == std::vector<std::string>{ "5", "Z9" } ) );
    CHECK( StaleSettings( d, "", { { "2", PinDrive::Keep } } ).empty() );

    // Safe image: control 0 = 0 (A, B off), OUTB cell 1, OUTC 0, control 4 = 1 (D off)
    ExtestPlan safe = PlanExtest( d, "", {}, {} );
    CHECK( safe.conflicts.empty() );
    CHECK( safe.image.ToBitString() == "010100" );   // cells 5..0
    CHECK( safe.pins.size() == 4 );
    CHECK( safe.pins[0].pin == "1" && safe.pins[0].planned == PinState::HighZ );
    CHECK( safe.pins[2].pin == "3" && safe.pins[2].planned == PinState::Low );
    CHECK( safe.pins[3].planned == PinState::HighZ );
    CHECK( safe.pins[0].current == PinState::Unknown );

    // Drive OUTA high: shared control enables OUTB too (with its safe value 1)
    ExtestPlan a1 = PlanExtest( d, "", {}, { { "1", PinDrive::High } } );
    CHECK( a1.conflicts.empty() );
    CHECK( a1.pins[0].planned == PinState::High && a1.pins[0].explicitSetting );
    CHECK( a1.pins[1].planned == PinState::High && !a1.pins[1].explicitSetting );

    // Conflict: OUTA driven, OUTB off, same control cell
    ExtestPlan c = PlanExtest( d, "", {}, { { "1", PinDrive::Low }, { "2", PinDrive::HighZ } } );
    CHECK( c.conflicts.size() == 1 );

    // Driving pins of a device in EXTEST (safe image active)
    {
        std::map<std::string, DriveDevice> devices = { { "shared.tap", { &d, "", safe.image, "U3" } } };
        using Skip = DrivePlan::SkipReason;

        // OUTA low: OUTB (same enable) starts driving too
        DrivePlan p1 = PlanDrive( devices, { { "shared.tap", "1", "U3 1" } }, PinDrive::Low );
        CHECK( p1.applied == 1 && p1.skipped.empty() && p1.images.size() == 1 );
        CHECK( p1.alsoAffected == std::vector<std::string>{ "U3 2" } );
        CHECK( StateFromImage( p1.images["shared.tap"], *d.LookupPin( "", "1" ) ) == PinState::Low );

        // Both requested: no side effect to report
        DrivePlan p2 = PlanDrive( devices, { { "shared.tap", "1", "U3 1" }, { "shared.tap", "2", "U3 2" } },
                                  PinDrive::High );
        CHECK( p2.applied == 2 && p2.alsoAffected.empty() );

        // OUTC has no enable (Z), TCK no output, U9 not in EXTEST
        DrivePlan p3 = PlanDrive( devices,
                                  { { "shared.tap", "3", "U3 3" }, { "", "7", "U9 7" } },
                                  PinDrive::HighZ );
        CHECK( p3.applied == 0 && p3.images.empty() && p3.skipped.size() == 2 );
        CHECK( p3.skipped.size() == 2 && p3.skipped[0].reason == Skip::NoOutputEnable
               && p3.skipped[1].reason == Skip::NotInExtest && p3.skipped[1].name == "U9 7" );
        DrivePlan p4 = PlanDrive( devices, { { "shared.tap", "5", "U3 5" } }, PinDrive::Low );
        CHECK( p4.skipped.size() == 1 && p4.skipped[0].reason == Skip::NoOutput );

        // OUTC low again (safe value): applied, but the image does not change
        DrivePlan p5 = PlanDrive( devices, { { "shared.tap", "3", "U3 3" } }, PinDrive::Low );
        CHECK( p5.applied == 1 && p5.images.empty() );
    }

    // OUTC cannot be Z; OUTD (disable value 1) can
    ExtestPlan z = PlanExtest( d, "", {}, { { "3", PinDrive::HighZ }, { "4", PinDrive::Low } } );
    CHECK( z.conflicts.size() == 1 );
    CHECK( z.pins[3].planned == PinState::Low );
    CHECK( z.image[4] == false && z.image[5] == false );

    // Keep without samples is refused
    CHECK( PlanExtest( d, "", {}, { { "4", PinDrive::Keep } } ).conflicts.size() == 1 );

    // Keep with samples; OUTD toggles between the samples
    Bits s1 = *Bits::FromBitString( "000001" );   // A,B enabled driving 0; D: ctrl 0 -> on, 0
    Bits s2 = *Bits::FromBitString( "100001" );   // D drives 1
    ExtestPlan k = PlanExtest( d, "", { s1, s2 }, { { "4", PinDrive::Keep } } );
    CHECK( k.conflicts.empty() );
    CHECK( k.pins[3].current == PinState::Toggling && k.pins[3].Changes() );
    CHECK( k.pins[3].planned == PinState::High );      // last sample
    CHECK( k.pins[0].current == PinState::Low && k.pins[0].planned == PinState::HighZ );
    CHECK( k.pins[0].Changes() );
    CHECK( k.pins[2].current == PinState::Low && !k.pins[2].Changes() );

    // SetPin: switching OUTA off also switches OUTB off
    auto ch = SetPin( d, "", a1.image, "1", PinDrive::HighZ );
    CHECK( ch && ch->alsoAffected.size() == 1 && ch->alsoAffected[0] == "2" );
    CHECK( ch && StateFromImage( ch->image, outs[0].cells ) == PinState::HighZ );
    CHECK( !SetPin( d, "", a1.image, "3", PinDrive::HighZ ) );   // no output enable
    auto d1 = SetPin( d, "", safe.image, "4", PinDrive::High );
    CHECK( d1 && d1->alsoAffected.empty() && StateFromImage( d1->image, outs[3].cells ) == PinState::High );

    // While in EXTEST, the output comes from the image, the input from the capture
    CHECK( PinDriveFromString( "keep" ) == PinDrive::Keep && !PinDriveFromString( "x" ) );
}


static void testCurIr()
{
    auto dev = BsdlDevice::Load( SIMCHIP_BSD );
    CHECK( dev.has_value() );

    if( !dev )
        return;

    const BsdlDevice& d = **dev;
    auto              extest = d.Opcode( "EXTEST" );
    auto              sample = d.Opcode( "SAMPLE" );
    auto              bypass = d.Opcode( "BYPASS" );
    CHECK( extest && sample && bypass );

    if( extest && sample && bypass )
    {
        CHECK( PinControlInstruction( d, *extest ) == "EXTEST" );
        CHECK( PinControlInstruction( d, *sample ).empty() );
        CHECK( PinControlInstruction( d, *bypass ).empty() );
        auto names = d.InstructionsFor( *bypass );
        CHECK( std::find( names.begin(), names.end(), "BYPASS" ) != names.end() );
    }

    CHECK( IsPinControlInstruction( "clamp" ) && !IsPinControlInstruction( "SAMPLE" ) );
}


static void testTapStatus()
{
    auto dev = BsdlDevice::Load( SIMCHIP_BSD );
    CHECK( dev.has_value() );

    if( !dev )
        return;

    using Check = TapStatus::Check;
    TapInfo tap{ 0, "simchip.tap", true, 0x1b5c30ef, 4, { 0x1b5c30ef }, false };

    TapFacts ok;
    ok.connected = true;
    ok.assigned  = true;
    ok.device    = dev->get();

    TapStatus s = EvaluateTap( tap, ok );
    CHECK( s.check == Check::Ok && !s.problem && s.extestAllowed && !s.lookForBsdl );

    // No file: BYPASS, matching files worth looking for
    TapFacts none = ok;
    none.assigned = false;
    none.device   = nullptr;
    s = EvaluateTap( tap, none );
    CHECK( s.check == Check::Bypass && !s.problem && !s.extestAllowed && s.lookForBsdl );

    // File not readable
    TapFacts err = ok;
    err.device   = nullptr;
    CHECK( EvaluateTap( tap, err ).check == Check::BsdlError && EvaluateTap( tap, err ).problem );

    // Only in the settings: offline / not in chain, no EXTEST, no suggestions
    TapFacts gone = ok;
    gone.present  = false;
    s = EvaluateTap( tap, gone );
    CHECK( s.check == Check::NotInChain && !s.extestAllowed && !s.lookForBsdl );
    gone.connected = false;
    CHECK( EvaluateTap( tap, gone ).check == Check::Offline );

    // Wrong IR length: mismatch, suggestions; other IDCODE than assigned
    TapInfo wrongIr = tap;
    wrongIr.irLength = 5;
    s = EvaluateTap( wrongIr, ok );
    CHECK( s.check == Check::Mismatch && s.problem && !s.extestAllowed && s.lookForBsdl );
    TapFacts changed     = ok;
    changed.assignedIdcode = 0x2b5c30ef;
    CHECK( EvaluateTap( tap, changed ).check == Check::IdcodeChanged );

    // Unknown reference wins over the file check, also without a file
    TapFacts ref       = none;
    ref.referenceUnknown = true;
    CHECK( EvaluateTap( tap, ref ).check == Check::UnknownReference && EvaluateTap( tap, ref ).problem );

    // EXTEST wins over everything; no suggestions then
    TapFacts ex = ok;
    ex.inExtest = true;
    s = EvaluateTap( wrongIr, ex );
    CHECK( s.check == Check::Extest && s.problem && !s.lookForBsdl );

    // Another TAP in EXTEST blocks; held: take over if possible
    TapFacts blocked  = ok;
    blocked.elsewhere = "other.tap";
    s = EvaluateTap( tap, blocked );
    CHECK( s.blocked && !s.extestAllowed );

    TapFacts held  = blocked;
    held.held      = true;
    s = EvaluateTap( tap, held );
    CHECK( s.check == Check::Held && !s.blocked && !s.extestAllowed && !s.takeOver );
    held.canTakeOver = true;
    s = EvaluateTap( tap, held );
    CHECK( s.takeOver && s.extestAllowed );
}


static void testBsdlIndex()
{
    namespace fs = std::filesystem;

    CHECK( IdcodePatternMatches( "XXXX0000000000000000000000000001", 0xF0000001 ) );
    CHECK( !IdcodePatternMatches( "XXXX0000000000000000000000000001", 0xF0000003 ) );
    CHECK( !IdcodePatternMatches( "", 0 ) );

    fs::path dir = fs::temp_directory_path() / "bscan-test-bsdl-index";
    fs::remove_all( dir );
    fs::create_directories( dir / "a" / "deep" );
    fs::create_directories( dir / "b" );
    fs::copy_file( SIMCHIP_BSD, dir / "a" / "deep" / "simchip.bsd" );
    fs::copy_file( SIMCHIP_BSD, dir / "b" / "simchip_copy.BSDL" );
    fs::copy_file( SHARED_BSD, dir / "a" / "shared.bsd" );
    std::ofstream( dir / "a" / "junk.bsd" ) << "this is not a BSDL file\n";
    std::ofstream( dir / "a" / "notes.txt" ) << "ignored\n";

    auto sim = BsdlDevice::Load( SIMCHIP_BSD );
    CHECK( sim.has_value() );

    if( !sim )
        return;

    const uint32_t id = 0x1B5C30EF;
    CHECK( ( *sim )->IdcodeMatches( id ).value_or( false ) );

    std::string cache = ( dir / "cache" / "index.json" ).string();
    BsdlIndex   index;
    auto        stats = index.Scan( { ( dir / "a" ).string(), ( dir / "b" ).string() } );
    CHECK( stats.files == 4 );
    CHECK( stats.parsed == 4 );
    CHECK( stats.failed == 1 );
    CHECK( index.SaveCache( cache ) );

    auto matches = index.Match( id, ( *sim )->IrLength() );
    CHECK( matches.size() == 2 );
    CHECK( index.Match( id, ( *sim )->IrLength() + 1 ).empty() );
    CHECK( index.Match( id ^ 0x00010000, -1 ).empty() );

    // Second run: everything from the cache
    BsdlIndex again;
    again.LoadCache( cache );
    auto stats2 = again.Scan( { ( dir / "a" ).string(), ( dir / "b" ).string() } );
    CHECK( stats2.files == 4 && stats2.parsed == 0 && stats2.failed == 1 );
    CHECK( again.Match( id, -1 ).size() == 2 );

    // Ranking: preferred folder first, then the pin map that fits the pads
    std::set<std::string> bgaPads = { "A1", "A2", "A3", "B1", "B2", "B3",
                                      "C1", "C2", "C3", "D1", "D2", "D3" };
    auto ranked = RankSuggestions( matches, bgaPads, { ( dir / "b" ).string() } );
    CHECK( ranked.size() == 4 );   // 2 files x 2 pin maps

    if( ranked.size() == 4 )
    {
        CHECK( ranked[0].preferred && ranked[0].pinMap == "BGA9" );
        CHECK( ranked[0].padsFound == 12 && ranked[0].padsTotal == 12 );
        CHECK( ranked[1].preferred && ranked[1].pinMap == "QFN16" );
        CHECK( !ranked[2].preferred && ranked[2].pinMap == "BGA9" );
    }

    auto qfn = RankSuggestions( matches, { "1", "2", "3", "4", "5", "9", "10", "11", "12", "13",
                                           "14", "15", "16" },
                                {} );
    CHECK( !qfn.empty() && qfn[0].pinMap == "QFN16" && qfn[0].padsFound == 12 );

    fs::remove_all( dir );
}


// --- interconnect test ---

// Board model for the simulated captures: every net is driven by the pins
// whose output is enabled in the image (wired AND), otherwise pulled up.
// aShort: two nets that are shorted (wired AND). aOpen: "REF pad" that does
// not reach its net (reads 1).
static std::vector<std::vector<Bits>> simulateCaptures( const InterconnectPlan& aPlan,
                                                        std::pair<std::string, std::string> aShort = {},
                                                        std::string aOpen = {} )
{
    std::vector<std::vector<Bits>> captured;

    for( const std::vector<Bits>& images : aPlan.patterns )
    {
        std::map<std::string, bool> level;

        for( const TestNet& n : aPlan.nets )
        {
            bool v = true;

            for( const NetPin& p : n.bsPins )
            {
                if( p.reference + " " + p.pad == aOpen || p.cells.outputCell < 0 )
                    continue;

                const Bits& img = images[p.device];
                bool enabled = p.cells.controlCell < 0
                               || img[p.cells.controlCell] != ( p.cells.disableValue != 0 );

                if( enabled )
                    v = v && img[p.cells.outputCell];
            }

            level[n.name] = v;
        }

        if( !aShort.first.empty() )
        {
            bool v = level[aShort.first] && level[aShort.second];
            level[aShort.first] = level[aShort.second] = v;
        }

        std::vector<Bits> caps;

        for( const IcDevice& d : aPlan.devices )
            caps.emplace_back( static_cast<size_t>( d.device->BoundaryLength() ) );

        for( const TestNet& n : aPlan.nets )
            for( const NetPin& p : n.bsPins )
                if( p.cells.inputCell >= 0 )
                    caps[p.device].set( p.cells.inputCell,
                                        p.reference + " " + p.pad == aOpen ? true : level[n.name] );

        captured.push_back( std::move( caps ) );
    }

    return captured;
}


static const TestNet* findNet( const InterconnectPlan& aPlan, const std::string& aName )
{
    for( const TestNet& n : aPlan.nets )
        if( n.name == aName )
            return &n;

    return nullptr;
}


static void testInterconnect()
{
    auto u1 = BsdlDevice::Load( SIMCHIP_BSD );
    auto u2 = BsdlDevice::Load( SIMCHIP2_BSD );
    CHECK( u1 && u2 );

    if( !u1 || !u2 )
        return;

    // QFN16: IN0 1, IN1 2, OUT0 3, IO0 4, A1 5, TCK 9, VCC 13/14, GND 15
    PadNets board = {
        { "U1", { { "1", "N_IN0" }, { "2", "NET_B" }, { "3", "NET_C" }, { "4", "NET_A" },
                  { "9", "TCK" }, { "13", "VCC" }, { "15", "GND" } } },
        { "U2", { { "1", "N_IN0" }, { "2", "NET_A" }, { "4", "NET_B" }, { "15", "GND" } } },
        { "R1", { { "1", "NET_C" }, { "2", "GND" } } },
        { "R2", { { "1", "N_IN0" } } },
    };

    auto devices = [&]( std::map<std::string, PinDrive> aU2Constraints = {} )
    {
        return std::vector<IcDevice>{ { "simchip.tap", "U1", *u1, "", {}, ( *u1 )->SafeBits() },
                                      { "other.tap", "U2", *u2, "", aU2Constraints,
                                        ( *u2 )->SafeBits() } };
    };

    InterconnectPlan plan = PlanInterconnect( devices(), board, {} );

    const TestNet* a = findNet( plan, "NET_A" );
    const TestNet* b = findNet( plan, "NET_B" );
    const TestNet* c = findNet( plan, "NET_C" );
    const TestNet* i = findNet( plan, "N_IN0" );
    CHECK( a && a->status == TestNet::Status::Tested && a->driver >= 0
           && a->bsPins[a->driver].reference == "U1" );
    CHECK( b && b->status == TestNet::Status::Tested && b->bsPins[b->driver].reference == "U2" );
    CHECK( c && c->status == TestNet::Status::FewPins );
    CHECK( i && i->status == TestNet::Status::OtherParts && i->otherParts == std::vector<std::string>{ "R2" } );
    CHECK( !findNet( plan, "VCC" ) && !findNet( plan, "TCK" ) );   // no pin with cells
    CHECK( plan.TestedCount() == 2 && plan.width == 3 && plan.patterns.size() == 6 );
    CHECK( a && b && a->code != b->code );

    // Included, but two inputs: nobody can drive
    InterconnectPlan incl = PlanInterconnect( devices(), board, { "N_IN0" } );
    CHECK( findNet( incl, "N_IN0" )->status == TestNet::Status::NoDriver );

    // A starting state fixes the net
    InterconnectPlan fixed = PlanInterconnect( devices( { { "4", PinDrive::Low } } ), board, {} );
    const TestNet* fb = findNet( fixed, "NET_B" );
    CHECK( fb && fb->status == TestNet::Status::Fixed
           && fb->reason.find( "U2 pin 4" ) != std::string::npos );
    CHECK( fb && fb->fixedBy.size() == 1 && fb->bsPins[fb->fixedBy[0]].pad == "4" );

    // Roles of U2's pins: pin 4 keeps NET_B out, pin 2 (NET_A) is tested
    auto roles = PinTestRoles( fixed, 1 );
    CHECK( roles["4"].kind == PinTestRole::Kind::FixedHere && roles["4"].net == "NET_B" );
    CHECK( roles["2"].kind == PinTestRole::Kind::Tested && roles["2"].net == "NET_A" );
    CHECK( roles["1"].kind == PinTestRole::Kind::NotTested );   // N_IN0: other parts
    // ... and of U1's: NET_B is fixed by U2, not by U1
    CHECK( PinTestRoles( fixed, 0 )["2"].kind == PinTestRole::Kind::NotTested );

    // A pin set by hand in EXTEST fixes its net the same way, named as such
    {
        std::vector<IcDevice> devs = devices( { { "4", PinDrive::High } } );
        devs[1].currentState.insert( "4" );
        InterconnectPlan now = PlanInterconnect( devs, board, {} );
        const TestNet*   nb  = findNet( now, "NET_B" );
        CHECK( nb && nb->status == TestNet::Status::Fixed
               && nb->reason.find( "current EXTEST state of U2 pin 4" ) != std::string::npos );
        auto r = PinTestRoles( now, 1 );
        CHECK( r["4"].kind == PinTestRole::Kind::FixedHere && r["4"].currentState );
        CHECK( !roles["4"].currentState );   // the starting state above
    }

    // Other parts come first: a starting state on such a net does not count
    PadNets boardR = board;
    boardR["R3"]["1"] = "NET_B";
    InterconnectPlan mixedFixed =
            PlanInterconnect( devices( { { "4", PinDrive::Low } } ), boardR, {} );
    CHECK( findNet( mixedFixed, "NET_B" )->status == TestNet::Status::OtherParts );
    InterconnectPlan inclFixed =
            PlanInterconnect( devices( { { "4", PinDrive::Low } } ), boardR, { "NET_B" } );
    CHECK( findNet( inclFixed, "NET_B" )->status == TestNet::Status::Fixed );

    // Session: request, runs, statistics
    {
        InterconnectSession session;
        session.SetPlan( plan );
        CHECK( UsedDevices( plan ) == std::vector<size_t>( { 0, 1 } ) );

        TapInfo t1, t2;
        t1.name = "simchip.tap";
        t2.name = "other.tap";
        CHECK( !session.Start( { t1 }, false ) );   // other.tap missing
        CHECK( !session.Running() );

        auto req = session.Start( { t2, t1 }, true );
        CHECK( req && req->taps.size() == 2 && req->taps[0].name == "simchip.tap"
               && req->patterns.size() == plan.patterns.size() && req->repeat );
        CHECK( session.Running() && session.Repeating() && !session.Stat( "NET_A" ) );

        auto s1 = session.AddRun( simulateCaptures( plan ) );
        CHECK( s1.runs == 1 && s1.tested == 2 && s1.failing == 0 && s1.failingNow == 0 );

        auto s2 = session.AddRun( simulateCaptures( plan, { "NET_A", "NET_B" } ) );
        CHECK( s2.runs == 2 && s2.failing == 2 && s2.failingNow == 2 );

        auto s3 = session.AddRun( simulateCaptures( plan ) );
        const auto* a3 = session.Stat( "NET_A" );
        CHECK( s3.failing == 2 && s3.failingNow == 0 && a3 && a3->fails == 1 && !a3->failingNow
               && a3->lastFail.find( "short" ) != std::string::npos );
        CHECK( session.Running() );
        session.Stop();
        CHECK( !session.Running() && session.Runs() == 3 );

        // A single run ends by itself; a new start resets the statistics
        CHECK( session.Start( { t1, t2 }, false ) );
        CHECK( !session.Stat( "NET_A" ) );
        session.AddRun( simulateCaptures( plan ) );
        CHECK( !session.Running() && session.Runs() == 1 && session.Stat( "NET_A" )->fails == 0 );
    }

    // Good board
    auto ok = EvaluateInterconnect( plan, simulateCaptures( plan ) );
    CHECK( ok.size() == 2 && ok[0].kind == NetResult::Kind::Ok && ok[1].kind == NetResult::Kind::Ok );

    // Short between the two nets
    auto sh = EvaluateInterconnect( plan, simulateCaptures( plan, { "NET_A", "NET_B" } ) );
    CHECK( sh.size() == 2 && sh[0].kind == NetResult::Kind::Short && sh[1].kind == NetResult::Kind::Short );
    CHECK( sh.size() == 2 && sh[0].shortWith == std::vector<std::string>{ "NET_B" } );

    // Open at the receiver of NET_A (reads 1): one receiver, so driver or receiver
    auto op = EvaluateInterconnect( plan, simulateCaptures( plan, {}, "U2 2" ) );
    CHECK( op.size() == 2 && op[0].net == "NET_A" && op[0].kind == NetResult::Kind::Stuck1 );
    CHECK( op.size() == 2 && op[0].text.find( "U2 pin 2" ) != std::string::npos );
    CHECK( op.size() == 2 && op[1].kind == NetResult::Kind::Ok );

    // Two receivers on NET_A (U2 pin 5 = A1 added): open at one of them
    PadNets board3 = board;
    board3["U2"]["5"] = "NET_A";
    InterconnectPlan plan3 = PlanInterconnect( devices(), board3, {} );
    auto op3 = EvaluateInterconnect( plan3, simulateCaptures( plan3, {}, "U2 2" ) );
    CHECK( op3.size() == 2 && op3[0].kind == NetResult::Kind::Open
           && op3[0].pins == std::vector<std::string>{ "U2 pin 2" } );

    // Shared output enable: OUTA and OUTB of SHARED share control cell 0 and
    // cannot receive (no input cells); U1 receives
    auto sd = BsdlDevice::Load( SHARED_BSD );
    CHECK( sd.has_value() );

    if( !sd )
        return;

    PadNets board2 = {
        { "U3", { { "1", "X" }, { "2", "Y" } } },
        { "U1", { { "1", "Y" }, { "2", "X" } } },
        { "R9", { { "1", "Y" } } },
    };

    auto devices2 = [&]
    {
        return std::vector<IcDevice>{ { "simchip.tap", "U1", *u1, "", {}, ( *u1 )->SafeBits() },
                                      { "shared.tap", "U3", *sd, "", {}, ( *sd )->SafeBits() } };
    };

    // Y excluded (R9): enabling OUTA for X would make OUTB drive -> not tested
    InterconnectPlan p2 = PlanInterconnect( devices2(), board2, {} );
    CHECK( findNet( p2, "X" )->status == TestNet::Status::SharedEnable );
    CHECK( findNet( p2, "X" )->reason.find( "U3 pin 2" ) != std::string::npos );

    // Y included: both drivers share the enable, both nets tested
    InterconnectPlan p3 = PlanInterconnect( devices2(), board2, { "Y" } );
    CHECK( findNet( p3, "X" )->status == TestNet::Status::Tested );
    CHECK( findNet( p3, "Y" )->status == TestNet::Status::Tested );
    auto r3 = EvaluateInterconnect( p3, simulateCaptures( p3 ) );
    CHECK( r3.size() == 2 && r3[0].kind == NetResult::Kind::Ok && r3[1].kind == NetResult::Kind::Ok );
}


int main()
{
    testBits();
    testInterconnect();
    testScanChain();
    testBsdl();
    testProjectConfig();
    testExtest();
    testBsdlIndex();
    testCurIr();
    testTapStatus();

    std::printf( g_failed ? "%d check(s) failed\n" : "all checks passed\n", g_failed );
    return g_failed ? 1 : 0;
}
