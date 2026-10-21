// SPDX-License-Identifier: GPL-3.0-only
// Reading pins in the sampler thread: one cycle of LiveSampler (all
// targets), and the snapshot before entering EXTEST.

#pragma once

#include <vector>

#include "bscan/sampler_context.h"

namespace bscan
{

// One cycle with 'jtag scan': all TAPs in EXTEST (with their image, so they
// stay in EXTEST and changed images are applied) and all sampled TAPs
// (SAMPLE) in one scan.
SampleUpdate SampleTogether( const SamplerContext& aCtx, const std::vector<SampleTarget>& aTargets,
                             unsigned aTargetsVersion );

// One cycle without 'jtag scan': one scan per TAP. A TAP in EXTEST is read
// with EXTEST scans (keeps driving its image). While a TAP is held in
// EXTEST no other TAP is scanned: that would put it into BYPASS.
SampleUpdate SampleEach( const SamplerContext& aCtx, const std::vector<SampleTarget>& aTargets,
                         unsigned aTargetsVersion );

// aCount SAMPLE captures of one TAP, a few ms apart, and whether OpenOCD
// polls targets in the background (see LiveSampler::RequestSnapshot).
Result<Snapshot> TakeSnapshot( const SamplerContext& aCtx, const TapInfo& aTap,
                               const BsdlDevice& aDevice, int aCount );

} // namespace bscan
