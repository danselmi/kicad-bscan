// SPDX-License-Identifier: GPL-3.0-only
// Per-project settings, stored as <project>.bscan.json in the KiCad project
// directory:
//
// {
//   "version": 1,
//   "openocd": { "host": "127.0.0.1", "port": 6666 },
//   "taps": [
//     { "tap": "xc7.tap", "idcode": "0x037c4093", "reference": "U1",
//       "bsdl": "bsdl/xc7s25_csga225.bsd", "pin_map": "CSGA225",
//       "extest_start": { "D5": "z", "E1": "0" } }
//   ],
//   "watch": [
//     { "reference": "U1", "pad": "D1", "net": "BTN0" }
//   ],
//   "bsdl_dirs": [ "bsdl" ],
//   "interconnect_include": [ "SDA" ]
// }
//
// BSDL paths below the project directory are stored relative to it, others
// absolutely. Unknown keys are ignored when loading.

#pragma once

#include <cstdint>
#include <expected>
#include <map>
#include <string>
#include <vector>

namespace bscan
{

struct TapAssignment
{
    std::string tap;          // OpenOCD TAP name, e.g. "xc7.tap"
    uint32_t    idcode = 0;   // IDCODE seen when the assignment was made (0: unknown)
    std::string reference;    // KiCad reference designator, e.g. "U1"
    std::string bsdl;         // absolute path (resolved on load)
    std::string pinMap;       // empty: BSDL default

    // EXTEST starting state per pin ("z", "0", "1", "keep"); pins not listed
    // use the BSDL safe values
    std::map<std::string, std::string> extestStart;

    bool operator==( const TapAssignment& ) const = default;
};

// A pin on the watch list. Identified by reference and pad number, which
// stay valid across KiCad sessions (pad KIIDs do too, but are not readable).
struct WatchEntry
{
    std::string reference;
    std::string pad;
    std::string net;          // as seen when added, for display

    bool operator==( const WatchEntry& ) const = default;
};

// Image driven when the window was closed with EXTEST kept active, for
// "Take over" in the next session
struct SavedExtestImage
{
    std::string bsdl;     // absolute path (resolved on load)
    std::string pinMap;
    std::string bits;     // boundary register, MSB first ('0'/'1')
    std::string saved;    // when, for display (local time)

    bool operator==( const SavedExtestImage& ) const = default;
};

struct ProjectConfig
{
    std::string                host = "127.0.0.1";
    int                        port = 6666;
    std::vector<TapAssignment> taps;
    std::vector<WatchEntry>    watch;

    // TAPs that were (probably) left in EXTEST when the window was closed
    std::vector<std::string>   extestLeft;
    std::map<std::string, SavedExtestImage> extestImages;   // by TAP name

    // Folders searched for BSDL files (absolute; stored relative to the
    // project directory if below it), in addition to the user's folders
    std::vector<std::string>   bsdlDirs;

    // Interconnect test: nets with other parts that are tested anyway
    std::vector<std::string>   interconnectInclude;   // sorted

    TapAssignment*       Find( const std::string& aTap );
    const TapAssignment* Find( const std::string& aTap ) const;

    // Assignment whose reference matches (case-insensitive), or nullptr
    const TapAssignment* FindByReference( const std::string& aReference ) const;

    bool operator==( const ProjectConfig& ) const = default;
};

// All paths are UTF-8 strings.
std::string ProjectConfigPath( const std::string& aProjectDir, const std::string& aProjectName );

// A missing file yields the defaults (not an error).
std::expected<ProjectConfig, std::string> LoadProjectConfig( const std::string& aFile );

// Writes atomically (temporary file + rename).
std::expected<void, std::string> SaveProjectConfig( const std::string&   aFile,
                                                    const ProjectConfig& aConfig );

} // namespace bscan
