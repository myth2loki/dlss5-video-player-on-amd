#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

// Pure GPU scheduling policy. Nothing here knows about D3D12, Vulkan, HIP,
// renderers or worker processes, so every threshold can be exercised without a
// GPU. GpuScheduler owns the mutable state and calls these decisions at job
// boundaries; the present path only reads an already-published snapshot.
namespace gpu_scheduler {

enum class GpuWorkClass : uint8_t {
    PlaybackCritical,
    PlaybackSupport,
    Interactive,
    Background,
    Maintenance,
};

enum class GpuWorkKind : uint8_t {
    NeuralRender,
    Upscale,
    FrameGeneration,
    SpeechRecognition,
    Translation,
    Ocr,
    CachePrefetch,
    Maintenance,
};

enum class GpuSchedulerMode : uint8_t {
    Observe,
    Admit,
    Enforce,
};

enum class GpuPressure : uint8_t {
    Normal,
    Soft,
    Hard,
    Critical,
};

enum class GpuRejectReason : uint8_t {
    None,
    WrongAdapter,
    InvalidRequest,
    InsufficientVram,
    Pressure,
    PlaybackProtected,
    Cancelled,
    StaleGeneration,
    DeviceLost,
};

inline const char* WorkClassName(GpuWorkClass value) noexcept
{
    switch (value) {
    case GpuWorkClass::PlaybackCritical: return "playback-critical";
    case GpuWorkClass::PlaybackSupport: return "playback-support";
    case GpuWorkClass::Interactive: return "interactive";
    case GpuWorkClass::Background: return "background";
    case GpuWorkClass::Maintenance: return "maintenance";
    }
    return "unknown";
}

inline const char* WorkKindName(GpuWorkKind value) noexcept
{
    switch (value) {
    case GpuWorkKind::NeuralRender: return "neural-render";
    case GpuWorkKind::Upscale: return "upscale";
    case GpuWorkKind::FrameGeneration: return "frame-generation";
    case GpuWorkKind::SpeechRecognition: return "speech-recognition";
    case GpuWorkKind::Translation: return "translation";
    case GpuWorkKind::Ocr: return "ocr";
    case GpuWorkKind::CachePrefetch: return "cache-prefetch";
    case GpuWorkKind::Maintenance: return "maintenance";
    }
    return "unknown";
}

inline const char* ModeName(GpuSchedulerMode value) noexcept
{
    switch (value) {
    case GpuSchedulerMode::Observe: return "observe";
    case GpuSchedulerMode::Admit: return "admit";
    case GpuSchedulerMode::Enforce: return "enforce";
    }
    return "unknown";
}

inline const char* PressureName(GpuPressure value) noexcept
{
    switch (value) {
    case GpuPressure::Normal: return "normal";
    case GpuPressure::Soft: return "soft";
    case GpuPressure::Hard: return "hard";
    case GpuPressure::Critical: return "critical";
    }
    return "unknown";
}

inline const char* RejectReasonName(GpuRejectReason value) noexcept
{
    switch (value) {
    case GpuRejectReason::None: return "none";
    case GpuRejectReason::WrongAdapter: return "wrong-adapter";
    case GpuRejectReason::InvalidRequest: return "invalid-request";
    case GpuRejectReason::InsufficientVram: return "insufficient-vram";
    case GpuRejectReason::Pressure: return "pressure";
    case GpuRejectReason::PlaybackProtected: return "playback-protected";
    case GpuRejectReason::Cancelled: return "cancelled";
    case GpuRejectReason::StaleGeneration: return "stale-generation";
    case GpuRejectReason::DeviceLost: return "device-lost";
    }
    return "unknown";
}

struct GpuSchedulerLimits {
    double reserveRatio = 0.10;
    uint64_t minimumReserveBytes = 512ull << 20;

    double softVramRatio = 0.78;
    double hardVramRatio = 0.88;
    double criticalVramRatio = 0.94;

    double softFrameRatio = 0.90;
    double hardFrameRatio = 1.00;
    double criticalFrameRatio = 1.20;

    double softLeadSeconds = 6.0;
    double hardLeadSeconds = 3.0;
    double criticalLeadSeconds = 1.0;

    uint32_t softRecoverySamples = 4;
    uint32_t hardRecoverySamples = 4;
    uint32_t criticalRecoverySamples = 6;
};

struct GpuRuntimeSample {
    uint64_t adapterLuid{};
    uint64_t localBudgetBytes{};
    uint64_t localUsageBytes{};
    uint64_t peakLocalUsageBytes{};

    double frameBudgetMs{};
    double frameP50Ms{};
    double frameP95Ms{};
    uint64_t droppedFrames{};

    double neuralRealtimeRatio{};
    double neuralLeadSeconds{};
    bool neuralActive{};

    bool upscaleActive{};
    bool frameGenerationActive{};
    bool rendererUsable{true};

    uint64_t workerLocalUsageBytes{};
    uint64_t workerPeakLocalUsageBytes{};
    uint64_t sampleSequence{};
    int64_t sampleTimeMs{};
};

struct GpuPressureSnapshot {
    uint64_t adapterLuid{};
    uint64_t budgetBytes{};
    uint64_t usageBytes{};
    uint64_t peakUsageBytes{};
    uint64_t reservedBytes{};
    uint64_t sampleSequence{};
    int64_t sampleTimeMs{};
    uint64_t generation{};
    GpuPressure pressure = GpuPressure::Normal;
    GpuSchedulerMode mode = GpuSchedulerMode::Observe;
    bool playbackProtected{};
};

struct GpuWorkloadRequest {
    GpuWorkKind kind = GpuWorkKind::Maintenance;
    GpuWorkClass workClass = GpuWorkClass::Maintenance;
    uint64_t adapterLuid{};
    uint64_t estimatedVramBytes{};
    uint64_t generation{};
    uint32_t estimatedDurationMs{};
    bool cancellable{};
    bool canTimeSlice{};
    bool persistent{};
};

struct GpuAdmissionDecision {
    bool admitted{};
    bool wouldReject{};
    GpuPressure pressure = GpuPressure::Normal;
    GpuRejectReason reason = GpuRejectReason::None;
};

inline uint64_t RequiredReserveBytes(const GpuSchedulerLimits& limits, uint64_t budgetBytes) noexcept
{
    const long double proportional = static_cast<long double>(budgetBytes) *
                                     static_cast<long double>(limits.reserveRatio);
    const uint64_t scaled = proportional <= 0.0L
        ? 0
        : proportional >= static_cast<long double>(UINT64_MAX)
            ? UINT64_MAX
            : static_cast<uint64_t>(proportional);
    return std::max(limits.minimumReserveBytes, scaled);
}

inline GpuPressure ClassifyPressure(const GpuRuntimeSample& sample,
                                    const GpuSchedulerLimits& limits) noexcept
{
    // A budget that cannot be read is not evidence that resources are free.
    if (!sample.rendererUsable || sample.localBudgetBytes == 0)
        return GpuPressure::Critical;

    const double vramRatio = static_cast<double>(sample.localUsageBytes) /
                             static_cast<double>(sample.localBudgetBytes);
    const double frameRatio = sample.frameBudgetMs > 0.0 && std::isfinite(sample.frameP95Ms)
        ? sample.frameP95Ms / sample.frameBudgetMs
        : 0.0;

    GpuPressure pressure = GpuPressure::Normal;
    if (frameRatio >= limits.softFrameRatio || vramRatio >= limits.softVramRatio)
        pressure = GpuPressure::Soft;
    if (frameRatio >= limits.hardFrameRatio || vramRatio >= limits.hardVramRatio)
        pressure = GpuPressure::Hard;
    if (frameRatio >= limits.criticalFrameRatio || vramRatio >= limits.criticalVramRatio)
        pressure = GpuPressure::Critical;

    if (sample.neuralActive) {
        const double lead = std::isfinite(sample.neuralLeadSeconds)
            ? std::max(0.0, sample.neuralLeadSeconds)
            : 0.0;
        if (lead < limits.softLeadSeconds)
            pressure = std::max(pressure, GpuPressure::Soft);
        if (lead < limits.hardLeadSeconds)
            pressure = std::max(pressure, GpuPressure::Hard);
        if (lead < limits.criticalLeadSeconds)
            pressure = GpuPressure::Critical;
    }

    return pressure;
}

inline bool PressureAdmits(GpuPressure pressure, GpuWorkClass workClass) noexcept
{
    if (workClass == GpuWorkClass::PlaybackCritical)
        return true;
    if (pressure == GpuPressure::Critical)
        return false;
    if (pressure == GpuPressure::Hard)
        return workClass == GpuWorkClass::PlaybackSupport;
    if (pressure == GpuPressure::Soft)
        return workClass != GpuWorkClass::Background &&
               workClass != GpuWorkClass::Maintenance;
    return true;
}

inline GpuAdmissionDecision DecideAdmission(GpuSchedulerMode mode,
                                            GpuPressure pressure,
                                            GpuWorkClass workClass) noexcept
{
    GpuAdmissionDecision decision;
    decision.pressure = pressure;
    const bool pressureAllows = PressureAdmits(pressure, workClass);
    decision.wouldReject = !pressureAllows;
    if (mode == GpuSchedulerMode::Observe) {
        decision.admitted = true;
        decision.reason = pressureAllows ? GpuRejectReason::None : GpuRejectReason::Pressure;
        return decision;
    }

    decision.admitted = pressureAllows;
    decision.reason = pressureAllows ? GpuRejectReason::None : GpuRejectReason::Pressure;
    return decision;
}

class PressureTracker {
public:
    GpuPressure Level() const noexcept { return level_; }

    void Reset() noexcept
    {
        level_ = GpuPressure::Normal;
        recoverySamples_ = 0;
    }

    GpuPressure Observe(const GpuRuntimeSample& sample, const GpuSchedulerLimits& limits) noexcept
    {
        const GpuPressure candidate = ClassifyPressure(sample, limits);
        if (candidate > level_) {
            level_ = candidate;
            recoverySamples_ = 0;
        } else if (candidate < level_) {
            ++recoverySamples_;
            const uint32_t needed = level_ == GpuPressure::Critical
                ? limits.criticalRecoverySamples
                : level_ == GpuPressure::Hard
                    ? limits.hardRecoverySamples
                    : limits.softRecoverySamples;
            if (recoverySamples_ >= needed) {
                level_ = static_cast<GpuPressure>(static_cast<uint8_t>(level_) - 1u);
                recoverySamples_ = 0;
            }
        } else {
            recoverySamples_ = 0;
        }
        return level_;
    }

private:
    GpuPressure level_ = GpuPressure::Normal;
    uint32_t recoverySamples_{};
};

} // namespace gpu_scheduler
