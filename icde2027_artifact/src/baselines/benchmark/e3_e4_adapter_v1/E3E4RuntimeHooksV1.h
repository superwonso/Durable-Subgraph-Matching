#ifndef DSM_E3_E4_RUNTIME_HOOKS_V1_H
#define DSM_E3_E4_RUNTIME_HOOKS_V1_H

#include "E3E4AdapterCommonV1.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dsm::e3e4::adapter_v1::runtime_hooks_v1 {

// This file is an integration source slice, not an authority receipt.  The
// future exhaustive source receipt must bind this implementation and every
// native call site before an observation may enter an authoritative child
// receipt.
inline constexpr bool kSourceReceiptAuthority = false;
inline constexpr bool kBuildOrExecutionAuthority = false;

class MonotonicClockReading final {
public:
    [[nodiscard]] std::uint64_t nanoseconds() const noexcept;
    [[nodiscard]] bool productionObserved() const noexcept;
    [[nodiscard]] std::uint64_t authorityNanoseconds() const;

private:
    friend class MonotonicNowSource;
    MonotonicClockReading(
        std::uint64_t nanoseconds,
        bool production_observed) noexcept;

    std::uint64_t nanoseconds_ = 0;
    bool production_observed_ = false;
};

class MonotonicNowSource {
public:
    virtual ~MonotonicNowSource() = default;
    MonotonicNowSource(const MonotonicNowSource&) = delete;
    MonotonicNowSource& operator=(const MonotonicNowSource&) = delete;

    // The provenance bit is assigned by this sealed wrapper.  A caller-owned
    // subclass can provide test timestamps but cannot mark them as production
    // observations.
    [[nodiscard]] MonotonicClockReading now() const;

protected:
    MonotonicNowSource() noexcept;
    // Moving a caller-authored/test source must never transfer the private
    // production-authority bit out of the sealed singleton.  These operations
    // deliberately reset provenance instead of memberwise-moving it.
    MonotonicNowSource(MonotonicNowSource&&) noexcept;
    MonotonicNowSource& operator=(MonotonicNowSource&&) noexcept;

private:
    struct ProductionAuthorityTag final {};
    explicit MonotonicNowSource(ProductionAuthorityTag) noexcept;
    virtual std::uint64_t readNanoseconds() const = 0;

    bool production_authority_ = false;
    friend class ProductionSteadyClockSource;
};

class ProductionSteadyClockSource final : public MonotonicNowSource {
public:
    [[nodiscard]] static const ProductionSteadyClockSource& instance() noexcept;

private:
    ProductionSteadyClockSource() noexcept;
    std::uint64_t readNanoseconds() const override;
};

[[nodiscard]] const MonotonicNowSource& productionMonotonicNowSource() noexcept;

class WorkerAffinityObservation final {
public:
    [[nodiscard]] std::uint32_t requestedCpu() const noexcept;
    [[nodiscard]] bool bindSucceeded() const noexcept;
    [[nodiscard]] const std::vector<std::uint32_t>&
        observedCpuSet() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;
    [[nodiscard]] bool productionObserved() const noexcept;

private:
    friend class WorkerAffinityBinder;
    WorkerAffinityObservation(
        std::uint32_t requested_cpu,
        bool bind_succeeded,
        std::vector<std::uint32_t> observed_cpu_set,
        std::string error,
        bool production_observed);

    std::uint32_t requested_cpu_ = 0;
    bool bind_succeeded_ = false;
    std::vector<std::uint32_t> observed_cpu_set_;
    std::string error_;
    bool production_observed_ = false;
};

class WorkerAffinityBinder {
public:
    virtual ~WorkerAffinityBinder() = default;
    WorkerAffinityBinder(const WorkerAffinityBinder&) = delete;
    WorkerAffinityBinder& operator=(const WorkerAffinityBinder&) = delete;

    // Must be called by the worker whose affinity is being established.
    // Production provenance is attached by this nonvirtual wrapper, never by
    // caller-authored result data.
    [[nodiscard]] WorkerAffinityObservation bindCurrentWorkerToCpu(
        std::uint32_t requested_cpu) const;

protected:
    struct RawBindingResult final {
        bool bind_succeeded = false;
        std::vector<std::uint32_t> observed_cpu_set;
        std::string error;
    };

    WorkerAffinityBinder() noexcept;
    // As with the clock source, moving through this public polymorphic base
    // cannot transfer production provenance to a caller-defined subclass.
    WorkerAffinityBinder(WorkerAffinityBinder&&) noexcept;
    WorkerAffinityBinder& operator=(WorkerAffinityBinder&&) noexcept;

private:
    struct ProductionAuthorityTag final {};
    explicit WorkerAffinityBinder(ProductionAuthorityTag) noexcept;
    virtual RawBindingResult bindAndObserve(
        std::uint32_t requested_cpu) const = 0;

    bool production_authority_ = false;
    friend class ProductionLinuxWorkerAffinityBinder;
};

class ProductionLinuxWorkerAffinityBinder final
    : public WorkerAffinityBinder {
public:
    [[nodiscard]] static const ProductionLinuxWorkerAffinityBinder&
        instance() noexcept;

private:
    ProductionLinuxWorkerAffinityBinder() noexcept;
    RawBindingResult bindAndObserve(
        std::uint32_t requested_cpu) const override;
};

[[nodiscard]] const WorkerAffinityBinder&
productionWorkerAffinityBinder() noexcept;

class ExactWorkerAffinityProof final {
public:
    [[nodiscard]] bool exact() const noexcept;
    [[nodiscard]] bool authorityEligible() const noexcept;
    [[nodiscard]] std::uint32_t requested() const noexcept;
    [[nodiscard]] std::uint32_t effective() const noexcept;
    [[nodiscard]] const std::vector<std::uint32_t>&
        orderedCpuSet() const noexcept;
    [[nodiscard]] const std::string& failureReason() const noexcept;

    // This is the only conversion supplied by the hook layer.  It rejects a
    // logically exact scripted/fake result, preventing caller-authored success
    // from being promoted to production affinity provenance.
    [[nodiscard]] ThreadObservation toAuthorityThreadObservation() const;

private:
    friend ExactWorkerAffinityProof aggregateExact16WorkerAffinity(
        const std::vector<WorkerAffinityObservation>&);
    ExactWorkerAffinityProof(
        bool exact,
        bool authority_eligible,
        std::uint32_t effective,
        std::vector<std::uint32_t> ordered_cpu_set,
        std::string failure_reason);

    bool exact_ = false;
    bool authority_eligible_ = false;
    std::uint32_t effective_ = 0;
    std::vector<std::uint32_t> ordered_cpu_set_;
    std::string failure_reason_;
};

// Requires exactly sixteen worker observations in worker-index order.  Row i
// must request CPU i, report one observed CPU i, and all CPUs must be unique.
// A perfect test-support script yields exact()==true but remains ineligible
// for authority.
[[nodiscard]] ExactWorkerAffinityProof aggregateExact16WorkerAffinity(
    const std::vector<WorkerAffinityObservation>& observations);

namespace test_support {

// Explicit test-only clock.  It is finite and fails closed when exhausted.
// Its readings can never carry production provenance.
class ScriptedMonotonicNowSource final : public MonotonicNowSource {
public:
    explicit ScriptedMonotonicNowSource(
        std::vector<std::uint64_t> readings);
    ~ScriptedMonotonicNowSource() override;
    ScriptedMonotonicNowSource(ScriptedMonotonicNowSource&&) noexcept;
    ScriptedMonotonicNowSource& operator=(
        ScriptedMonotonicNowSource&&) noexcept;

private:
    struct Impl;
    std::uint64_t readNanoseconds() const override;
    std::unique_ptr<Impl> impl_;
};

struct ScriptedWorkerAffinityOutcome final {
    std::uint32_t requested_cpu = 0;
    bool bind_succeeded = false;
    std::vector<std::uint32_t> observed_cpu_set;
    std::string error;
};

// Outcomes are consumed in input order among rows matching requested_cpu.
// This makes repeated CECIDur worker pools deterministic without depending on
// inter-thread call order.
class ScriptedWorkerAffinityBinder final : public WorkerAffinityBinder {
public:
    explicit ScriptedWorkerAffinityBinder(
        std::vector<ScriptedWorkerAffinityOutcome> outcomes);
    ~ScriptedWorkerAffinityBinder() override;
    ScriptedWorkerAffinityBinder(
        ScriptedWorkerAffinityBinder&&) noexcept;
    ScriptedWorkerAffinityBinder& operator=(
        ScriptedWorkerAffinityBinder&&) noexcept;

private:
    struct Impl;
    RawBindingResult bindAndObserve(
        std::uint32_t requested_cpu) const override;
    std::unique_ptr<Impl> impl_;
};

}  // namespace test_support

}  // namespace dsm::e3e4::adapter_v1::runtime_hooks_v1

#endif  // DSM_E3_E4_RUNTIME_HOOKS_V1_H
