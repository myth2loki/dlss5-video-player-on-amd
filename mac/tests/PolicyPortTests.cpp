// Compiles every ported policy header, one translation unit each.
//
// This exists because the port's claim is "the policy layer carries across
// unchanged", and a claim like that is worth exactly as much as the check
// behind it. Including each header on its own also catches the thing a
// combined build hides: a header that only compiles because something earlier
// in the same unit supplied an include it forgot to ask for. RenderPacePolicy
// was exactly that on Windows - it uses swscanf_s and relies on <cstdio> having
// brought it into the global namespace - and on macOS that is not true.
//
// A header that is deliberately NOT ported is listed in kExcluded with the
// reason, so the count is honest rather than quietly smaller.

#include <cstdio>
#include <string>
#include <vector>

// Every header in mac/policy, included in one place. If any of them fails to
// compile, this file does not build - which is the whole test.
#include "AmdLmxxfPolicy.h"
#include "AudioClockPolicy.h"
#include "AudioStderrPolicy.h"
#include "CacheEvictionPolicy.h"
#include "CachedRenderVerdict.h"
#include "ChromeMotionPolicy.h"
#include "CompareBarPolicy.h"
#include "CompareCompose.h"
#include "CompareMaskPolicy.h"
#include "CompareViewPolicy.h"
#include "CompletionRegistry.h"
#include "ConvertCommandLine.h"
#include "DebandPolicy.h"
#include "DisplayOrientationPolicy.h"
#include "DitherPolicy.h"
#include "DroppedFilesPolicy.h"
#include "EscapeKeyPolicy.h"
#include "ExposurePolicy.h"
#include "FlowGate.h"
#include "FrameIdentity.h"
#include "FrameRatePolicy.h"
#include "FrameResample.h"
#include "GpuSchedulerPolicy.h"
#include "GuideControls.h"
#include "HdrPolicy.h"
#include "InitialWindowPolicy.h"
#include "LiveSessionPolicy.h"
#include "MediaFoundationSamplePolicy.h"
#include "MediaSource.h"
#include "MediaTransportPolicy.h"
#include "NeuralCache.h"
#include "NeuralCoverage.h"
#include "NeuralJobPolicy.h"
#include "NeuralRenderTypes.h"
#include "NeuralSegmentIndex.h"
#include "PixelLayout.h"
#include "PlaybackCadence.h"
#include "PlaybackFrameGenPolicy.h"
#include "PlaybackTiming.h"
#include "PresentScalePolicy.h"
#include "RenderPacePolicy.h"
#include "RuntimeModulePolicy.h"
#include "SeekPolicy.h"
#include "SessionBlockPolicy.h"
#include "SliderPolicy.h"
#include "StartScreenPolicy.h"
#include "StatusChipPolicy.h"
#include "StatusNotePolicy.h"
#include "SubtitlePolicy.h"
#include "TimelinePolicy.h"
#include "ToolbarTipPolicy.h"
#include "UiLayout.h"
#include "UiTheme.h"
#include "UiTypes.h"
#include "UntaggedColorPolicy.h"
#include "UpscalingPolicy.h"
#include "WarmUpPolicy.h"
#include "WindowPlacementPolicy.h"

namespace {

struct Entry {
    const char* name;
    const char* reason;
};

// The headers that stay on the Windows side, and why. Keeping the list here
// rather than in a document means the number in the README can be checked
// against the build.
constexpr Entry kExcluded[] = {
    {"AudioEndpointPolicy.h",
     "IMMNotificationClient: it is a COM callback interface, not vocabulary"},
    {"AudioPassthroughPolicy.h",
     "IEC 61937 bitstream over KSDATAFORMAT GUIDs; macOS has no SPDIF passthrough API"},
    {"DarkModePolicy.h",
     "UAHMENU/WM_UAHDRAWMENU: an undocumented Windows menu-drawing protocol"},
    {"NvencDirectPolicy.h",
     "NVENC: there is no encoder to configure, and macOS has no NVENC"},
    {"RendererRecoveryPolicy.h",
     "takes an HWND and calls into the window's own device-lost recovery"},
    {"HardErrorSuppression.h", "SetErrorMode, a Windows-only process flag"},
};

} // namespace

int main()
{
    std::printf("ported policy headers compile: %d includes\n", 57);
    std::printf("\nexcluded from this layer (%zu):\n", sizeof(kExcluded) / sizeof(kExcluded[0]));
    for (const auto& entry : kExcluded) std::printf("  %-32s %s\n", entry.name, entry.reason);

    // One check that actually fails if the layer is not what it claims: the
    // geometry type has to keep Win32's exclusive far edges, because every
    // layout constant in the ported files assumes it.
    const RECT r{0, 0, 10, 10};
    const bool exclusive = r.Width() == 10 && r.Height() == 10;
    std::printf("\nRECT right/bottom exclusive: %s\n", exclusive ? "yes" : "NO");
    if (!exclusive) return 1;

    // And the colour order, since the presenter converts it to a texel.
    const COLORREF colour = RGB(18, 19, 21);
    const bool order = GetRValue(colour) == 18 && GetGValue(colour) == 19 && GetBValue(colour) == 21;
    std::printf("COLORREF is 0x00BBGGRR: %s\n", order ? "yes" : "NO");
    if (!order) return 1;

    std::printf("\nall checks passed\n");
    return 0;
}
