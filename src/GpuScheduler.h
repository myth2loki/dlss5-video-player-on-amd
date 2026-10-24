#pragma once

#include "GpuSchedulerPolicy.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace gpu_scheduler {

class GpuScheduler;
struct GpuSchedulerControlState;
struct GpuSchedulerPublishedState;

// A permit is a reservation in the scheduler, not a GPU command or fence. It
// must be released at a job or session boundary. Destroying one is safe and
// idempotent; it never waits for the GPU.
class GpuLease {
public:
    GpuLease() = default;
    ~GpuLease() { Release(); }

    GpuLease(const GpuLease&) = delete;
    GpuLease& operator=(const GpuLease&) = delete;

    GpuLease(GpuLease&& other) noexcept
        : id_(std::exchange(other.id_, 0)),
          release_(std::move(other.release_)),
          commit_(std::move(other.commit_))
    {
    }

    GpuLease& operator=(GpuLease&& other) noexcept
    {
        if (this == &other) return *this;
        Release();
        id_ = std::exchange(other.id_, 0);
        release_ = std::move(other.release_);
        commit_ = std::move(other.commit_);
        return *this;
    }

    bool Valid() const noexcept { return id_ != 0; }
    uint64_t Id() const noexcept { return id_; }

    // Replace the initial estimate with the measured model/workspace size.
    // Safe to call more than once; later measurements update the reservation.
    void Commit(uint64_t actualBytes) noexcept
    {
        if (!Valid() || !commit_) return;
        try {
            commit_(id_, actualBytes);
        } catch (...) {
            // A reservation accounting failure must not unwind through a job.
        }
    }

    void Release() noexcept
    {
        if (!Valid()) return;
        const uint64_t released = std::exchange(id_, 0);
        auto release = std::move(release_);
        commit_ = nullptr;
        if (!release) return;
        try {
            release(released);
        } catch (...) {
            // See Commit: release is bookkeeping, not a throwing operation.
        }
    }

private:
    friend class GpuScheduler;

    GpuLease(uint64_t id,
             std::function<void(uint64_t)> release,
             std::function<void(uint64_t, uint64_t)> commit)
        : id_(id), release_(std::move(release)), commit_(std::move(commit))
    {
    }

    uint64_t id_{};
    std::function<void(uint64_t)> release_;
    std::function<void(uint64_t, uint64_t)> commit_;
};

struct GpuAdmissionResult {
    bool admitted{};
    bool wouldReject{};
    GpuPressure pressure = GpuPressure::Normal;
    GpuRejectReason reason = GpuRejectReason::None;
    GpuLease lease;
};

struct GpuSchedulerReceipt {
    uint64_t observedSamples{};
    uint64_t admittedRequests{};
    uint64_t rejectedRequests{};
    uint64_t wouldRejectRequests{};
    uint64_t committedLeases{};
    uint64_t releasedLeases{};
    uint64_t invalidatedLeases{};
    uint64_t cancelGenerationCalls{};
    uint64_t currentLeaseCount{};
    uint64_t peakReservedBytes{};
    uint64_t peakUsageBytes{};
    GpuPressure maximumPressure = GpuPressure::Normal;
};

class GpuScheduler final {
public:
    explicit GpuScheduler(GpuSchedulerLimits limits = {});
    ~GpuScheduler();

    GpuScheduler(const GpuScheduler&) = delete;
    GpuScheduler& operator=(const GpuScheduler&) = delete;
    GpuScheduler(GpuScheduler&&) = delete;
    GpuScheduler& operator=(GpuScheduler&&) = delete;

    void SetMode(GpuSchedulerMode mode) noexcept;
    GpuSchedulerMode Mode() const noexcept;

    void Observe(const GpuRuntimeSample& sample) noexcept;
    GpuPressureSnapshot Snapshot() const noexcept;

    GpuAdmissionResult TryAcquire(const GpuWorkloadRequest& request);

    void ResetSession(uint64_t generation) noexcept;
    void CancelGeneration(uint64_t generation) noexcept;
    uint64_t Generation() const noexcept;

    GpuSchedulerReceipt Receipt() const;

private:
    std::shared_ptr<GpuSchedulerControlState> state_;
    std::shared_ptr<GpuSchedulerPublishedState> published_;
};

std::string FormatGpuSchedulerLine(const GpuPressureSnapshot& snapshot,
                                   const GpuSchedulerReceipt& receipt);

} // namespace gpu_scheduler
