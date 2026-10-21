// SPDX-License-Identifier: GPL-3.0-only
#include "bscan/bsdl_index.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>

#include <nlohmann/json.hpp>

#include "bscan/bsdl_device.h"

#include <bsdl.h>

namespace fs = std::filesystem;
using json   = nlohmann::json;

namespace bscan
{

static constexpr uintmax_t MAX_BSDL_SIZE = 8 * 1024 * 1024;
static constexpr int       CACHE_VERSION = 1;


static fs::path toPath( const std::string& aUtf8 )
{
    return fs::path( std::u8string( aUtf8.begin(), aUtf8.end() ) );
}


static std::string toUtf8( const fs::path& aPath )
{
    std::u8string s = aPath.u8string();
    return std::string( s.begin(), s.end() );
}


static std::string lower( std::string aText )
{
    std::transform( aText.begin(), aText.end(), aText.begin(),
                    []( unsigned char c ) { return static_cast<char>( std::tolower( c ) ); } );
    return aText;
}


static bool isBsdlFile( const fs::path& aPath )
{
    std::string ext = lower( toUtf8( aPath.extension() ) );
    return ext == ".bsd" || ext == ".bsdl" || ext == ".bsm";
}


bool IdcodePatternMatches( const std::string& aPattern, uint32_t aIdcode )
{
    char bits[33];

    for( int i = 0; i < 32; ++i )
        bits[i] = ( aIdcode >> ( 31 - i ) ) & 1 ? '1' : '0';

    bits[32] = '\0';
    return bsdl_bits_match( aPattern.c_str(), bits ) == 1;
}


// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

void BsdlIndex::LoadCache( const std::string& aFile )
{
    std::ifstream in( toPath( aFile ) );

    if( !in )
        return;

    try
    {
        json j = json::parse( in );

        if( j.value( "version", 0 ) != CACHE_VERSION )
            return;

        for( const json& e : j.value( "files", json::array() ) )
        {
            BsdlIndexEntry entry;
            entry.path     = e.value( "path", "" );
            entry.mtime    = e.value( "mtime", int64_t( 0 ) );
            entry.size     = e.value( "size", uint64_t( 0 ) );
            entry.ok       = e.value( "ok", false );
            entry.entity   = e.value( "entity", "" );
            entry.idcode   = e.value( "idcode", "" );
            entry.irLength = e.value( "ir_length", -1 );
            entry.pinMaps  = e.value( "pin_maps", std::vector<std::string>{} );

            if( !entry.path.empty() )
                m_cache[entry.path] = std::move( entry );
        }
    }
    catch( const std::exception& )
    {
        m_cache.clear();   // unreadable cache: start over
    }
}


bool BsdlIndex::SaveCache( const std::string& aFile ) const
{
    json files = json::array();

    for( const auto& [path, e] : m_cache )
    {
        files.push_back( { { "path", e.path },
                           { "mtime", e.mtime },
                           { "size", e.size },
                           { "ok", e.ok },
                           { "entity", e.entity },
                           { "idcode", e.idcode },
                           { "ir_length", e.irLength },
                           { "pin_maps", e.pinMaps } } );
    }

    fs::path        file = toPath( aFile );
    std::error_code ec;
    fs::create_directories( file.parent_path(), ec );

    fs::path tmp = file;
    tmp += ".tmp";

    {
        std::ofstream out( tmp, std::ios::trunc );

        if( !out )
            return false;

        out << json{ { "version", CACHE_VERSION }, { "files", std::move( files ) } }.dump() << "\n";

        if( !out )
            return false;
    }

    fs::rename( tmp, file, ec );
    return !ec;
}


// ---------------------------------------------------------------------------
// Scan
// ---------------------------------------------------------------------------

BsdlIndex::Stats BsdlIndex::Scan( const std::vector<std::string>& aDirs,
                                  const std::function<bool()>& aCancel )
{
    Stats stats;
    m_current.clear();

    for( const std::string& dir : aDirs )
    {
        std::error_code ec;
        fs::path        root = toPath( dir );

        if( dir.empty() || !fs::is_directory( root, ec ) )
            continue;

        auto it = fs::recursive_directory_iterator(
                root, fs::directory_options::skip_permission_denied, ec );

        for( ; !ec && it != fs::recursive_directory_iterator(); it.increment( ec ) )
        {
            if( aCancel && aCancel() )
                return stats;

            const fs::directory_entry& de = *it;
            std::error_code            fec;

            if( !de.is_regular_file( fec ) || !isBsdlFile( de.path() ) )
                continue;

            uintmax_t size = de.file_size( fec );

            if( fec || size > MAX_BSDL_SIZE )
                continue;

            auto mtime = std::chrono::duration_cast<std::chrono::seconds>(
                                 de.last_write_time( fec ).time_since_epoch() )
                                 .count();

            std::string path = toUtf8( fs::absolute( de.path(), fec ).lexically_normal() );

            if( m_current.contains( path ) )
                continue;   // same file through two folders

            m_current.insert( path );
            ++stats.files;

            auto cached = m_cache.find( path );

            if( cached != m_cache.end() && cached->second.mtime == mtime
                && cached->second.size == size )
            {
                stats.failed += cached->second.ok ? 0 : 1;
                continue;
            }

            BsdlIndexEntry entry;
            entry.path  = path;
            entry.mtime = mtime;
            entry.size  = size;

            // Tolerant parsing builds a device from almost anything: a file
            // without entity name is not a BSDL file
            if( auto dev = BsdlDevice::Load( path ); dev && !( *dev )->EntityName().empty() )
            {
                entry.ok       = true;
                entry.entity   = ( *dev )->EntityName();
                entry.idcode   = ( *dev )->IdcodePattern();
                entry.irLength = ( *dev )->IrLength();
                entry.pinMaps  = ( *dev )->PinMapNames();
            }
            else
            {
                ++stats.failed;
            }

            ++stats.parsed;
            m_cache[path] = std::move( entry );
        }
    }

    // Forget cached files that no longer exist
    std::erase_if( m_cache,
                   []( const auto& aItem )
                   {
                       std::error_code ec;
                       return !fs::exists( toPath( aItem.first ), ec );
                   } );

    return stats;
}


std::vector<BsdlIndexEntry> BsdlIndex::Match( uint32_t aIdcode, int aIrLength ) const
{
    std::vector<BsdlIndexEntry> out;

    for( const std::string& path : m_current )
    {
        auto it = m_cache.find( path );

        if( it == m_cache.end() || !it->second.ok )
            continue;

        const BsdlIndexEntry& e = it->second;

        if( !IdcodePatternMatches( e.idcode, aIdcode ) )
            continue;

        if( aIrLength > 0 && e.irLength > 0 && e.irLength != aIrLength )
            continue;

        out.push_back( e );
    }

    return out;
}


// ---------------------------------------------------------------------------
// Ranking
// ---------------------------------------------------------------------------

static bool below( const std::string& aPath, const std::string& aDir )
{
    if( aDir.empty() )
        return false;

    fs::path rel = toPath( aPath ).lexically_relative( toPath( aDir ).lexically_normal() );
    return !rel.empty() && *rel.begin() != "..";
}


std::vector<BsdlSuggestion> RankSuggestions( const std::vector<BsdlIndexEntry>& aMatches,
                                             const std::set<std::string>& aPads,
                                             const std::vector<std::string>& aPreferredDirs )
{
    std::set<std::string> pads;

    for( const std::string& p : aPads )
        pads.insert( lower( p ) );

    std::vector<BsdlSuggestion> out;

    for( const BsdlIndexEntry& e : aMatches )
    {
        bool preferred = std::any_of( aPreferredDirs.begin(), aPreferredDirs.end(),
                                      [&]( const std::string& d ) { return below( e.path, d ); } );

        auto dev = BsdlDevice::Load( e.path );

        if( !dev )
            continue;   // changed since it was indexed

        std::vector<std::string> maps = ( *dev )->PinMapNames();

        if( maps.empty() )
            maps.push_back( "" );

        for( const std::string& map : maps )
        {
            BsdlSuggestion s;
            s.path      = e.path;
            s.entity    = e.entity;
            s.pinMap    = map;
            s.padsTotal = pads.size();
            s.preferred = preferred;

            std::set<std::string> pins;

            for( const std::string& pin : ( *dev )->PinNames( map ) )
                if( !pin.empty() )
                    pins.insert( lower( pin ) );

            s.mapPins = pins.size();

            for( const std::string& p : pads )
                s.padsFound += pins.contains( p ) ? 1 : 0;

            out.push_back( std::move( s ) );
        }
    }

    auto key = []( const BsdlSuggestion& s )
    {
        double share = s.padsTotal ? double( s.padsFound ) / double( s.padsTotal ) : 0.0;
        size_t diff  = s.padsTotal ? ( s.mapPins > s.padsTotal ? s.mapPins - s.padsTotal
                                                               : s.padsTotal - s.mapPins )
                                   : 0;
        return std::make_tuple( !s.preferred, -share, diff, s.path, s.pinMap );
    };

    std::sort( out.begin(), out.end(),
               [&]( const BsdlSuggestion& a, const BsdlSuggestion& b ) { return key( a ) < key( b ); } );
    return out;
}

} // namespace bscan
