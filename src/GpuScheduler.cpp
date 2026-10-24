#include "GpuScheduler.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace gpu_scheduler {

namespace {

constexpr uint64_t kMiB = 1ull << 20;

uint64_t SaturatingAdd(uint64_t left, uint64_t right) noexcept
{
    return right > std::numeric_limits<uint64_t>::max() - left
        ? std::numeric_limits<uint64_t>::max()
        : left + right;
}

uint64_t SaturatingSubtract(uint64_t left, uint64_t right) noexcept
{
    return right > left ? 0 : left - right;
}

struct LeaseRecord {
    GpuWorkClass workClass = GpuWorkClass::Maintenance;
    uint64_t generation{};
    uint64_t reservedBytes{};
    bool committed{};
};

} // namespace

struct GpuSchedulerPublishedState {
    std::atomic<uint64_t> adapterLuid{0};
    std::atomic<uint64_t> budgetBytes{0};
    std::atomic<uint64_t> usageBytes{0};
    std::atomic<uint64_t> peakUsageBytes{0};
    std::atomic<uint64_t> reservedBytes{0};
    std::atomic<uint64_t> sampleSequence{0};
    std::atomic<int64_t> sampleTimeMs{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<uint8_t> pressure{static_cast<uint8_t>(GpuPressure::Normal)};
    std::atomic<uint8_t> mode{static_cast<uint8_t>(GpuSchedulerMode::Observe)};
    std::atomic<bool> playbackProtected{false};
};

struct GpuSchedulerControlState {
    mutable std::mutex mutex;
    GpuSchedulerLimits limits;
    GpuSchedulerMode mode = GpuSchedulerMode::Observe;
    PressureTracker pressure;
    GpuRuntimeSample sample;
    GpuSchedulerReceipt receipt;
    uint64_t generation{};
    uint64_t nextLeaseId{1};
    uint64_t pendingReservationBytes{};
    uint64_t committedReservationBytes{};
    std::unordered_map<uint64_t, LeaseRecord> leases;
    bool shuttingDown{};
};

namespace {

void PublishLocked(const GpuSchedulerControlState& state,
                   GpuSchedulerPublishedState& published) noexcept
{
    const uint64_t reserved = SaturatingAdd(state.pendingReservationBytes,
                                             state.committedReservationBytes);
    const GpuPressure pressure = state.pressure.Level();
    published.adapterLuid.store(state.sample.adapterLuid, std::memory_order_relaxed);
    published.budgetBytes.store(state.sample.localBudgetBytes, std::memory_order_relaxed);
    published.usageBytes.store(state.sample.localUsageBytes, std::memory_order_relaxed);
    published.peakUsageBytes.store(state.sample.peakLocalUsageBytes, std::memory_order_relaxed);
    published.reservedBytes.store(reserved, std::memory_order_relaxed);
    published.sampleSequence.store(state.sample.sampleSequence, std::memory_order_relaxed);
    published.sampleTimeMs.store(state.sample.sampleTimeMs, std::memory_order_relaxed);
    published.generation.store(state.generation, std::memory_order_relaxed);
    published.pressure.store(static_cast<uint8_t>(pressure), std::memory_order_relaxed);
    published.mode.store(static_cast<uint8_t>(state.mode), std::memory_order_relaxed);
    published.playbackProtected.store(pressure == GpuPressure::Critical,
                                      std::memory_order_relaxed);
}

void ReleaseLease(GpuSchedulerControlState& state,
                  GpuSchedulerPublishedState& published,
                  uint64_t leaseId) noexcept
{
    std::lock_guard lock(state.mutex);
    if (state.shuttingDown) return;
    const auto found = state.leases.find(leaseId);
    if (found == state.leases.end()) return;
    const LeaseRecord& record = found->second;
    if (record.committed)
        state.committedReservationBytes =
            SaturatingSubtract(state.committedReservationBytes, record.reservedBytes);
    else
        state.pendingReservationBytes =
            SaturatingSubtract(state.pendingReservationBytes, record.reservedBytes);
    state.leases.erase(found);
    ++state.receipt.releasedLeases;
    state.receipt.currentLeaseCount = state.leases.size();
    PublishLocked(state, published);
}

void CommitLease(GpuSchedulerControlState& state,
                 GpuSchedulerPublishedState& published,
                 uint64_t leaseId,
                 uint64_t actualBytes) noexcept
{
    if (actualBytes == 0) return;
    std::lock_guard lock(state.mutex);
    if (state.shuttingDown) return;
    const auto found = state.leases.find(leaseId);
    if (found == state.leases.end()) return;
    LeaseRecord& record = found->second;
    if (!record.committed) {
        state.pendingReservationBytes =
            SaturatingSubtract(state.pendingReservationBytes, record.reservedBytes);
        record.committed = true;
    } else {
        state.committedReservationBytes =
            SaturatingSubtract(state.committedReservationBytes, record.reservedBytes);
    }
    record.reservedBytes = actualBytes;
    state.committedReservationBytes =
        SaturatingAdd(state.committedReservationBytes, actualBytes);
    ++state.receipt.committedLeases;
    state.receipt.peakReservedBytes = std::max(
        state.receipt.peakReservedBytes,
        SaturatingAdd(state.pendingReservationBytes, state.committedReservationBytes));
    PublishLocked(state, published);
}

} // namespace

GpuScheduler::GpuScheduler(GpuSchedulerLimits limits)
    : state_(std::make_shared<GpuSchedulerControlState>()),
      published_(std::make_shared<GpuSchedulerPublishedState>())
{
    state_->limits = limits;
    std::lock_guard lock(state_->mutex);
    PublishLocked(*state_, *published_);
}

GpuScheduler::~GpuScheduler()
{
    std::lock_guard lock(state_->mutex);
    state_->shuttingDown = true;
    state_->leases.clear();
    state_->pendingReservationBytes = 0;
    state_->committedReservationBytes = 0;
    state_->receipt.currentLeaseCount = 0;
    PublishLocked(*state_, *published_);
}

void GpuScheduler::SetMode(GpuSchedulerMode mode) noexcept
{
    std::lock_guard lock(state_->mutex);
    if (state_->shuttingDown) return;
    state_->mode = mode;
    PublishLocked(*state_, *published_);
}

GpuSchedulerMode GpuScheduler::Mode() const noexcept
{
    return static_cast<GpuSchedulerMode>(
        published_->mode.load(std::memory_order_relaxed));
}

void GpuScheduler::Observe(const GpuRuntimeSample& sample) noexcept
{
    std::lock_guard lock(state_->mutex);
    if (state_->shuttingDown) return;
    if (sample.sampleSequence != 0 &&
        state_->sample.sampleSequence != 0 &&
        sample.sampleSequence <= state_->sample.sampleSequence)
        return;

    const uint64_t previousAdapter = state_->sample.adapterLuid;
    if (previousAdapter != 0 && sample.adapterLuid != 0 &&
        previousAdapter != sample.adapterLuid) {
        state_->receipt.invalidatedLeases += state_->leases.size();
        state_->leases.clear();
        state_->pendingReservationBytes = 0;
        state_->committedReservationBytes = 0;
        state_->receipt.currentLeaseCount = 0;
    }

    state_->sample = sample;
    state_->pressure.Observe(sample, state_->limits);
    ++state_->receipt.observedSamples;
    state_->receipt.peakUsageBytes =
        std::max(state_->receipt.peakUsageBytes, sample.peakLocalUsageBytes);
    state_->receipt.maximumPressure =
        std::max(state_->receipt.maximumPressure, state_->pressure.Level());
    PublishLocked(*state_, *published_);
}

GpuPressureSnapshot GpuScheduler::Snapshot() const noexcept
{
    GpuPressureSnapshot snapshot;
    snapshot.adapterLuid = published_->adapterLuid.load(std::memory_order_relaxed);
    snapshot.budgetBytes = published_->budgetBytes.load(std::memory_order_relaxed);
    snapshot.usageBytes = published_->usageBytes.load(std::memory_order_relaxed);
    snapshot.peakUsageBytes = published_->peakUsageBytes.load(std::memory_order_relaxed);
    snapshot.reservedBytes = published_->reservedBytes.load(std::memory_order_relaxed);
    snapshot.sampleSequence = published_->sampleSequence.load(std::memory_order_relaxed);
    snapshot.sampleTimeMs = published_->sampleTimeMs.load(std::memory_order_relaxed);
    snapshot.generation = published_->generation.load(std::memory_order_relaxed);
    snapshot.pressure = static_cast<GpuPressure>(
        published_->pressure.load(std::memory_order_relaxed));
    snapshot.mode = static_cast<GpuSchedulerMode>(
        published_->mode.load(std::memory_order_relaxed));
    snapshot.playbackProtected =
        published_->playbackProtected.load(std::memory_order_relaxed);
    return snapshot;
}

GpuAdmissionResult GpuScheduler::TryAcquire(const GpuWorkloadRequest& request)
{
    std::lock_guard lock(state_->mutex);
    GpuAdmissionResult result;
    result.pressure = state_->pressure.Level();

    if (state_->shuttingDown || !state_->sample.rendererUsable ||
        state_->sample.localBudgetBytes == 0) {
        result.reason = GpuRejectReason::DeviceLost;
        ++state_->receipt.rejectedRequests;
        return result;
    }
    if (request.estimatedVramBytes == 0 || request.adapterLuid == 0) {
        result.reason = GpuRejectReason::InvalidRequest;
        ++state_->receipt.rejectedRequests;
        return result;
    }
    if (request.generation != state_->generation) {
        result.reason = GpuRejectReason::StaleGeneration;
        ++state_->receipt.rejectedRequests;
        return result;
    }
    if (state_->sample.adapterLuid == 0 ||
        request.adapterLuid != state_->sample.adapterLuid) {
        result.reason = GpuRejectReason::WrongAdapter;
        ++state_->receipt.rejectedRequests;
        return result;
    }

    GpuAdmissionDecision decision = DecideAdmission(
        state_->mode, result.pressure, request.workClass);
    result.wouldReject = decision.wouldReject;
    result.reason = decision.reason;

    const uint64_t pending = SaturatingAdd(state_->pendingReservationBytes,
                                           request.estimatedVramBytes);
    const uint64_t baseUsage = std::max(
        state_->sample.localUsageBytes, state_->committedReservationBytes);
    const uint64_t projected = SaturatingAdd(baseUsage, pending);
    const uint64_t reserve = RequiredReserveBytes(
        state_->limits, state_->sample.localBudgetBytes);
    const bool fits = projected <= state_->sample.localBudgetBytes &&
                      reserve <= state_->sample.localBudgetBytes - projected;
    if (!fits) {
        result.wouldReject = true;
        if (state_->mode == GpuSchedulerMode::Observe) {
            if (!decision.wouldReject)
                result.reason = GpuRejectReason::InsufficientVram;
        } else if (request.workClass == GpuWorkClass::PlaybackCritical) {
            result.reason = GpuRejectReason::PlaybackProtected;
        } else {
            decision.admitted = false;
            result.reason = GpuRejectReason::InsufficientVram;
        }
    }

    result.admitted = decision.admitted;
    if (!result.admitted) {
        ++state_->receipt.rejectedRequests;
        if (result.wouldReject) ++state_->receipt.wouldRejectRequests;
        return result;
    }

    const uint64_t leaseId = state_->nextLeaseId++;
    LeaseRecord record;
    record.workClass = request.workClass;
    record.generation = request.generation;
    record.reservedBytes = request.estimatedVramBytes;
    record.committed = false;
    state_->leases.emplace(leaseId, record);
    state_->pendingReservationBytes =
        SaturatingAdd(state_->pendingReservationBytes, request.estimatedVramBytes);
    state_->receipt.currentLeaseCount = state_->leases.size();
    state_->receipt.peakReservedBytes = std::max(
        state_->receipt.peakReservedBytes,
        SaturatingAdd(state_->pendingReservationBytes,
                      state_->committedReservationBytes));
    ++state_->receipt.admittedRequests;
    if (result.wouldReject) ++state_->receipt.wouldRejectRequests;

    try {
        auto state = state_;
        auto published = published_;
        result.lease = GpuLease(
            leaseId,
            [state, published](uint64_t id) {
                ReleaseLease(*state, *published, id);
            },
            [state, published](uint64_t id, uint64_t actualBytes) {
                CommitLease(*state, *published, id, actualBytes);
            });
    } catch (...) {
        // The reservation was inserted before the lease object's callbacks were
        // allocated. Roll it back rather than leaving unpaid budget behind.
        state_->leases.erase(leaseId);
        state_->pendingReservationBytes =
            SaturatingSubtract(state_->pendingReservationBytes, request.estimatedVramBytes);
        state_->receipt.currentLeaseCount = state_->leases.size();
        --state_->receipt.admittedRequests;
        if (result.wouldReject && state_->receipt.wouldRejectRequests)
            --state_->receipt.wouldRejectRequests;
        PublishLocked(*state_, *published_);
        throw;
    }
    PublishLocked(*state_, *published_);
    return result;
}

void GpuScheduler::ResetSession(uint64_t generation) noexcept
{
    std::lock_guard lock(state_->mutex);
    if (state_->shuttingDown) return;
    state_->generation = generation;
    state_->pressure.Reset();
    PublishLocked(*state_, *published_);
}

void GpuScheduler::CancelGeneration(uint64_t generation) noexcept
{
    std::lock_guard lock(state_->mutex);
    if (state_->shuttingDown) return;
    ++state_->receipt.cancelGenerationCalls;
    if (state_->generation == generation)
        ++state_->generation;
    PublishLocked(*state_, *published_);
}

uint64_t GpuScheduler::Generation() const noexcept
{
    return published_->generation.load(std::memory_order_relaxed);
}

GpuSchedulerReceipt GpuScheduler::Receipt() const
{
    std::lock_guard lock(state_->mutex);
    GpuSchedulerReceipt receipt = state_->receipt;
    receipt.currentLeaseCount = state_->leases.size();
    receipt.maximumPressure = std::max(receipt.maximumPressure, state_->pressure.Level());
    receipt.peakReservedBytes = std::max(
        receipt.peakReservedBytes,
        SaturatingAdd(state_->pendingReservationBytes,
                      state_->committedReservationBytes));
    receipt.peakUsageBytes = std::max(receipt.peakUsageBytes, state_->sample.peakLocalUsageBytes);
    return receipt;
}

std::string FormatGpuSchedulerLine(const GpuPressureSnapshot& snapshot,
                                   const GpuSchedulerReceipt& receipt)
{
    std::ostringstream out;
    out << "GPU scheduler: mode=" << ModeName(snapshot.mode)
        << " pressure=" << PressureName(snapshot.pressure)
        << " vram=" << (snapshot.usageBytes / kMiB)
        << '/' << (snapshot.budgetBytes / kMiB) << "MiB"
        << " reserved=" << (snapshot.reservedBytes / kMiB) << "MiB"
        << " leases=" << receipt.currentLeaseCount
        << " rejected=" << receipt.rejectedRequests
        << " would_reject=" << receipt.wouldRejectRequests
        << " generation=" << snapshot.generation;
    return out.str();
}

} // namespace gpu_scheduler
