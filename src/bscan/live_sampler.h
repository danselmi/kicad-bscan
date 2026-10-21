// SPDX-License-Identifier: GPL-3.0-only
// Background thread that talks to OpenOCD:
//  - reads the JTAG chain (on start, after reconnects, on request and every
//    few seconds while idle, to notice OpenOCD going away),
//  - samples the boundary registers of all TAPs that hold one of the target
//    pins (one SAMPLE scan per TAP per cycle) and reports every pin's value,
//  - enters, updates and leaves EXTEST on request. While a TAP is in EXTEST
//    its pins are read with EXTEST scans that keep driving the image.
//
// EXTEST safety rules implemented here:
//  - PRELOAD and EXTEST are issued in one Tcl command,
//  - if the connection to OpenOCD is lost while in EXTEST, EXTEST is left
//    (SAMPLE) as soon as the connection is back, never re-entered,
//  - on Stop() all TAPs in EXTEST are returned to SAMPLE (unless disabled),
//  - with OpenOCD's 'jtag scan', several TAPs can be in EXTEST: every scan
//    names all of them with EXTEST and their image (and the sampled TAPs
//    with SAMPLE), one 'jtag scan' per cycle. Without it, only one TAP can be
//    in EXTEST and no other TAP is scanned meanwhile (each scan of one TAP
//    puts all others into BYPASS),
//  - while in EXTEST, 'jtag cur_ir' (if OpenOCD has it) is checked every
//    cycle before scanning: anything else that scanned the chain since our
//    last scan (target polling, IPDBG, another client) shows up there. EXTEST
//    and the image are restored right away and the interruption is reported.
//
// Callbacks are invoked from the sampler thread and only when something
// changed.
//
// Structure: this class holds the API (requests from the GUI, under a lock)
// and the thread's cycle (run()). The work of a cycle is done by the jobs:
// ExtestJob (extest_job.h), the sampling functions (sample_job.h) and
// InterconnectJob (interconnect_job.h); the TAPs in EXTEST are kept in an
// ExtestTable (extest_table.h).

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "bscan/extest_table.h"
#include "bscan/sampler_context.h"
#include "bscan/sampler_types.h"

namespace bscan
{

class LiveSampler
{
public:
    // The types live in sampler_types.h (shared with the jobs)
    using Target               = SampleTarget;
    using ChainUpdate          = bscan::ChainUpdate;
    using PinResult            = bscan::PinResult;
    using SampleUpdate         = bscan::SampleUpdate;
    using ExtestUpdate         = bscan::ExtestUpdate;
    using Snapshot             = bscan::Snapshot;
    using TakeOver             = bscan::TakeOver;
    using InterconnectRequest  = bscan::InterconnectRequest;
    using Captures             = bscan::Captures;
    using ChainCallback        = bscan::ChainCallback;
    using SampleCallback       = bscan::SampleCallback;
    using ExtestCallback       = bscan::ExtestCallback;
    using SnapshotCallback     = bscan::SnapshotCallback;
    using InterconnectCallback = bscan::InterconnectCallback;

    LiveSampler();
    ~LiveSampler();

    LiveSampler( const LiveSampler& ) = delete;
    LiveSampler& operator=( const LiveSampler& ) = delete;

    void Start( ChainCallback aOnChain, SampleCallback aOnSample );
    void Stop();   // blocks until the thread has finished

    // All setters are thread-safe and wake the thread.
    void SetServer( const std::string& aHost, int aPort );

    // Returns the version number reported back in SampleUpdate
    unsigned SetTargets( std::vector<Target> aTargets );

    void SetInterval( std::chrono::milliseconds aInterval );
    void RefreshChain();

    // --- EXTEST (call SetExtestCallback before Start) ---
    void SetExtestCallback( ExtestCallback aCallback );
    void EnterExtest( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                      const Bits& aImage );
    void UpdateExtest( const std::string& aTap, const Bits& aImage );
    void LeaveExtest( const std::string& aTap );

    // Takes over a TAP that is already in EXTEST (see bscan::TakeOverExtest);
    // reported like EnterExtest(). On failure nothing is changed.
    void TakeOverExtest( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                         const Bits& aImage );

    // Several TAPs at once (needs 'jtag scan'): all must be in EXTEST
    // ('jtag cur_ir'), then one scan drives all images, so none of them is
    // put into BYPASS by the take-over of another. All or none; each is
    // reported like TakeOverExtest().
    void TakeOverExtest( const std::vector<TakeOver>& aTakeOvers );

    // Leaves EXTEST on a TAP this sampler did not enter (e.g. left in EXTEST
    // by an earlier session). Reported like LeaveExtest().
    void ForceLeaveExtest( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice );
    void SetLeaveExtestOnStop( bool aLeave );

    // aCount SAMPLE captures of one TAP, a few ms apart, and whether OpenOCD
    // polls targets in the background. While a TAP is in EXTEST: refused
    // without 'jtag scan'; with it the TAPs in EXTEST are scanned along and
    // polling is not checked ('poll' would interrupt EXTEST).
    // The callback runs in the sampler thread.
    void RequestSnapshot( const TapInfo& aTap, std::shared_ptr<const BsdlDevice> aDevice,
                          int aCount, SnapshotCallback aCallback );

    // --- interconnect test (see bscan/interconnect.h) ---
    // Applies the patterns and reports the captures (in the sampler thread),
    // once, or with aRequest.repeat until StopInterconnect() (one callback
    // per run). Devices not in EXTEST enter it (PRELOAD first) and leave it
    // at the end; devices already in EXTEST get their image back. Other TAPs
    // in EXTEST are scanned along. Several devices need 'jtag scan'. An error
    // ends the run (one callback with the error).
    void RunInterconnect( InterconnectRequest aRequest, InterconnectCallback aCallback );
    void StopInterconnect();

private:
    struct SnapshotRequest
    {
        TapInfo                           tap;
        std::shared_ptr<const BsdlDevice> device;
        int                               count = 1;
        SnapshotCallback                  callback;
    };

    // What one cycle works with (copied from the members under the lock)
    struct Settings
    {
        std::string               host;
        int                       port = 0;
        unsigned                  serverVersion = 0;
        std::vector<Target>       targets;
        unsigned                  targetsVersion = 0;
        std::chrono::milliseconds interval{};
        bool                      chainRequested = false;
    };

    // --- the sampler thread: one cycle after the other ---
    void run();
    bool takeSettings( Settings& aSettings );          // false: stopping
    void ensureClient( Settings& aSettings );          // new server: new client
    bool updateChain( const Settings& aSettings );     // true: connected
    bool trackConnection( bool aConnected );           // true: reconnected
    void runJobs( const Settings& aSettings, bool aReconnected );
    void processSnapshot( const SamplerContext& aCtx );
    void processInterconnect( const SamplerContext& aCtx );
    void sample( const SamplerContext& aCtx, const Settings& aSettings, bool aAnyDirty );
    void reportSample( const SampleUpdate& aUpdate );   // only if changed
    std::chrono::milliseconds nextWait( const Settings& aSettings, bool aConnected ) const;
    void leaveAllExtest( openocd::TclClient& aClient );

    SamplerContext context( openocd::TclClient& aClient );
    void        wake();           // wakes the thread (after a request)
    void        requestChain();   // re-read the chain in the next cycle
    void        reportExtest( const std::string& aTap, ExtestUpdate::State aState,
                       const std::string& aMessage, int aInterruptions = 0 );

    ChainCallback  m_onChain;
    SampleCallback m_onSample;
    ExtestCallback m_onExtest;

    std::thread             m_thread;
    std::mutex              m_mutex;
    std::condition_variable m_wake;
    bool                    m_stop = false;
    bool                    m_wakeup = false;

    // Shared with the GUI, protected by m_mutex
    std::string               m_host = "127.0.0.1";
    int                       m_port = 6666;
    unsigned                  m_serverVersion = 0;
    std::vector<Target>       m_targets;
    unsigned                  m_targetsVersion = 0;
    std::chrono::milliseconds m_interval{ 100 };
    bool                      m_chainRequested = true;

    ExtestTable                        m_extest;
    bool                               m_leaveOnStop = true;
    std::optional<SnapshotRequest>     m_snapshot;
    std::optional<std::pair<InterconnectRequest, InterconnectCallback>> m_interconnect;
    std::atomic<bool>                  m_interconnectStop{ false };

    // Sampler thread only
    std::unique_ptr<openocd::TclClient>   m_client;
    unsigned                              m_clientVersion = ~0u;
    std::optional<ChainUpdate>            m_lastChain;
    std::optional<SampleUpdate>           m_lastSample;
    std::chrono::steady_clock::time_point m_lastChainRead;
    bool                                  m_wasConnected = false;
};

} // namespace bscan
