// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/project_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace bscan
{

static bool equalsNoCase( const std::string& a, const std::string& b )
{
    return a.size() == b.size()
           && std::equal( a.begin(), a.end(), b.begin(),
                          []( unsigned char x, unsigned char y )
                          { return std::tolower( x ) == std::tolower( y ); } );
}


TapAssignment* ProjectConfig::Find( const std::string& aTap )
{
    for( TapAssignment& t : taps )
        if( t.tap == aTap )
            return &t;

    return nullptr;
}


const TapAssignment* ProjectConfig::Find( const std::string& aTap ) const
{
    return const_cast<ProjectConfig*>( this )->Find( aTap );
}


const TapAssignment* ProjectConfig::FindByReference( const std::string& aReference ) const
{
    if( aReference.empty() )
        return nullptr;

    for( const TapAssignment& t : taps )
        if( equalsNoCase( t.reference, aReference ) )
            return &t;

    return nullptr;
}


// UTF-8 std::string <-> path (C++20: u8string() returns std::u8string)
static fs::path toPath( const std::string& aUtf8 )
{
    return fs::path( std::u8string( aUtf8.begin(), aUtf8.end() ) );
}


static std::string toUtf8( const fs::path& aPath, bool aGeneric = false )
{
    std::u8string s = aGeneric ? aPath.generic_u8string() : aPath.u8string();
    return std::string( s.begin(), s.end() );
}


std::string ProjectConfigPath( const std::string& aProjectDir, const std::string& aProjectName )
{
    return toUtf8( toPath( aProjectDir ) / toPath( aProjectName + ".bscan.json" ) );
}


static std::string hex32( uint32_t v )
{
    char buf[16];
    std::snprintf( buf, sizeof( buf ), "0x%08x", v );
    return buf;
}


// Relative to aBase if aPath lies below it, otherwise unchanged (absolute)
static std::string toStored( const std::string& aPath, const fs::path& aBase )
{
    if( aPath.empty() )
        return aPath;

    std::error_code ec;
    fs::path        p   = fs::weakly_canonical( toPath( aPath ), ec );
    fs::path        rel = p.lexically_relative( fs::weakly_canonical( aBase, ec ) );

    if( !rel.empty() && *rel.begin() != ".." )
        return toUtf8( rel, true );

    return toUtf8( p, true );
}


static std::string fromStored( const std::string& aPath, const fs::path& aBase )
{
    if( aPath.empty() )
        return aPath;

    fs::path p = toPath( aPath );
    return toUtf8( p.is_absolute() ? p : ( aBase / p ).lexically_normal() );
}


std::expected<ProjectConfig, std::string> LoadProjectConfig( const std::string& aFileUtf8 )
{
    const fs::path aFile = toPath( aFileUtf8 );
    ProjectConfig  cfg;
    std::ifstream  in( aFile );

    if( !in )
        return cfg;   // no file yet: defaults

    try
    {
        json j = json::parse( in );

        if( j.contains( "openocd" ) )
        {
            const json& o = j["openocd"];
            cfg.host      = o.value( "host", cfg.host );
            cfg.port      = o.value( "port", cfg.port );
        }

        fs::path base = aFile.parent_path();

        for( const json& t : j.value( "taps", json::array() ) )
        {
            TapAssignment a;
            a.tap       = t.value( "tap", "" );
            a.reference = t.value( "reference", "" );
            a.bsdl      = fromStored( t.value( "bsdl", "" ), base );
            a.pinMap    = t.value( "pin_map", "" );

            if( t.contains( "extest_start" ) && t["extest_start"].is_object() )
            {
                for( const auto& [pin, mode] : t["extest_start"].items() )
                    if( mode.is_string() )
                        a.extestStart[pin] = mode.get<std::string>();
            }

            std::string id = t.value( "idcode", "" );

            if( !id.empty() )
                a.idcode = static_cast<uint32_t>( std::stoul( id, nullptr, 16 ) );

            if( !a.tap.empty() )
                cfg.taps.push_back( std::move( a ) );
        }

        for( const json& w : j.value( "watch", json::array() ) )
        {
            WatchEntry e{ w.value( "reference", "" ), w.value( "pad", "" ), w.value( "net", "" ) };

            if( !e.reference.empty() && !e.pad.empty() )
                cfg.watch.push_back( std::move( e ) );
        }

        for( const json& d : j.value( "bsdl_dirs", json::array() ) )
            if( d.is_string() && !d.get<std::string>().empty() )
                cfg.bsdlDirs.push_back( fromStored( d.get<std::string>(), base ) );

        for( const json& n : j.value( "interconnect_include", json::array() ) )
            if( n.is_string() && !n.get<std::string>().empty() )
                cfg.interconnectInclude.push_back( n.get<std::string>() );

        std::ranges::sort( cfg.interconnectInclude );

        if( j.contains( "extest_images" ) && j["extest_images"].is_object() )
        {
            for( const auto& [tap, v] : j["extest_images"].items() )
            {
                SavedExtestImage img{ fromStored( v.value( "bsdl", "" ), base ),
                                      v.value( "pin_map", "" ), v.value( "bits", "" ),
                                      v.value( "saved", "" ) };

                if( !img.bits.empty() )
                    cfg.extestImages[tap] = std::move( img );
            }
        }

        for( const json& t : j.value( "extest_left", json::array() ) )
            if( t.is_string() && !t.get<std::string>().empty() )
                cfg.extestLeft.push_back( t.get<std::string>() );
    }
    catch( const std::exception& e )
    {
        return std::unexpected( toUtf8( aFile ) + ": " + e.what() );
    }

    return cfg;
}


std::expected<void, std::string> SaveProjectConfig( const std::string&   aFileUtf8,
                                                    const ProjectConfig& aConfig )
{
    const fs::path aFile = toPath( aFileUtf8 );
    fs::path base = aFile.parent_path();
    json     taps = json::array();

    for( const TapAssignment& a : aConfig.taps )
    {
        json t = { { "tap", a.tap } };

        if( a.idcode )
            t["idcode"] = hex32( a.idcode );
        if( !a.reference.empty() )
            t["reference"] = a.reference;
        if( !a.bsdl.empty() )
            t["bsdl"] = toStored( a.bsdl, base );
        if( !a.pinMap.empty() )
            t["pin_map"] = a.pinMap;
        if( !a.extestStart.empty() )
            t["extest_start"] = a.extestStart;

        taps.push_back( std::move( t ) );
    }

    json watch = json::array();

    for( const WatchEntry& w : aConfig.watch )
    {
        json e = { { "reference", w.reference }, { "pad", w.pad } };

        if( !w.net.empty() )
            e["net"] = w.net;

        watch.push_back( std::move( e ) );
    }

    json j = { { "version", 1 },
               { "openocd", { { "host", aConfig.host }, { "port", aConfig.port } } },
               { "taps", std::move( taps ) },
               { "watch", std::move( watch ) } };

    if( !aConfig.extestLeft.empty() )
        j["extest_left"] = aConfig.extestLeft;

    if( !aConfig.extestImages.empty() )
    {
        json images = json::object();

        for( const auto& [tap, img] : aConfig.extestImages )
            images[tap] = { { "bsdl", toStored( img.bsdl, base ) },
                            { "pin_map", img.pinMap },
                            { "bits", img.bits },
                            { "saved", img.saved } };

        j["extest_images"] = std::move( images );
    }

    if( !aConfig.bsdlDirs.empty() )
    {
        json dirs = json::array();

        for( const std::string& d : aConfig.bsdlDirs )
            dirs.push_back( toStored( d, aFile.parent_path() ) );

        j["bsdl_dirs"] = std::move( dirs );
    }

    if( !aConfig.interconnectInclude.empty() )
        j["interconnect_include"] = aConfig.interconnectInclude;

    fs::path tmp = aFile;
    tmp += ".tmp";

    {
        std::ofstream out( tmp, std::ios::trunc );

        if( !out )
            return std::unexpected( "cannot write " + toUtf8( tmp ) );

        out << j.dump( 2 ) << "\n";

        if( !out )
            return std::unexpected( "error writing " + toUtf8( tmp ) );
    }

    std::error_code ec;
    fs::rename( tmp, aFile, ec );

    if( ec )
        return std::unexpected( "cannot rename " + toUtf8( tmp ) + ": " + ec.message() );

    return {};
}

} // namespace bscan
