// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/bsdl_device.h"

#include <algorithm>
#include <cctype>
#include <string_view>

#include <bsdl.h>

namespace bscan
{

static const bsdl_device* dev( const void* p )
{
    return static_cast<const bsdl_device*>( p );
}


std::expected<std::shared_ptr<const BsdlDevice>, std::string>
BsdlDevice::Load( const std::string& aPath )
{
    bsdl_result* res = nullptr;
    bsdl_status  st  = bsdl_parse_file( aPath.c_str(), nullptr, &res );

    if( !res )
        return std::unexpected( aPath + ": " + bsdl_status_str( st ) );

    std::shared_ptr<BsdlDevice> d( new BsdlDevice() );
    d->m_path   = aPath;
    d->m_result = res;
    d->m_dev    = res->device;

    for( size_t i = 0; i < res->diag_count; ++i )
    {
        const bsdl_diag& g = res->diags[i];
        d->m_diags.push_back( { g.severity == BSDL_SEV_ERROR, g.line,
                                g.message ? g.message : "" } );
    }

    if( !res->device )
    {
        std::string msg = aPath + ": no device could be parsed";

        for( const Diagnostic& g : d->m_diags )
        {
            if( g.error )
            {
                msg += " (line " + std::to_string( g.line ) + ": " + g.message + ")";
                break;
            }
        }

        return std::unexpected( msg );
    }

    return d;
}


BsdlDevice::~BsdlDevice()
{
    bsdl_free( static_cast<bsdl_result*>( m_result ) );
}


std::string BsdlDevice::EntityName() const
{
    const char* n = dev( m_dev )->entity_name;
    return n ? n : "";
}


int BsdlDevice::IrLength() const
{
    return dev( m_dev )->instruction_length;
}


int BsdlDevice::BoundaryLength() const
{
    const bsdl_device* d = dev( m_dev );

    if( d->boundary_length >= 0 )
        return d->boundary_length;

    // Fall back to the highest cell number in BOUNDARY_REGISTER
    int maxCell = -1;

    for( size_t i = 0; i < d->cell_count; ++i )
        maxCell = std::max( maxCell, d->cells[i].number );

    return maxCell >= 0 ? maxCell + 1 : -1;
}


std::string BsdlDevice::IdcodePattern() const
{
    const char* id = dev( m_dev )->idcode;
    return id ? id : "";
}


std::optional<bool> BsdlDevice::IdcodeMatches( uint32_t aIdcode ) const
{
    int match = bsdl_idcode_match( dev( m_dev ), aIdcode );

    if( match < 0 )
        return std::nullopt;

    return match == 1;
}


std::optional<uint64_t> BsdlDevice::Opcode( const std::string& aInstruction ) const
{
    const bsdl_instruction* ins = bsdl_find_instruction( dev( m_dev ), aInstruction.c_str() );

    if( !ins || ins->opcode_count == 0 )
        return std::nullopt;

    // An instruction may have several opcodes; the first one is used.
    auto bits = Bits::FromBitString( ins->opcodes[0], false );

    if( !bits || bits->size() > 64 )
        return std::nullopt;

    return bits->ToU64();
}


std::vector<std::string> BsdlDevice::InstructionsFor( uint64_t aValue ) const
{
    const bsdl_device* d = dev( m_dev );
    std::vector<const bsdl_instruction*> found( bsdl_instructions_for_opcode( d, aValue, nullptr, 0 ) );

    bsdl_instructions_for_opcode( d, aValue, found.data(), found.size() );

    std::vector<std::string> names;

    for( const bsdl_instruction* ins : found )
        if( ins->name )
            names.emplace_back( ins->name );

    return names;
}


Bits BsdlDevice::SafeBits() const
{
    const bsdl_device* d   = dev( m_dev );
    int                len = BoundaryLength();
    Bits               bits( len > 0 ? static_cast<size_t>( len ) : 0 );

    if( d->safe_image )
    {
        // Indexed by cell number; 'X' as 0
        for( size_t i = 0; d->safe_image[i] && i < bits.size(); ++i )
            bits.set( i, d->safe_image[i] == '1' );

        return bits;
    }

    // No BOUNDARY_LENGTH (length from the highest cell number): first
    // value other than 'X' of a cell, as libbsdl does
    std::vector<char> safe( bits.size(), 'X' );

    for( size_t i = 0; i < d->cell_count; ++i )
    {
        const bsdl_cell& c = d->cells[i];

        if( c.number >= 0 && static_cast<size_t>( c.number ) < safe.size()
            && safe[c.number] == 'X' )
            safe[c.number] = c.safe;
    }

    for( size_t i = 0; i < safe.size(); ++i )
        bits.set( i, safe[i] == '1' );

    return bits;
}


static PinCells toPinCells( const bsdl_pin_info& aInfo )
{
    PinCells out;
    out.port = aInfo.port.name ? aInfo.port.name : "";

    if( aInfo.port.index >= 0 )
        out.port += "(" + std::to_string( aInfo.port.index ) + ")";

    out.inputCell    = aInfo.input_cell;
    out.outputCell   = aInfo.output_cell;
    out.controlCell  = aInfo.control_cell;
    out.disableValue = aInfo.disable_value;
    return out;
}


// all pins with an output cell (aControlCell < -1) or those with aControlCell
static std::vector<OutputPin> outputPins( const bsdl_device* d, const std::string& aPinMap,
                                          int aControlCell )
{
    const bsdl_pin_map* map = nullptr;

    if( !aPinMap.empty() )
    {
        map = bsdl_find_pin_map( d, aPinMap.c_str() );

        if( !map )
            return {};
    }

    auto query = [&]( bsdl_pin_info* aOut, size_t aMax )
    {
        return aControlCell < -1 ? bsdl_output_pins( d, map, aOut, aMax )
                                 : bsdl_pins_with_control_cell( d, map, aControlCell, aOut, aMax );
    };

    std::vector<bsdl_pin_info> infos( query( nullptr, 0 ) );
    query( infos.data(), infos.size() );

    std::vector<OutputPin> out;

    for( const bsdl_pin_info& info : infos )
        out.push_back( { info.pin ? info.pin : "", toPinCells( info ) } );

    return out;
}


std::vector<OutputPin> BsdlDevice::OutputPins( const std::string& aPinMap ) const
{
    return outputPins( dev( m_dev ), aPinMap, -2 );
}


std::vector<OutputPin> BsdlDevice::PinsWithControlCell( const std::string& aPinMap,
                                                        int aControlCell ) const
{
    if( aControlCell < 0 )
        return {};

    return outputPins( dev( m_dev ), aPinMap, aControlCell );
}


std::vector<std::string> BsdlDevice::PinMapNames() const
{
    const bsdl_device*       d = dev( m_dev );
    std::vector<std::string> names;

    for( size_t i = 0; i < d->pin_map_count; ++i )
        names.emplace_back( d->pin_maps[i].name ? d->pin_maps[i].name : "" );

    return names;
}


std::string BsdlDevice::DefaultPinMap() const
{
    const bsdl_pin_map* m = dev( m_dev )->active_pin_map;
    return m && m->name ? m->name : "";
}


std::vector<std::string> BsdlDevice::PinNames( const std::string& aPinMap ) const
{
    const bsdl_device*  d   = dev( m_dev );
    const bsdl_pin_map* map = aPinMap.empty() ? d->active_pin_map
                                              : bsdl_find_pin_map( d, aPinMap.c_str() );
    std::vector<std::string> pins;

    for( size_t i = 0; map && i < map->entry_count; ++i )
        pins.emplace_back( map->entries[i].pin ? map->entries[i].pin : "" );

    return pins;
}


std::vector<PackagePin> BsdlDevice::PackagePins( const std::string& aPinMap ) const
{
    const bsdl_device*  d   = dev( m_dev );
    const bsdl_pin_map* map = aPinMap.empty() ? d->active_pin_map
                                              : bsdl_find_pin_map( d, aPinMap.c_str() );
    std::vector<PackagePin> pins;

    auto same = []( const char* aA, const char* aB )
    {
        if( !aA || !aB )
            return false;

        std::string_view a( aA ), b( aB );
        return std::ranges::equal( a, b, []( unsigned char x, unsigned char y )
                                   { return std::toupper( x ) == std::toupper( y ); } );
    };

    for( size_t i = 0; map && i < map->entry_count; ++i )
    {
        const bsdl_pin_map_entry& e = map->entries[i];

        if( !e.pin )
            continue;

        PackagePin p;
        p.pin = e.pin;

        if( auto cells = LookupPin( aPinMap, p.pin ) )
            p.cells = *cells;

        const char* port = e.port.name;
        const bsdl_tap& tap = d->tap;

        bool linkage = false;

        for( size_t k = 0; k < d->port_count; ++k )
        {
            if( same( d->ports[k].name, port ) )
                linkage = d->ports[k].dir == BSDL_DIR_LINKAGE;
        }

        if( same( port, tap.tdi ) || same( port, tap.tdo ) || same( port, tap.tms )
            || same( port, tap.tck ) || same( port, tap.trst ) )
            p.kind = PackagePin::Kind::Jtag;
        else if( linkage )
            p.kind = PackagePin::Kind::Linkage;
        else if( p.cells.HasCells() )
            p.kind = PackagePin::Kind::BoundaryScan;
        else
            p.kind = PackagePin::Kind::NoCells;

        if( p.cells.port.empty() && port )
        {
            p.cells.port = port;

            if( e.port.index >= 0 )
                p.cells.port += "(" + std::to_string( e.port.index ) + ")";
        }

        pins.push_back( std::move( p ) );
    }

    return pins;
}


std::optional<PinCells> BsdlDevice::LookupPin( const std::string& aPinMap,
                                               const std::string& aPin ) const
{
    const bsdl_device*  d   = dev( m_dev );
    const bsdl_pin_map* map = nullptr;

    if( !aPinMap.empty() )
    {
        map = bsdl_find_pin_map( d, aPinMap.c_str() );

        if( !map )
            return std::nullopt;
    }

    bsdl_pin_info info;

    if( bsdl_pin_lookup_physical( d, map, aPin.c_str(), &info ) != BSDL_OK )
        return std::nullopt;

    return toPinCells( info );
}

} // namespace bscan
