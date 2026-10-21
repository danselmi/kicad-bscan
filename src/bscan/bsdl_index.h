// SPDX-License-Identifier: GPL-3.0-only
// Index of BSDL files in a set of folders, for suggesting a file for a TAP
// from its IDCODE.
//
// Folders are searched recursively for *.bsd, *.bsdl and *.bsm. Every file
// is parsed once; the results (entity, IDCODE pattern, IR length, pin maps)
// are kept in a cache file and reused while size and modification time of
// the file are unchanged.
//
// Several files usually match one IDCODE (same die, different packages).
// RankSuggestions() orders them by how well a pin map fits the footprint's
// pad numbers; the user always chooses.

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace bscan
{

struct BsdlIndexEntry
{
    std::string              path;          // UTF-8, absolute
    int64_t                  mtime = 0;
    uint64_t                 size  = 0;
    bool                     ok    = false; // parsed successfully
    std::string              entity;
    std::string              idcode;        // 32 chars MSB first, 'X' = don't care; empty: none
    int                      irLength = -1;
    std::vector<std::string> pinMaps;

    bool operator==( const BsdlIndexEntry& ) const = default;
};

// True if aIdcode matches a 32-character pattern of '0', '1' and 'X'
bool IdcodePatternMatches( const std::string& aPattern, uint32_t aIdcode );

class BsdlIndex
{
public:
    struct Stats
    {
        size_t files  = 0;   // BSDL files found
        size_t parsed = 0;   // parsed now (not from the cache)
        size_t failed = 0;   // not parseable
    };

    // Cache handling; a missing or unreadable cache is not an error
    void LoadCache( const std::string& aFile );
    bool SaveCache( const std::string& aFile ) const;

    // Searches aDirs recursively. Afterwards Match() only considers files
    // found in this scan. aCancel is checked between files.
    Stats Scan( const std::vector<std::string>& aDirs,
                const std::function<bool()>& aCancel = {} );

    // Parsed files whose IDCODE pattern matches and (if aIrLength > 0) whose
    // IR length is aIrLength
    std::vector<BsdlIndexEntry> Match( uint32_t aIdcode, int aIrLength ) const;

private:
    std::map<std::string, BsdlIndexEntry> m_cache;     // all known files, by path
    std::set<std::string>                 m_current;   // files of the last scan
};

struct BsdlSuggestion
{
    std::string path;
    std::string entity;
    std::string pinMap;          // empty if the file has no pin map
    size_t      mapPins    = 0;  // pins in the pin map
    size_t      padsFound  = 0;  // footprint pads found in the pin map
    size_t      padsTotal  = 0;  // pads of the footprint (0: unknown)
    bool        preferred  = false;   // below one of aPreferredDirs
};

// One suggestion per (file, pin map), best first:
//   files below aPreferredDirs (project folders) first, then by the share of
//   footprint pads found in the pin map, then by how close the pin count is
//   to the pad count, then by path.
// aPads: pad numbers of the footprint (may be empty).
std::vector<BsdlSuggestion> RankSuggestions( const std::vector<BsdlIndexEntry>& aMatches,
                                             const std::set<std::string>& aPads,
                                             const std::vector<std::string>& aPreferredDirs );

} // namespace bscan
