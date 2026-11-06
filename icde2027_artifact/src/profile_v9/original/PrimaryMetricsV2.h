#ifndef DSM_PRIMARY_METRICS_V2_H
#define DSM_PRIMARY_METRICS_V2_H

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ostream>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(__linux__)
#include <sched.h>
#include <time.h>
#include <unistd.h>
#else
#error "icde2027-pure-online-v2 native metrics require Linux clock/affinity APIs"
#endif

namespace dsm::pure_online_v2 {

inline constexpr const char* kContractId = "icde2027-pure-online-v2";

struct ClockPair {
    std::uint64_t wall_ns = 0;
    std::uint64_t process_cpu_ns = 0;
};

struct IntervalDelta {
    std::uint64_t wall_ns = 0;
    std::uint64_t process_cpu_ns = 0;
};

inline std::uint64_t readClockNanoseconds(clockid_t clock_id) {
    timespec value{};
    if (::clock_gettime(clock_id, &value) != 0) {
        throw std::runtime_error(
            "clock_gettime failed: " + std::string(std::strerror(errno)));
    }
    if (value.tv_sec < 0 || value.tv_nsec < 0 ||
        value.tv_nsec >= 1'000'000'000L) {
        throw std::runtime_error("clock_gettime returned an invalid timespec");
    }
    const auto seconds = static_cast<std::uint64_t>(value.tv_sec);
    const auto nanoseconds = static_cast<std::uint64_t>(value.tv_nsec);
    if (seconds >
        (std::numeric_limits<std::uint64_t>::max() - nanoseconds) /
            UINT64_C(1000000000)) {
        throw std::overflow_error("clock reading is not representable in ns");
    }
    return seconds * UINT64_C(1000000000) + nanoseconds;
}

// The wall read is taken first at entry and last at exit; the process-CPU
// reads are nested inside that wall interval.  Consequently both deltas cover
// the same native work interval while the wall interval conservatively also
// contains the few instructions needed to read CLOCK_PROCESS_CPUTIME_ID.
inline ClockPair beginInterval() {
    ClockPair result;
    result.wall_ns = readClockNanoseconds(CLOCK_MONOTONIC);
    result.process_cpu_ns =
        readClockNanoseconds(CLOCK_PROCESS_CPUTIME_ID);
    return result;
}

inline IntervalDelta endInterval(const ClockPair& start) {
    const std::uint64_t process_cpu_end =
        readClockNanoseconds(CLOCK_PROCESS_CPUTIME_ID);
    const std::uint64_t wall_end = readClockNanoseconds(CLOCK_MONOTONIC);
    if (process_cpu_end < start.process_cpu_ns || wall_end < start.wall_ns) {
        throw std::runtime_error("primary metric clock regressed");
    }
    return {
        wall_end - start.wall_ns,
        process_cpu_end - start.process_cpu_ns,
    };
}

inline void checkedAdd(std::uint64_t& destination, std::uint64_t value) {
    if (value > std::numeric_limits<std::uint64_t>::max() - destination) {
        throw std::overflow_error("primary metric nanosecond sum overflow");
    }
    destination += value;
}

inline std::vector<std::uint32_t> observedAffinityCpuSet() {
    cpu_set_t mask;
    CPU_ZERO(&mask);
    if (::sched_getaffinity(0, sizeof(mask), &mask) != 0) {
        throw std::runtime_error(
            "sched_getaffinity failed: " +
            std::string(std::strerror(errno)));
    }
    std::vector<std::uint32_t> cpus;
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(cpu, &mask)) {
            cpus.push_back(static_cast<std::uint32_t>(cpu));
        }
    }
    return cpus;
}

inline std::size_t hostOnlineCpuCount() {
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    if (count < 1) {
        throw std::runtime_error("sysconf(_SC_NPROCESSORS_ONLN) failed");
    }
    return static_cast<std::size_t>(count);
}

inline void updateAtomicPeak(
    std::atomic<std::size_t>& peak,
    std::size_t candidate) {
    std::size_t previous = peak.load(std::memory_order_relaxed);
    while (previous < candidate &&
           !peak.compare_exchange_weak(
               previous,
               candidate,
               std::memory_order_relaxed,
               std::memory_order_relaxed)) {
    }
}

struct PrimaryMetrics {
    std::string scope;
    std::uint64_t wall_ns = 0;
    std::uint64_t process_cpu_ns = 0;
    std::uint64_t interval_count = 0;
    std::vector<std::uint32_t> affinity_cpu_set_start;
    std::vector<std::uint32_t> affinity_cpu_set_end;
    std::size_t host_online_cpu_count = 0;
    std::size_t worker_threads_created = 0;
    std::size_t peak_live_worker_threads = 0;
    // Peak number of concurrently live worker threads that had claimed at
    // least one native parallel work unit in an interval.
    std::size_t workers_with_tasks = 0;
    std::uint64_t parallel_interval_count = 0;
    std::uint64_t parallel_intervals_with_at_least_requested_work = 0;
    std::uint64_t parallel_work_units_min = 0;
    std::uint64_t parallel_work_units_max = 0;
    std::uint64_t parallel_work_units_total = 0;
    std::uint64_t parallel_work_units_claimed = 0;
    std::uint64_t parallel_workers_with_tasks_min = 0;
    std::uint64_t parallel_workers_with_tasks_max = 0;
    std::uint64_t parallel_workers_with_tasks_sum = 0;
    bool parallelism_eligible = false;
};

inline void captureEnvironmentStart(PrimaryMetrics& metrics) {
    metrics.affinity_cpu_set_start = observedAffinityCpuSet();
    metrics.host_online_cpu_count = hostOnlineCpuCount();
}

inline void captureEnvironmentEnd(PrimaryMetrics& metrics) {
    metrics.affinity_cpu_set_end = observedAffinityCpuSet();
}

inline void addInterval(
    PrimaryMetrics& metrics,
    const IntervalDelta& delta) {
    checkedAdd(metrics.wall_ns, delta.wall_ns);
    checkedAdd(metrics.process_cpu_ns, delta.process_cpu_ns);
    checkedAdd(metrics.interval_count, UINT64_C(1));
}

inline void writeCpuSet(
    std::ostream& output,
    const std::vector<std::uint32_t>& cpus) {
    output << '[';
    for (std::size_t index = 0; index < cpus.size(); ++index) {
        if (index != 0) output << ',';
        output << cpus[index];
    }
    output << ']';
}

inline void writePrimaryMetrics(
    std::ostream& output,
    const PrimaryMetrics& metrics) {
    const bool affinity_stable =
        metrics.affinity_cpu_set_start == metrics.affinity_cpu_set_end;
    output << "primary_metric_contract_id: " << kContractId << '\n'
           << "primary_scope: " << metrics.scope << '\n'
           << "primary_excludes_planning: true\n"
           << "primary_wall_clock_id: CLOCK_MONOTONIC\n"
           << "primary_process_cpu_clock_id: CLOCK_PROCESS_CPUTIME_ID\n"
           << "primary_clock_pairing: wall-outer-cpu-inner\n"
           << "primary_cpu_wall_interval_aligned: true\n"
           << "primary_wall_ns: " << metrics.wall_ns << '\n'
           << "primary_process_cpu_ns: " << metrics.process_cpu_ns << '\n'
           << "primary_interval_count: " << metrics.interval_count << '\n'
           << "allowed_affinity_cpu_set: ";
    writeCpuSet(output, metrics.affinity_cpu_set_start);
    output << "\nallowed_affinity_cpu_count: "
           << metrics.affinity_cpu_set_start.size()
           << "\nallowed_affinity_cpu_set_start: ";
    writeCpuSet(output, metrics.affinity_cpu_set_start);
    output << "\nallowed_affinity_cpu_set_end: ";
    writeCpuSet(output, metrics.affinity_cpu_set_end);
    output << "\nallowed_affinity_stable: "
           << (affinity_stable ? "true" : "false") << '\n'
           << "allowed_affinity_semantics: "
              "calling-main-thread-allowed-mask-not-utilization\n"
           << "host_online_cpu_count: "
           << metrics.host_online_cpu_count << '\n'
           << "worker_threads_created: "
           << metrics.worker_threads_created << '\n'
           << "peak_live_worker_threads: "
           << metrics.peak_live_worker_threads << '\n'
           << "workers_with_tasks: "
           << metrics.workers_with_tasks << '\n'
           << "parallel_interval_count: "
           << metrics.parallel_interval_count << '\n'
           << "parallel_intervals_with_at_least_requested_work: "
           << metrics.parallel_intervals_with_at_least_requested_work << '\n'
           << "parallel_work_units_min: "
           << metrics.parallel_work_units_min << '\n'
           << "parallel_work_units_max: "
           << metrics.parallel_work_units_max << '\n'
           << "parallel_work_units_total: "
           << metrics.parallel_work_units_total << '\n'
           << "parallel_work_units_claimed: "
           << metrics.parallel_work_units_claimed << '\n'
           << "parallel_workers_with_tasks_min: "
           << metrics.parallel_workers_with_tasks_min << '\n'
           << "parallel_workers_with_tasks_max: "
           << metrics.parallel_workers_with_tasks_max << '\n'
           << "parallel_workers_with_tasks_sum: "
           << metrics.parallel_workers_with_tasks_sum << '\n'
           << "parallelism_eligible: "
           << (metrics.parallelism_eligible ? "true" : "false") << '\n';
}

}  // namespace dsm::pure_online_v2

#endif  // DSM_PRIMARY_METRICS_V2_H
