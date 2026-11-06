#include "E3E4RuntimeHooksV1.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <mutex>
#include <ratio>
#include <set>
#include <stdexcept>
#include <utility>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace dsm::e3e4::adapter_v1::runtime_hooks_v1 {
namespace {

[[noreturn]] void fail(ErrorCode code, const std::string& message) {
    throw AdapterError(code, "E3/E4 runtime hook: " + message);
}

std::string workerContext(std::size_t worker, const std::string& detail) {
    return "worker " + std::to_string(worker) + ": " + detail;
}

}  // namespace

MonotonicClockReading::MonotonicClockReading(
    std::uint64_t nanoseconds,
    bool production_observed) noexcept
    : nanoseconds_(nanoseconds),
      production_observed_(production_observed) {}

std::uint64_t MonotonicClockReading::nanoseconds() const noexcept {
    return nanoseconds_;
}

bool MonotonicClockReading::productionObserved() const noexcept {
    return production_observed_;
}

std::uint64_t MonotonicClockReading::authorityNanoseconds() const {
    if (!production_observed_) {
        fail(
            ErrorCode::kInvalidPhaseLedger,
            "scripted monotonic reading is test-only and not authority eligible");
    }
    return nanoseconds_;
}

MonotonicNowSource::MonotonicNowSource() noexcept = default;

MonotonicNowSource::MonotonicNowSource(MonotonicNowSource&&) noexcept
    : production_authority_(false) {}

MonotonicNowSource& MonotonicNowSource::operator=(
    MonotonicNowSource&&) noexcept {
    production_authority_ = false;
    return *this;
}

MonotonicNowSource::MonotonicNowSource(
    ProductionAuthorityTag) noexcept
    : production_authority_(true) {}

MonotonicClockReading MonotonicNowSource::now() const {
    return MonotonicClockReading(
        readNanoseconds(), production_authority_);
}

ProductionSteadyClockSource::ProductionSteadyClockSource() noexcept
    : MonotonicNowSource(
          MonotonicNowSource::ProductionAuthorityTag{}) {}

const ProductionSteadyClockSource&
ProductionSteadyClockSource::instance() noexcept {
    static const ProductionSteadyClockSource source;
    return source;
}

std::uint64_t ProductionSteadyClockSource::readNanoseconds() const {
    using Clock = std::chrono::steady_clock;
    using Rep = Clock::duration::rep;
    static_assert(
        std::ratio_equal<typename Clock::period, std::nano>::value,
        "authoritative steady clock must expose exact nanosecond ticks");
    static_assert(
        std::numeric_limits<Rep>::is_integer,
        "steady-clock nanosecond representation must be integral");
    static_assert(
        std::numeric_limits<Rep>::digits <=
            std::numeric_limits<std::uint64_t>::digits,
        "steady-clock nanosecond representation exceeds uint64");
    // The period assertion above makes this a representation-preserving read,
    // avoiding an overflowing duration_cast before the range checks run.
    const Rep count = Clock::now().time_since_epoch().count();
    if constexpr (std::numeric_limits<Rep>::is_signed) {
        if (count < 0) {
            fail(ErrorCode::kClockRegression, "negative steady-clock reading");
        }
    }
    return static_cast<std::uint64_t>(count);
}

const MonotonicNowSource& productionMonotonicNowSource() noexcept {
    return ProductionSteadyClockSource::instance();
}

WorkerAffinityObservation::WorkerAffinityObservation(
    std::uint32_t requested_cpu,
    bool bind_succeeded,
    std::vector<std::uint32_t> observed_cpu_set,
    std::string error,
    bool production_observed)
    : requested_cpu_(requested_cpu),
      bind_succeeded_(bind_succeeded),
      observed_cpu_set_(std::move(observed_cpu_set)),
      error_(std::move(error)),
      production_observed_(production_observed) {}

std::uint32_t WorkerAffinityObservation::requestedCpu() const noexcept {
    return requested_cpu_;
}

bool WorkerAffinityObservation::bindSucceeded() const noexcept {
    return bind_succeeded_;
}

const std::vector<std::uint32_t>&
WorkerAffinityObservation::observedCpuSet() const noexcept {
    return observed_cpu_set_;
}

const std::string& WorkerAffinityObservation::error() const noexcept {
    return error_;
}

bool WorkerAffinityObservation::productionObserved() const noexcept {
    return production_observed_;
}

WorkerAffinityBinder::WorkerAffinityBinder() noexcept = default;

WorkerAffinityBinder::WorkerAffinityBinder(
    WorkerAffinityBinder&&) noexcept
    : production_authority_(false) {}

WorkerAffinityBinder& WorkerAffinityBinder::operator=(
    WorkerAffinityBinder&&) noexcept {
    production_authority_ = false;
    return *this;
}

WorkerAffinityBinder::WorkerAffinityBinder(
    ProductionAuthorityTag) noexcept
    : production_authority_(true) {}

WorkerAffinityObservation WorkerAffinityBinder::bindCurrentWorkerToCpu(
    std::uint32_t requested_cpu) const {
    RawBindingResult result = bindAndObserve(requested_cpu);
    if (result.bind_succeeded && !result.error.empty()) {
        result.bind_succeeded = false;
        result.observed_cpu_set.clear();
        result.error = "binder reported success together with an error";
    }
    if (!result.bind_succeeded && result.error.empty()) {
        result.error = "binder failed without a diagnostic";
    }
    return WorkerAffinityObservation(
        requested_cpu,
        result.bind_succeeded,
        std::move(result.observed_cpu_set),
        std::move(result.error),
        production_authority_);
}

ProductionLinuxWorkerAffinityBinder::
ProductionLinuxWorkerAffinityBinder() noexcept
    : WorkerAffinityBinder(
          WorkerAffinityBinder::ProductionAuthorityTag{}) {}

const ProductionLinuxWorkerAffinityBinder&
ProductionLinuxWorkerAffinityBinder::instance() noexcept {
    static const ProductionLinuxWorkerAffinityBinder binder;
    return binder;
}

WorkerAffinityBinder::RawBindingResult
ProductionLinuxWorkerAffinityBinder::bindAndObserve(
    std::uint32_t requested_cpu) const {
    RawBindingResult result;
#if defined(__linux__)
    if (requested_cpu >= static_cast<std::uint32_t>(CPU_SETSIZE) ||
        requested_cpu > static_cast<std::uint32_t>(
            std::numeric_limits<int>::max())) {
        result.error = "requested CPU is outside CPU_SETSIZE";
        return result;
    }

    cpu_set_t requested;
    CPU_ZERO(&requested);
    CPU_SET(static_cast<int>(requested_cpu), &requested);
    const int set_status = pthread_setaffinity_np(
        pthread_self(), sizeof(requested), &requested);
    if (set_status != 0) {
        result.error = "pthread_setaffinity_np failed with code " +
            std::to_string(set_status);
        return result;
    }

    cpu_set_t observed;
    CPU_ZERO(&observed);
    const int get_status = pthread_getaffinity_np(
        pthread_self(), sizeof(observed), &observed);
    if (get_status != 0) {
        result.error = "pthread_getaffinity_np failed with code " +
            std::to_string(get_status);
        return result;
    }
    result.observed_cpu_set.reserve(1);
    for (std::size_t cpu = 0;
         cpu < static_cast<std::size_t>(CPU_SETSIZE);
         ++cpu) {
        if (CPU_ISSET(static_cast<int>(cpu), &observed) != 0) {
            result.observed_cpu_set.push_back(
                static_cast<std::uint32_t>(cpu));
        }
    }
    result.bind_succeeded = true;
    return result;
#else
    static_cast<void>(requested_cpu);
    result.error = "exact worker affinity is supported only on Linux";
    return result;
#endif
}

const WorkerAffinityBinder& productionWorkerAffinityBinder() noexcept {
    return ProductionLinuxWorkerAffinityBinder::instance();
}

ExactWorkerAffinityProof::ExactWorkerAffinityProof(
    bool exact,
    bool authority_eligible,
    std::uint32_t effective,
    std::vector<std::uint32_t> ordered_cpu_set,
    std::string failure_reason)
    : exact_(exact),
      authority_eligible_(authority_eligible),
      effective_(effective),
      ordered_cpu_set_(std::move(ordered_cpu_set)),
      failure_reason_(std::move(failure_reason)) {}

bool ExactWorkerAffinityProof::exact() const noexcept {
    return exact_;
}

bool ExactWorkerAffinityProof::authorityEligible() const noexcept {
    return authority_eligible_;
}

std::uint32_t ExactWorkerAffinityProof::requested() const noexcept {
    return kRequiredThreads;
}

std::uint32_t ExactWorkerAffinityProof::effective() const noexcept {
    return effective_;
}

const std::vector<std::uint32_t>&
ExactWorkerAffinityProof::orderedCpuSet() const noexcept {
    return ordered_cpu_set_;
}

const std::string& ExactWorkerAffinityProof::failureReason() const noexcept {
    return failure_reason_;
}

ThreadObservation
ExactWorkerAffinityProof::toAuthorityThreadObservation() const {
    if (!exact_ || !authority_eligible_) {
        fail(
            ErrorCode::kInvalidChildReceipt,
            failure_reason_.empty()
                ? "affinity proof is not production-authority eligible"
                : failure_reason_);
    }
    ThreadObservation observation;
    observation.requested = kRequiredThreads;
    observation.effective = kRequiredThreads;
    observation.parallel_mode = "roots";
    observation.affinity_cpu_set = ordered_cpu_set_;
    return observation;
}

ExactWorkerAffinityProof aggregateExact16WorkerAffinity(
    const std::vector<WorkerAffinityObservation>& observations) {
    if (observations.size() != kRequiredThreads) {
        return ExactWorkerAffinityProof(
            false,
            false,
            static_cast<std::uint32_t>(std::min<std::size_t>(
                observations.size(),
                std::numeric_limits<std::uint32_t>::max())),
            {},
            "exact affinity requires sixteen worker observations");
    }

    bool production = true;
    std::vector<std::uint32_t> ordered;
    ordered.reserve(kRequiredThreads);
    std::set<std::uint32_t> unique;
    for (std::size_t worker = 0; worker < observations.size(); ++worker) {
        const WorkerAffinityObservation& observation = observations[worker];
        production = production && observation.productionObserved();
        if (observation.requestedCpu() != worker) {
            return ExactWorkerAffinityProof(
                false,
                false,
                static_cast<std::uint32_t>(worker),
                std::move(ordered),
                workerContext(worker, "requested CPU differs from worker index"));
        }
        if (!observation.bindSucceeded()) {
            return ExactWorkerAffinityProof(
                false,
                false,
                static_cast<std::uint32_t>(worker),
                std::move(ordered),
                workerContext(worker, observation.error()));
        }
        if (observation.observedCpuSet().size() != 1U ||
            observation.observedCpuSet().front() != worker) {
            return ExactWorkerAffinityProof(
                false,
                false,
                static_cast<std::uint32_t>(worker + 1U),
                std::move(ordered),
                workerContext(worker, "observed affinity is not the requested singleton"));
        }
        const std::uint32_t cpu = observation.observedCpuSet().front();
        if (!unique.insert(cpu).second) {
            return ExactWorkerAffinityProof(
                false,
                false,
                static_cast<std::uint32_t>(worker + 1U),
                std::move(ordered),
                workerContext(worker, "observed CPU is duplicated"));
        }
        ordered.push_back(cpu);
    }

    return ExactWorkerAffinityProof(
        true,
        production,
        kRequiredThreads,
        std::move(ordered),
        production
            ? std::string{}
            : "exact scripted affinity is test-only and not authority eligible");
}

namespace test_support {

struct ScriptedMonotonicNowSource::Impl final {
    explicit Impl(std::vector<std::uint64_t> values)
        : readings(std::move(values)) {}

    std::vector<std::uint64_t> readings;
    mutable std::size_t position = 0;
    mutable std::mutex mutex;
};

ScriptedMonotonicNowSource::ScriptedMonotonicNowSource(
    std::vector<std::uint64_t> readings)
    : impl_(std::make_unique<Impl>(std::move(readings))) {
    if (impl_->readings.empty()) {
        fail(ErrorCode::kClockRegression, "scripted clock requires a reading");
    }
}

ScriptedMonotonicNowSource::~ScriptedMonotonicNowSource() = default;

ScriptedMonotonicNowSource::ScriptedMonotonicNowSource(
    ScriptedMonotonicNowSource&&) noexcept = default;

ScriptedMonotonicNowSource&
ScriptedMonotonicNowSource::operator=(
    ScriptedMonotonicNowSource&&) noexcept = default;

std::uint64_t ScriptedMonotonicNowSource::readNanoseconds() const {
    if (!impl_) {
        fail(ErrorCode::kClockRegression, "moved-from scripted clock");
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->position >= impl_->readings.size()) {
        fail(ErrorCode::kClockRegression, "scripted clock exhausted");
    }
    return impl_->readings[impl_->position++];
}

struct ScriptedWorkerAffinityBinder::Impl final {
    explicit Impl(std::vector<ScriptedWorkerAffinityOutcome> values)
        : outcomes(std::move(values)), consumed(outcomes.size(), 0U) {}

    std::vector<ScriptedWorkerAffinityOutcome> outcomes;
    mutable std::vector<std::uint8_t> consumed;
    mutable std::mutex mutex;
};

ScriptedWorkerAffinityBinder::ScriptedWorkerAffinityBinder(
    std::vector<ScriptedWorkerAffinityOutcome> outcomes)
    : impl_(std::make_unique<Impl>(std::move(outcomes))) {
    if (impl_->outcomes.empty()) {
        fail(
            ErrorCode::kInvalidChildReceipt,
            "scripted affinity binder requires an outcome");
    }
}

ScriptedWorkerAffinityBinder::~ScriptedWorkerAffinityBinder() = default;

ScriptedWorkerAffinityBinder::ScriptedWorkerAffinityBinder(
    ScriptedWorkerAffinityBinder&&) noexcept = default;

ScriptedWorkerAffinityBinder&
ScriptedWorkerAffinityBinder::operator=(
    ScriptedWorkerAffinityBinder&&) noexcept = default;

WorkerAffinityBinder::RawBindingResult
ScriptedWorkerAffinityBinder::bindAndObserve(
    std::uint32_t requested_cpu) const {
    RawBindingResult result;
    if (!impl_) {
        result.error = "moved-from scripted affinity binder";
        return result;
    }
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (std::size_t index = 0; index < impl_->outcomes.size(); ++index) {
        if (impl_->consumed[index] != 0U ||
            impl_->outcomes[index].requested_cpu != requested_cpu) {
            continue;
        }
        impl_->consumed[index] = 1U;
        const ScriptedWorkerAffinityOutcome& scripted =
            impl_->outcomes[index];
        result.bind_succeeded = scripted.bind_succeeded;
        result.observed_cpu_set = scripted.observed_cpu_set;
        result.error = scripted.error;
        return result;
    }
    result.error = "no scripted outcome remains for requested CPU " +
        std::to_string(requested_cpu);
    return result;
}

}  // namespace test_support

}  // namespace dsm::e3e4::adapter_v1::runtime_hooks_v1
