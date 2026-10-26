#include "FrameGenBackend.h"

#include "AmdFrameGenBackend.h"
#include "DLSSGBackend.h"
#include "FsrFrameGenBackend.h"

#include <memory>

// Backend selection, in the order the adapters are tried.
//
// NVIDIA adapters keep DLSS-G (NVidia NGX): that path is measured on an RTX 5090
// and is the only one that admits the feature there.
//
// Everything else gets the self-contained matcher (AmdFrameGenBackend): it needs
// no runtime at all, so a build without the AMD SDK still converts. This header
// used to say everything else preferred FidelityFX when its runtime was on disk;
// the code never did, and the probe below was reporting FidelityFX while the pass
// ran the matcher. That gap is what the explicit `Interpolate` kind closes: a
// caller that wants the interpolator asks for it and gets the one its adapter
// has, and a caller that asks for the adapter's default still gets the matcher it
// has always got rather than a silent change of generator.
//
// An explicit request does not consult the adapter. A backend that cannot run
// here reports that through Initialize's own detail (a missing runtime, a driver
// refusal) instead of being swapped for a different generator behind the caller's
// back - the same rule the rest of this player follows for a requested stage.
std::unique_ptr<IFrameGenBackend> CreateFrameGenBackend(bool preferNvidia, FrameGenBackendKind kind)
{
    // Interpolate names the JOB - one generated frame per source pair, on the
    // content - and each vendor has the generator for it: FidelityFX on an AMD
    // card, DLSS-G on an NVIDIA one. An NVIDIA machine has no FidelityFX runtime
    // to offer, so the fallback is not a second choice there; it is the
    // interpolator that machine has.
    if (kind == FrameGenBackendKind::Interpolate) {
        if (preferNvidia) return std::make_unique<DLSSGBackend>();
        return std::make_unique<FsrFrameGenBackend>();
    }
    if (preferNvidia) return std::make_unique<DLSSGBackend>();
    return std::make_unique<AmdFrameGenBackend>();
}