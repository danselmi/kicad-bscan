// SPDX-License-Identifier: GPL-3.0-only
// Polls KiCad in a background thread and reports the current selection.
//
// The KiCad IPC API (10.0) has no selection-changed event, so the selection is
// queried periodically. Only changes are reported. Requests that KiCad answers
// with "busy" are skipped silently; the last state stays valid.
//
// Pad -> footprint: KiCad >= 10.0.6 reports a pad's parent footprint directly.
// For older versions (KiCad 9) the watcher builds a table pad id -> footprint
// from all footprints of the board. The same table provides the list of all
// reference designators (for checking user input).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "kicad/ipc_client.h"

namespace kicad
{

struct PadInfo
{
    std::string padId;        // KIID of the pad
    std::string number;       // pad number / ball name, e.g. "A12"
    std::string net;          // net name, empty if unconnected
    std::string footprintId;  // KIID of the parent footprint
    std::string reference;    // reference designator, e.g. "U3"; empty if unknown

    bool operator==( const PadInfo& ) const = default;
};

struct BoardState
{
    enum class Status
    {
        Disconnected,       // KiCad not reachable
        NoBoard,            // KiCad reachable, no board open in the PCB editor
        NothingSelected,    // no pad and no footprint selected
        MultiplePads,
        PadSelected,        // exactly one pad
        FootprintSelected   // no pad, exactly one footprint
    };

    Status      status = Status::Disconnected;
    std::string boardFile;     // file name of the open board, e.g. "demo.kicad_pcb"
    std::string projectName;   // KiCad project name, if reported
    std::string projectPath;   // directory of the project, if reported
    int         padCount = 0;  // number of selected pads
    PadInfo     pad;           // valid if status == PadSelected
    std::vector<PadInfo> pads; // all selected pads (PadSelected, MultiplePads)
    std::string footprintRef;  // PadSelected: pad's footprint; FootprintSelected: that one
    std::string detail;        // human-readable note (error text, hints)

    // All reference designators of the board, sorted. Filled once per board
    // and refreshed on RefreshReferences() or when an unknown pad shows up.
    std::vector<std::string> references;

    // Net of every pad, by reference and pad number (same refresh as references)
    std::map<std::string, std::map<std::string, std::string>> padNets;

    bool operator==( const BoardState& ) const = default;
};

const char* ToString( BoardState::Status aStatus );

class BoardWatcher
{
public:
    // Called from the watcher thread whenever the state changes.
    using Callback = std::function<void( const BoardState& )>;

    BoardWatcher( Client::Config aConfig, std::chrono::milliseconds aInterval );
    ~BoardWatcher();

    BoardWatcher( const BoardWatcher& ) = delete;
    BoardWatcher& operator=( const BoardWatcher& ) = delete;

    void Start( Callback aCallback );
    void Stop();   // blocks until the thread has finished

    // Re-reads all footprints (e.g. after the user added parts). Thread-safe.
    void RefreshReferences();

    // Selects a net in the PCB editor: its pads, tracks, arcs and vias,
    // replacing the selection (KiCad's API has no "highlight net"). Empty:
    // clear the selection. Done by the polling thread. Thread-safe.
    void SelectNet( std::string aNet );

    // Polls synchronously until KiCad answers (used by the probe tool).
    BoardState PollOnce();

private:
    struct FootprintEntry
    {
        std::string id;
        std::string reference;
    };

    void run();
    bool poll( BoardState& aState );   // false: cycle skipped, state unchanged
    bool refreshDocument( BoardState& aState );

    // Reads all footprints. Returns false if KiCad was busy or gone; aError
    // then tells whether the connection is lost.
    bool loadFootprints( std::optional<Error>& aError );

    std::string referenceForPad( const std::string& aPadId, const std::string& aFootprintId,
                                 bool& aBusy, std::optional<Error>& aError );

    Client                    m_client;
    std::chrono::milliseconds m_interval;
    Callback                  m_callback;

    std::thread             m_thread;
    std::atomic<bool>       m_stop{ false };
    std::atomic<bool>       m_refreshRefs{ false };

    std::mutex                 m_selectMutex;
    std::optional<std::string> m_selectNet;
    void selectNet();   // polling thread
    std::mutex              m_waitMutex;
    std::condition_variable m_wake;

    // Only touched by the polling thread (or PollOnce)
    BoardState                            m_state;
    bool                                  m_haveDocument = false;
    std::string                           m_docFile;
    std::string                           m_docProjectName;
    std::string                           m_docProjectPath;
    std::chrono::steady_clock::time_point m_docCheckedAt;

    bool                                  m_footprintsLoaded = false;
    std::chrono::steady_clock::time_point m_footprintsLoadedAt;
    std::map<std::string, FootprintEntry> m_padToFootprint;   // pad id -> footprint
    std::map<std::pair<std::string, std::string>, std::string> m_padIds;   // (ref, pad) -> pad id
    std::map<std::string, std::string>    m_footprintRefs;    // footprint id -> reference
    std::vector<std::string>              m_references;       // sorted
    std::map<std::string, std::map<std::string, std::string>> m_padNets;
};

} // namespace kicad
