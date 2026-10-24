#include "GpuScheduler.h"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

void Check(bool condition, std::string_view expression, int line)
{
    if (condition) return;
    ++failures;
    std::cerr << "line " << line << ": CHECK failed: " << expression << '\n';
}

template <typename Expected, typename Actual>
void CheckEqual(const Expected& expected, const Actual& actual,
                std::string_view expectedExpression, std::string_view actualExpression,
                int line)
{
    if (expected == actual) return;
    ++failures;
    std::cerr << "line " << line << ": CHECK_EQ failed: "
              << expectedExpression << " != " << actualExpression << '\n';
}

#define CHECK(expression) Check((expression), #expression, __LINE__)
#define CHECK_EQ(expected, actual) \
    CheckEqual((expected), (actual), #expected, #actual, __LINE__)

using namespace gpu_scheduler;

constexpr uint64_t MiB = 1ull << 20;

GpuSchedulerLimits NoReserveLimits()
{
    GpuSchedulerLimits limits;
    limits.reserveRatio = 0.0;
    limits.minimumReserveBytes = 0;
    return limits;
}

GpuRuntimeSample Sample(uint64_t sequence = 1)
{
    GpuRuntimeSample sample;
    sample.adapterLuid = 0x1234;
    sample.localBudgetBytes = 1000 * MiB;
    sample.localUsageBytes = 100 * MiB;
    sample.peakLocalUsageBytes = sample.localUsageBytes;
    sample.frameBudgetMs = 41.7;
    sample.frameP50Ms = 8.0;
    sample.frameP95Ms = 10.0;
    sample.sampleSequence = sequence;
    sample.sampleTimeMs = static_cast<int64_t>(sequence);
    return sample;
}

GpuWorkloadRequest Request(GpuWorkClass workClass = GpuWorkClass::Background,
                           uint64_t bytes = 100 * MiB,
                           uint64_t generation = 1)
{
    GpuWorkloadRequest request;
    request.kind = GpuWorkKind::Translation;
    request.workClass = workClass;
    request.adapterLuid = 0x1234;
    request.estimatedVramBytes = bytes;
    request.generation = generation;
    return request;
}

void pressure_thresholds_are_ordered_test()
{
    const GpuSchedulerLimits limits = NoReserveLimits();
    GpuRuntimeSample sample = Sample();
    CHECK_EQ(GpuPressure::Normal, ClassifyPressure(sample, limits));

    sample.localUsageBytes = 800 * MiB;
    CHECK_EQ(GpuPressure::Soft, ClassifyPressure(sample, limits));

    sample.localUsageBytes = 890 * MiB;
    CHECK_EQ(GpuPressure::Hard, ClassifyPressure(sample, limits));

    sample.localUsageBytes = 950 * MiB;
    CHECK_EQ(GpuPressure::Critical, ClassifyPressure(sample, limits));
}

void frame_and_neural_lead_pressure_are_combined_test()
{
    const GpuSchedulerLimits limits = NoReserveLimits();
    GpuRuntimeSample sample = Sample();
    sample.frameP95Ms = 39.0;
    CHECK_EQ(GpuPressure::Soft, ClassifyPressure(sample, limits));

    sample.frameP95Ms = 10.0;
    sample.neuralActive = true;
    sample.neuralLeadSeconds = 5.0;
    CHECK_EQ(GpuPressure::Soft, ClassifyPressure(sample, limits));

    sample.neuralLeadSeconds = 2.0;
    CHECK_EQ(GpuPressure::Hard, ClassifyPressure(sample, limits));

    sample.neuralLeadSeconds = 0.5;
    CHECK_EQ(GpuPressure::Critical, ClassifyPressure(sample, limits));
}

void missing_budget_and_lost_device_are_critical_test()
{
    const GpuSchedulerLimits limits = NoReserveLimits();
    GpuRuntimeSample sample = Sample();
    sample.localBudgetBytes = 0;
    CHECK_EQ(GpuPressure::Critical, ClassifyPressure(sample, limits));

    sample = Sample();
    sample.rendererUsable = false;
    CHECK_EQ(GpuPressure::Critical, ClassifyPressure(sample, limits));
}

void pressure_recovery_uses_hysteresis_and_steps_down_test()
{
    GpuSchedulerLimits limits = NoReserveLimits();
    limits.criticalRecoverySamples = 2;
    limits.hardRecoverySamples = 2;
    limits.softRecoverySamples = 2;
    PressureTracker tracker;

    GpuRuntimeSample sample = Sample(1);
    sample.localUsageBytes = 950 * MiB;
    CHECK_EQ(GpuPressure::Critical, tracker.Observe(sample, limits));

    sample = Sample(2);
    CHECK_EQ(GpuPressure::Critical, tracker.Observe(sample, limits));
    sample = Sample(3);
    CHECK_EQ(GpuPressure::Hard, tracker.Observe(sample, limits));
    sample = Sample(4);
    CHECK_EQ(GpuPressure::Hard, tracker.Observe(sample, limits));
    sample = Sample(5);
    CHECK_EQ(GpuPressure::Soft, tracker.Observe(sample, limits));
}

void admission_policy_respects_work_class_test()
{
    CHECK(PressureAdmits(GpuPressure::Normal, GpuWorkClass::Background));
    CHECK(!PressureAdmits(GpuPressure::Soft, GpuWorkClass::Background));
    CHECK(PressureAdmits(GpuPressure::Soft, GpuWorkClass::Interactive));
    CHECK(!PressureAdmits(GpuPressure::Hard, GpuWorkClass::Interactive));
    CHECK(PressureAdmits(GpuPressure::Hard, GpuWorkClass::PlaybackSupport));
    CHECK(!PressureAdmits(GpuPressure::Critical, GpuWorkClass::PlaybackSupport));
    CHECK(PressureAdmits(GpuPressure::Critical, GpuWorkClass::PlaybackCritical));
}

void observe_mode_reports_would_reject_without_rejecting_test()
{
    const GpuAdmissionDecision decision = DecideAdmission(
        GpuSchedulerMode::Observe, GpuPressure::Soft, GpuWorkClass::Background);
    CHECK(decision.admitted);
    CHECK(decision.wouldReject);
    CHECK_EQ(GpuRejectReason::Pressure, decision.reason);
}

void lease_reserves_commits_and_releases_vram_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.SetMode(GpuSchedulerMode::Admit);
    scheduler.ResetSession(1);
    scheduler.Observe(Sample());

    GpuAdmissionResult admitted = scheduler.TryAcquire(Request());
    CHECK(admitted.admitted);
    CHECK(admitted.lease.Valid());
    CHECK_EQ(uint64_t{100} * MiB, scheduler.Snapshot().reservedBytes);

    admitted.lease.Commit(250 * MiB);
    CHECK_EQ(uint64_t{250} * MiB, scheduler.Snapshot().reservedBytes);

    admitted.lease.Release();
    CHECK_EQ(uint64_t{0}, scheduler.Snapshot().reservedBytes);
    CHECK_EQ(uint64_t{1}, scheduler.Receipt().releasedLeases);
}

void admission_rejects_when_vram_does_not_fit_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.SetMode(GpuSchedulerMode::Admit);
    scheduler.ResetSession(1);
    GpuRuntimeSample sample = Sample();
    sample.localUsageBytes = 950 * MiB;
    scheduler.Observe(sample);

    GpuAdmissionResult result = scheduler.TryAcquire(Request(GpuWorkClass::Interactive));
    CHECK(!result.admitted);
    CHECK_EQ(GpuRejectReason::InsufficientVram, result.reason);
}

void multiple_leases_account_independently_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.SetMode(GpuSchedulerMode::Admit);
    scheduler.ResetSession(1);
    scheduler.Observe(Sample());

    GpuAdmissionResult first = scheduler.TryAcquire(Request());
    GpuAdmissionResult second = scheduler.TryAcquire(Request());
    CHECK(first.admitted);
    CHECK(second.admitted);
    CHECK_EQ(uint64_t{200} * MiB, scheduler.Snapshot().reservedBytes);

    first.lease.Release();
    CHECK_EQ(uint64_t{100} * MiB, scheduler.Snapshot().reservedBytes);
    second.lease.Release();
    CHECK_EQ(uint64_t{0}, scheduler.Snapshot().reservedBytes);
}

void safety_reserve_is_enforced_even_when_the_raw_budget_allows_the_request_test()
{
    GpuSchedulerLimits limits = NoReserveLimits();
    limits.minimumReserveBytes = 512 * MiB;
    GpuScheduler scheduler(limits);
    scheduler.SetMode(GpuSchedulerMode::Admit);
    scheduler.ResetSession(1);
    GpuRuntimeSample sample = Sample();
    sample.localUsageBytes = 100 * MiB;
    scheduler.Observe(sample);

    GpuAdmissionResult result = scheduler.TryAcquire(
        Request(GpuWorkClass::Interactive, 400 * MiB));
    CHECK(!result.admitted);
    CHECK_EQ(GpuRejectReason::InsufficientVram, result.reason);
}

void invalid_zero_byte_request_is_rejected_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.SetMode(GpuSchedulerMode::Admit);
    scheduler.ResetSession(1);
    scheduler.Observe(Sample());

    GpuAdmissionResult result = scheduler.TryAcquire(
        Request(GpuWorkClass::Interactive, 0));
    CHECK(!result.admitted);
    CHECK_EQ(GpuRejectReason::InvalidRequest, result.reason);
}

void observe_mode_admits_but_marks_the_same_pressure_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.ResetSession(1);
    GpuRuntimeSample sample = Sample();
    sample.localUsageBytes = 950 * MiB;
    scheduler.Observe(sample);

    GpuAdmissionResult result = scheduler.TryAcquire(Request(GpuWorkClass::Background));
    CHECK(result.admitted);
    CHECK(result.wouldReject);
    CHECK_EQ(GpuRejectReason::Pressure, result.reason);
}

void stale_generation_wrong_adapter_and_device_loss_are_rejected_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.SetMode(GpuSchedulerMode::Admit);
    scheduler.ResetSession(2);
    scheduler.Observe(Sample());

    GpuAdmissionResult stale = scheduler.TryAcquire(Request(GpuWorkClass::Interactive, 100 * MiB, 1));
    CHECK(!stale.admitted);
    CHECK_EQ(GpuRejectReason::StaleGeneration, stale.reason);

    GpuWorkloadRequest wrongAdapter = Request(GpuWorkClass::Interactive, 100 * MiB, 2);
    wrongAdapter.adapterLuid = 0x9999;
    GpuAdmissionResult wrong = scheduler.TryAcquire(wrongAdapter);
    CHECK(!wrong.admitted);
    CHECK_EQ(GpuRejectReason::WrongAdapter, wrong.reason);

    GpuRuntimeSample lost = Sample(2);
    lost.rendererUsable = false;
    scheduler.Observe(lost);
    GpuAdmissionResult unavailable = scheduler.TryAcquire(Request(GpuWorkClass::Interactive, 100 * MiB, 2));
    CHECK(!unavailable.admitted);
    CHECK_EQ(GpuRejectReason::DeviceLost, unavailable.reason);
}

void stale_samples_do_not_lower_pressure_test()
{
    GpuScheduler scheduler(NoReserveLimits());
    scheduler.Observe(Sample(10));
    GpuRuntimeSample critical = Sample(11);
    critical.localUsageBytes = 950 * MiB;
    scheduler.Observe(critical);
    CHECK_EQ(GpuPressure::Critical, scheduler.Snapshot().pressure);

    scheduler.Observe(Sample(9));
    CHECK_EQ(GpuPressure::Critical, scheduler.Snapshot().pressure);

    scheduler.ResetSession(2);
    CHECK_EQ(GpuPressure::Normal, scheduler.Snapshot().pressure);
    CHECK_EQ(uint64_t{2}, scheduler.Generation());
}

void lease_release_after_scheduler_destruction_is_safe_test()
{
    GpuLease lease;
    {
        GpuScheduler scheduler(NoReserveLimits());
        scheduler.SetMode(GpuSchedulerMode::Admit);
        scheduler.ResetSession(1);
        scheduler.Observe(Sample());
        GpuAdmissionResult result = scheduler.TryAcquire(Request());
        CHECK(result.admitted);
        lease = std::move(result.lease);
    }
    CHECK(lease.Valid());
    lease.Release();
    CHECK(!lease.Valid());
}

} // namespace

int main()
{
    pressure_thresholds_are_ordered_test();
    frame_and_neural_lead_pressure_are_combined_test();
    missing_budget_and_lost_device_are_critical_test();
    pressure_recovery_uses_hysteresis_and_steps_down_test();
    admission_policy_respects_work_class_test();
    observe_mode_reports_would_reject_without_rejecting_test();
    lease_reserves_commits_and_releases_vram_test();
    admission_rejects_when_vram_does_not_fit_test();
    multiple_leases_account_independently_test();
    safety_reserve_is_enforced_even_when_the_raw_budget_allows_the_request_test();
    invalid_zero_byte_request_is_rejected_test();
    observe_mode_admits_but_marks_the_same_pressure_test();
    stale_generation_wrong_adapter_and_device_loss_are_rejected_test();
    stale_samples_do_not_lower_pressure_test();
    lease_release_after_scheduler_destruction_is_safe_test();

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "GpuSchedulerTests: PASS\n";
    return 0;
}
