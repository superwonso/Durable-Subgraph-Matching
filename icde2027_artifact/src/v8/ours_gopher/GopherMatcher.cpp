#include "GopherMatcher.h"
#include "OpKernels.h"
#include "WindowStore.h"
#include "DirectP4Evaluator.h"
#include "WindowProgram.h"
#include "JIndexExecution.h"
#include "BIndexExecution.h"
#include "JIndexSimulation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace {

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
GopherMatcherOptions normalizedMatcherOptions(GopherMatcherOptions options) {
    options.executor.window_guards = options.window_algebra;
    return options;
}
#endif

struct DigestAccumulator {
    std::uint64_t xor_value = 0;
    std::uint64_t sum_value = 0;

    void add(std::uint64_t value) {
        xor_value ^= value;
        sum_value += value;
    }

    void merge(const DigestAccumulator& other) {
        xor_value ^= other.xor_value;
        sum_value += other.sum_value;
    }

    std::string text() const {
        std::ostringstream output;
        output << std::hex << std::setfill('0')
               << std::setw(16) << xor_value << ':'
               << std::setw(16) << sum_value;
        return output.str();
    }
};

std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31U);
}

std::uint64_t mappingHash(
    const Graph& graph,
    const std::vector<int>& mapping) {
    std::uint64_t hash = 0x6a09e667f3bcc909ULL;
    for (std::size_t query_vertex = 0;
         query_vertex < mapping.size();
         ++query_vertex) {
        const std::uint64_t external = static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(
                graph.externalId(mapping[query_vertex])));
        hash = mix64(
            hash ^ mix64(
                external +
                static_cast<std::uint64_t>(query_vertex) *
                    0x9e3779b97f4a7c15ULL));
    }
    return hash;
}

struct RootResult {
    CheckedMatchCounter counter;
    DigestAccumulator digest;
};

struct SearchTask {
    bool is_root = false;
    std::size_t root_index = 0;
    SearchState state;
    std::size_t depth = 1;
    std::size_t accounted_bytes = 0;
};

struct WorkerMetrics {
    std::uint64_t tasks = 0;
    std::uint64_t dfs_states = 0;
    std::uint64_t active_nanoseconds = 0;
};

template <bool CollectStatistics>
std::chrono::steady_clock::time_point workerMeasurementStart() {
    if constexpr (CollectStatistics) {
        return std::chrono::steady_clock::now();
    }
    return std::chrono::steady_clock::time_point{};
}

template <bool CollectStatistics>
std::uint64_t workerMeasurementElapsed(
    std::chrono::steady_clock::time_point start) {
    if constexpr (CollectStatistics) {
        return static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - start).count());
    }
    (void)start;
    return 0;
}

// A bounded centralized work queue is used only at prefix split points.  The
// queue stays small (threads * tasks_per_thread), while idle workers can take
// subtrees created inside an otherwise long-running root DFS.  Root tasks are
// generated lazily so a graph with millions of root candidates does not
// materialize millions of SearchState objects.
template <bool CollectDiagnostics>
class DynamicPrefixQueue {
public:
    DynamicPrefixQueue(
        std::size_t root_count,
        std::size_t prefix_capacity)
        : root_count_(root_count),
          prefix_capacity_(std::max<std::size_t>(1, prefix_capacity)) {}

    bool mayAcceptPrefix() const {
        return queued_prefixes_.load(std::memory_order_relaxed) <
            prefix_capacity_;
    }

    bool tryPushPrefix(SearchTask task) {
        std::lock_guard<std::mutex> lock(mutex_);
        if constexpr (CollectDiagnostics) ++split_attempts_;
        if (stopping_ || prefix_tasks_.size() >= prefix_capacity_) {
            if constexpr (CollectDiagnostics) ++split_rejected_;
            return false;
        }
        task.is_root = false;
        if constexpr (CollectDiagnostics) {
            task.accounted_bytes =
                sizeof(SearchTask) +
                task.state.mapping.capacity() * sizeof(int) +
                task.state.active_intervals.capacity() *
                    sizeof(TimeInterval);
            current_state_bytes_ += task.accounted_bytes;
            peak_state_bytes_ = std::max(
                peak_state_bytes_, current_state_bytes_);
        }
        prefix_tasks_.push_back(std::move(task));
        ++primary_prefix_tasks_spawned_;
        queued_prefixes_.store(
            prefix_tasks_.size(), std::memory_order_relaxed);
        if constexpr (CollectDiagnostics) {
            ++prefix_tasks_spawned_;
            queue_peak_ = std::max<std::uint64_t>(
                queue_peak_,
                static_cast<std::uint64_t>(prefix_tasks_.size()));
        }
        condition_.notify_one();
        return true;
    }

    bool take(SearchTask& task) {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&]() {
            return stopping_ ||
                !prefix_tasks_.empty() ||
                next_root_ < root_count_ ||
                active_tasks_ == 0;
        });
        if (stopping_) return false;

        if (!prefix_tasks_.empty()) {
            task = std::move(prefix_tasks_.front());
            prefix_tasks_.pop_front();
            if constexpr (CollectDiagnostics) {
                if (task.accounted_bytes <= current_state_bytes_) {
                    current_state_bytes_ -= task.accounted_bytes;
                } else {
                    current_state_bytes_ = 0;
                }
            }
            queued_prefixes_.store(
                prefix_tasks_.size(), std::memory_order_relaxed);
        } else if (next_root_ < root_count_) {
            task = SearchTask{};
            task.is_root = true;
            task.root_index = next_root_++;
            if constexpr (CollectDiagnostics) ++root_tasks_claimed_;
        } else {
            stopping_ = true;
            condition_.notify_all();
            return false;
        }
        ++active_tasks_;
        if constexpr (CollectDiagnostics) ++queue_claims_;
        return true;
    }

    void complete() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_tasks_ > 0) --active_tasks_;
        if (active_tasks_ == 0 &&
            prefix_tasks_.empty() &&
            next_root_ >= root_count_) {
            stopping_ = true;
            condition_.notify_all();
        } else {
            condition_.notify_all();
        }
    }

    void cancel() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        prefix_tasks_.clear();
        queued_prefixes_.store(0, std::memory_order_relaxed);
        condition_.notify_all();
    }

    ParallelExecutionStatistics snapshot(
        std::size_t prefix_depth_limit) const {
        std::lock_guard<std::mutex> lock(mutex_);
        ParallelExecutionStatistics result;
        if constexpr (CollectDiagnostics) {
            result.root_tasks_claimed = root_tasks_claimed_;
            result.prefix_tasks_spawned = prefix_tasks_spawned_;
            result.prefix_split_attempts = split_attempts_;
            result.prefix_split_rejected = split_rejected_;
            result.queue_claims = queue_claims_;
            result.queue_peak = queue_peak_;
            result.queue_state_bytes_peak =
                static_cast<std::uint64_t>(peak_state_bytes_);
        }
        result.task_queue_capacity = prefix_capacity_;
        result.prefix_depth_limit = prefix_depth_limit;
        return result;
    }

    std::uint64_t primaryPrefixTasksSpawned() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return primary_prefix_tasks_spawned_;
    }

private:
    const std::size_t root_count_;
    const std::size_t prefix_capacity_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<SearchTask> prefix_tasks_;
    std::atomic<std::size_t> queued_prefixes_{0};
    std::size_t next_root_ = 0;
    std::size_t active_tasks_ = 0;
    bool stopping_ = false;
    std::uint64_t root_tasks_claimed_ = 0;
    std::uint64_t prefix_tasks_spawned_ = 0;
    std::uint64_t primary_prefix_tasks_spawned_ = 0;
    std::uint64_t split_attempts_ = 0;
    std::uint64_t split_rejected_ = 0;
    std::uint64_t queue_claims_ = 0;
    std::uint64_t queue_peak_ = 0;
    std::size_t current_state_bytes_ = 0;
    std::size_t peak_state_bytes_ = 0;
};

bool checkedAdd(
    CheckedMatchCounter& total,
    std::uint64_t value) {
    if (total.overflowed) return false;
    if (value > std::numeric_limits<std::uint64_t>::max() - total.value) {
        total.value = std::numeric_limits<std::uint64_t>::max();
        total.overflowed = true;
        return false;
    }
    total.value += value;
    return true;
}

} // namespace

#if defined(DSM_E3E4_CORE_ONLY)
struct GopherMatcher::E3E4RunContext final {
    enum class State : std::uint8_t {
        Ready,
        Enumerating,
        Reducing,
    };

    GopherE3E4NativeObserver& observer;
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::
        WorkerAffinityBinder& binder;
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::
        MonotonicNowSource& clock;
    std::vector<std::optional<dsm::e3e4::adapter_v1::runtime_hooks_v1::
        WorkerAffinityObservation>> affinity;
    State state = State::Ready;
    const QueryPlan* executable_plan_address = nullptr;
    bool dag_backend = false;

    E3E4RunContext(
        GopherE3E4NativeObserver& input_observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            WorkerAffinityBinder& input_binder,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            MonotonicNowSource& input_clock,
        std::size_t worker_count)
        : observer(input_observer),
          binder(input_binder),
          clock(input_clock),
          affinity(worker_count) {}

    void observe(
        GopherE3E4NativePhase phase,
        const QueryPlan& executable_plan,
        bool uses_dag) {
        if (phase == GopherE3E4NativePhase::Enumeration) {
            if (state != State::Ready || executable_plan_address != nullptr) {
                throw std::logic_error(
                    "E3/E4 matcher enumeration boundary repeated or out of order");
            }
            state = State::Enumerating;
            executable_plan_address = &executable_plan;
            dag_backend = uses_dag;
        } else {
            if (state != State::Enumerating ||
                executable_plan_address != &executable_plan ||
                dag_backend != uses_dag) {
                throw std::logic_error(
                    "E3/E4 matcher count-reduction boundary is out of order");
            }
            state = State::Reducing;
        }
        const auto reading = clock.now();
        observer.onGopherE3E4NativePhase(
            phase, reading, executable_plan, uses_dag);
    }

    void bindWorker(std::size_t worker_index) {
        if (state != State::Enumerating ||
            worker_index >= affinity.size() ||
            affinity[worker_index].has_value()) {
            throw std::logic_error(
                "E3/E4 worker-affinity observation is out of order");
        }
        affinity[worker_index].emplace(
            binder.bindCurrentWorkerToCpu(
                static_cast<std::uint32_t>(worker_index)));
    }
};
#endif

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
GopherMatcher::~GopherMatcher() = default;

const char* parallelModeName(ParallelMode mode) {
    switch (mode) {
        case ParallelMode::Roots:
            return "roots";
        case ParallelMode::Prefix:
            return "prefix";
    }
    return "roots";
}

bool parseParallelMode(
    const std::string& text,
    ParallelMode& mode) {
    if (text == "roots") {
        mode = ParallelMode::Roots;
        return true;
    }
    if (text == "prefix") {
        mode = ParallelMode::Prefix;
        return true;
    }
    return false;
}

GopherMatcher::GopherMatcher(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const CandidateDag& dag,
    const TDTree& td_tree,
    int minimum_duration,
    GopherMatcherOptions options)
    : G_(temporal_graph),
      Q_(query_graph),
      plan_(plan),
      dag_(&dag),
      td_tree_(td_tree),
      k_threshold_(minimum_duration),
      options_(normalizedMatcherOptions(std::move(options))),
      statistics_(options_.executor.collect_statistics),
      executor_(std::make_unique<DagExecutor>(
          temporal_graph,
          query_graph,
          plan,
          dag,
          td_tree,
          minimum_duration,
          options_.executor,
          statistics_)) {
    if (options_.threads == 0) options_.threads = 1;
    if (options_.order_simulation != OrderSimulationMode::Disabled) {
        throw std::invalid_argument("A2 order simulation requires the Direct backend");
    }
    if (dag.passes.physicalAny() &&
        (options_.parallel_mode != ParallelMode::Roots || !options_.early_prune ||
         options_.executor.adaptive != nullptr)) {
        throw std::invalid_argument("physical DAG passes require roots, early pruning, and no A1/timer observer");
    }
    prepareV5();
}

GopherMatcher::GopherMatcher(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const TDTree& td_tree,
    int minimum_duration,
    GopherMatcherOptions options)
    : G_(temporal_graph),
      Q_(query_graph),
      plan_(plan),
      td_tree_(td_tree),
      k_threshold_(minimum_duration),
      options_(normalizedMatcherOptions(std::move(options))),
      statistics_(options_.executor.collect_statistics),
      direct_evaluator_(std::make_unique<DirectEvaluator>(
          temporal_graph,
          query_graph,
          plan,
          td_tree,
          minimum_duration,
          statistics_,
          options_.executor.direct_passes,
          options_.executor.ops,
          options_.executor.adaptive)) {
    if (options_.threads == 0) options_.threads = 1;
    prepareV5();
    if (options_.order_simulation != OrderSimulationMode::Disabled) {
        if (options_.parallel_mode != ParallelMode::Roots ||
            options_.executor.memo_mode != MemoMode::None ||
            options_.executor.direct_passes.any() ||
            options_.executor.collect_statistics || !options_.early_prune) {
            throw std::invalid_argument("A2 simulation requires roots, no memo/passes/statistics, and early pruning");
        }
        order_simulator_ = std::make_unique<OrderSimulator>(
            G_, Q_, plan_, td_tree_, k_threshold_, options_.order_simulation);
    }
}
#endif

template <bool CollectStatistics, bool ExactDiagnostics>
DagExecutor::CandidateSetPtr GopherMatcher::structuralCandidates(
    int query_vertex,
    const SearchState& state) {
    if (executor_) {
        return executor_->structuralCandidatesSpecialized<
            CollectStatistics,
            ExactDiagnostics>(query_vertex, state);
    }
    if constexpr (ExactDiagnostics) {
        throw std::logic_error(
            "exact runtime-key diagnostics require a Candidate-DAG executor");
    }
    return direct_evaluator_
        ->structuralCandidatesSpecialized<CollectStatistics>(
            query_vertex, state);
}

template <bool CollectStatistics, bool ExactDiagnostics>
TemporalSupport GopherMatcher::temporalSupport(
    int query_vertex,
    int candidate,
    const SearchState& state) {
    if (executor_) {
        return executor_->temporalSupportSpecialized<
            CollectStatistics,
            ExactDiagnostics>(
                query_vertex, candidate, state);
    }
    if constexpr (ExactDiagnostics) {
        throw std::logic_error(
            "exact runtime-key diagnostics require a Candidate-DAG executor");
    }
    return direct_evaluator_
        ->temporalSupportSpecialized<CollectStatistics>(
            query_vertex, candidate, state);
}

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
GopherMatchSummary GopherMatcher::run(
    MatchOutputMode mode,
    std::ostream* match_rows) {
    if (options_.window_algebra || options_.direct_p4)
        return runV5(mode, match_rows);
#if defined(DSM_E3E4_CORE_ONLY)
    if (mode != MatchOutputMode::CountOnly || match_rows != nullptr ||
        e3e4_run_context_ == nullptr) {
        throw std::logic_error(
            "E3/E4 core run requires the closed observed count-only entrypoint");
    }
#endif
    // Resolve both diagnostic dimensions once before entering the recursive
    // search. Exact-key collection remains available independently from the
    // ordinary atomic counters for library-level diagnostic tests.
    const bool exact_diagnostics =
        options_.executor.runtime_key_diagnostics ==
        RuntimeKeyDiagnosticsMode::Exact;
#if defined(DSM_ERASE_SEARCH_STATISTICS)
    return exact_diagnostics
        ? runSpecialized<false, true>(mode, match_rows)
        : runSpecialized<false, false>(mode, match_rows);
#else
    const bool collect_statistics = statistics_.enabled();
    if (collect_statistics) {
        return exact_diagnostics
            ? runSpecialized<true, true>(mode, match_rows)
            : runSpecialized<true, false>(mode, match_rows);
    }
    return exact_diagnostics
        ? runSpecialized<false, true>(mode, match_rows)
        : runSpecialized<false, false>(mode, match_rows);
#endif
}
#endif

#if defined(DSM_E3E4_CORE_ONLY) && \
    !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
GopherE3E4ObservedRun GopherMatcher::runE3E4CountOnlyWithHooks(
    GopherE3E4NativeObserver& observer,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::
        WorkerAffinityBinder& binder,
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::
        MonotonicNowSource& clock) {
    constexpr std::size_t kAuthorityThreads =
        dsm::e3e4::adapter_v1::kRequiredThreads;
    if (e3e4_run_context_ != nullptr ||
        options_.threads != kAuthorityThreads ||
        options_.parallel_mode != ParallelMode::Roots ||
        options_.prefix_depth != 0U ||
        options_.tasks_per_thread != 8U ||
        options_.order_mode != OrderMode::Paper ||
        !options_.early_prune || options_.result_digest ||
        options_.executor.collect_statistics ||
        options_.executor.verify_dag ||
        options_.executor.runtime_key_diagnostics !=
            RuntimeKeyDiagnosticsMode::Off ||
        options_.executor.exact_runtime_key_cap != 0U ||
        options_.executor.memo_policy != MemoPolicy::Legacy ||
        options_.executor.runtime_key_mode !=
            RuntimeKeyMode::RequiredBindings ||
        options_.executor.memo_options.budget_bytes != (256ULL << 20) ||
        options_.executor.memo_options.max_entry_bytes != (8ULL << 20) ||
        options_.executor.memo_options.shard_count != 64U ||
        options_.executor.memo_options.minimum_estimated_work != 0U ||
        options_.executor.memo_options.cost_aware_admission ||
        options_.executor.memo_options.collect_statistics) {
        throw std::logic_error(
            "E3/E4 matcher options differ from the closed count-only contract");
    }
    const bool uses_dag = executor_ != nullptr;
    if (uses_dag == (direct_evaluator_ != nullptr) ||
        (uses_dag && dag_ == nullptr) ||
        (!uses_dag && dag_ != nullptr)) {
        throw std::logic_error("E3/E4 matcher backend ownership drift");
    }
    if ((uses_dag && options_.executor.memo_mode != MemoMode::Dag) ||
        (!uses_dag && options_.executor.memo_mode != MemoMode::None)) {
        throw std::logic_error(
            "E3/E4 matcher memo mode differs from the selected frozen arm");
    }

    E3E4RunContext context(
        observer, binder, clock, kAuthorityThreads);
    e3e4_run_context_ = &context;
    GopherMatchSummary summary;
    try {
        summary = run(MatchOutputMode::CountOnly, nullptr);
    } catch (...) {
        e3e4_run_context_ = nullptr;
        throw;
    }
    e3e4_run_context_ = nullptr;

    if (context.state != E3E4RunContext::State::Reducing ||
        context.executable_plan_address != &plan_ ||
        context.dag_backend != uses_dag ||
        summary.output_written ||
        summary.threads_used != kAuthorityThreads ||
        summary.parallel_mode_effective != "roots") {
        throw std::logic_error(
            "E3/E4 native count-only observation did not close exactly");
    }

    GopherE3E4ObservedRun result;
    result.summary = std::move(summary);
    result.executable_plan_address = context.executable_plan_address;
    result.dag_backend = context.dag_backend;
    result.worker_affinity.reserve(kAuthorityThreads);
    for (std::size_t worker = 0; worker < kAuthorityThreads; ++worker) {
        if (!context.affinity[worker].has_value()) {
            throw std::logic_error(
                "E3/E4 worker omitted its affinity observation");
        }
        result.worker_affinity.push_back(
            std::move(*context.affinity[worker]));
    }
    return result;
}

GopherE3E4ObservedRun GopherMatcher::runAdapterOwnedE3E4ClosedCountOnly(
    GopherE3E4NativeObserver& observer) {
    return runE3E4CountOnlyWithHooks(
        observer,
        dsm::e3e4::adapter_v1::runtime_hooks_v1::
            productionWorkerAffinityBinder(),
        dsm::e3e4::adapter_v1::runtime_hooks_v1::
            productionMonotonicNowSource());
}

#if defined(DSM_E3E4_TEST_HOOKS)
GopherE3E4ObservedRun
dsm::e3e4::gopher_runtime_v1::test_support::TestOnlyHookRunner::
    runCountOnly(
        GopherMatcher& matcher,
        GopherE3E4NativeObserver& observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            WorkerAffinityBinder& binder,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            MonotonicNowSource& clock) {
    return matcher.runE3E4CountOnlyWithHooks(
        observer, binder, clock);
}
#endif  // DSM_E3E4_TEST_HOOKS
#endif

template <bool CollectStatistics, bool ExactDiagnostics>
GopherMatchSummary GopherMatcher::runSpecialized(
    MatchOutputMode mode,
    std::ostream* match_rows) {
    GopherMatchSummary summary;
    summary.parallel_mode_requested = options_.parallel_mode;
    summary.primary_metrics.scope =
        "final_dag_search_and_count_reduction";
    dsm::pure_online_v2::captureEnvironmentStart(
        summary.primary_metrics);
    const auto primary_start = dsm::pure_online_v2::beginInterval();
    const auto& roots = td_tree_.rootCandidates();
    std::atomic<bool> cancel{false};
    std::atomic<std::size_t> live_workers{0};
    std::atomic<std::size_t> peak_live_workers{0};
    std::atomic<std::size_t> productive_workers{0};
    std::atomic<std::size_t> peak_productive_workers{0};
    std::vector<std::uint64_t> primary_worker_work_units;
    std::uint64_t primary_prefix_work_units_spawned = 0;
    AdaptiveDiagnostics* const adaptive = options_.executor.adaptive;
    const bool observe_funnel = adaptive != nullptr && adaptive->stateCountersEnabled();
    const bool physical_candidates = dag_ != nullptr && dag_->passes.physicalAny();

    // Shadow metadata only: preserve the original P0 temporal evaluation and
    // all pruning/extension decisions. Each candidate receives a disjoint
    // first-drop category in the virtual structure/injectivity/Lmax/time funnel.
    auto shadow_lmax_passes = [&](int query_vertex, int candidate, const SearchState& state) {
        bool durable = true;
        auto check = [&](int source, int target) {
            const TemporalEdge* edge = G_.findTemporalEdge(source, target);
            const bool passes = edge != nullptr && edge->maximumConsecutiveRun() >= k_threshold_;
            adaptive->recordShadowLmaxProbe(edge != nullptr, passes);
            durable = durable && passes;
        };
        for (const Edge& edge : Q_.adj[static_cast<std::size_t>(query_vertex)]) {
            const int other = edge.to == query_vertex ? candidate : state.mapping[static_cast<std::size_t>(edge.to)];
            if (other >= 0) check(candidate, other);
        }
        for (const Edge& edge : Q_.in_adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to == query_vertex) continue; // Self-edge already checked above.
            const int other = state.mapping[static_cast<std::size_t>(edge.to)];
            if (other >= 0) check(other, candidate);
        }
        return durable;
    };

    auto write_match = [&](const SearchState& state,
                           std::uint64_t match_index) {
#if defined(DSM_E3E4_CORE_ONLY)
        (void)state;
        (void)match_index;
        if (mode != MatchOutputMode::CountOnly || match_rows != nullptr) {
            throw std::logic_error(
                "E3/E4 core cannot materialize match rows");
        }
#else
        if (mode != MatchOutputMode::Full || match_rows == nullptr) return;
        std::ostringstream row;
        row << "Match " << match_index << ": ";
        for (int query_vertex = 0;
             query_vertex < Q_.num_vertices;
             ++query_vertex) {
            if (query_vertex > 0) row << ", ";
            row << 'q' << query_vertex << '('
                << labelToString(
                       Q_.vertex_labels[
                           static_cast<std::size_t>(query_vertex)])
                << ")->"
                << G_.externalId(
                       state.mapping[
                           static_cast<std::size_t>(query_vertex)]);
        }
        row << " | active="
            << formatIntervals(state.active_intervals) << '\n';
        const std::string text = row.str();
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.output_buffer_bytes, text.size());
        *match_rows << text;
        if (!*match_rows) {
            throw std::runtime_error("failed while writing match rows");
        }
#endif
    };

    auto initialize_root = [&](std::size_t root_index,
                               SearchState& state) {
        state.mapping.assign(
            static_cast<std::size_t>(Q_.num_vertices), -1);

        const int root_candidate = roots[root_index];
        statistics_.addSpecialized<CollectStatistics>(
            statistics_.root_tasks);

        if (GraphUtils::hasEdge(
                Q_.adj, plan_.root, plan_.root)) {
            const TemporalSupport root_support =
                temporalSupport<CollectStatistics, ExactDiagnostics>(
                    plan_.root, root_candidate, state);
            if (!root_support.has_constraints ||
                root_support.longest_run < k_threshold_ ||
                root_support.intervals->empty()) {
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.pruned_temporal_support);
                return false;
            }
            state.active_intervals = *root_support.intervals;
            state.has_active_intervals = true;
        }

        state.mapping[static_cast<std::size_t>(plan_.root)] =
            root_candidate;
        return true;
    };

    const std::size_t prefix_depth_limit =
        options_.prefix_depth == 0
        ? std::min<std::size_t>(2, plan_.order.size())
        : std::min(options_.prefix_depth, plan_.order.size());

    auto dfs = [&](auto&& self,
                   SearchState& state,
                   std::size_t depth,
                   RootResult& result,
                   std::uint64_t& output_match_index,
                   WorkerMetrics& worker_metrics,
                   DynamicPrefixQueue<CollectStatistics>* prefix_queue)
                   -> void {
                if (cancel.load(std::memory_order_relaxed) ||
                    result.counter.overflowed) {
                    return;
                }
                if constexpr (CollectStatistics) {
                    ++worker_metrics.dfs_states;
                }
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.dfs_states);
                if (depth >= plan_.order.size()) {
                    if (state.has_active_intervals &&
                        state.active_intervals.empty()) {
                        return;
                    }
                    const std::uint64_t match_index =
                        output_match_index;
                    if (!result.counter.increment()) {
                        cancel.store(true, std::memory_order_relaxed);
                        return;
                    }
                    ++output_match_index;
                    if (options_.result_digest) {
                        result.digest.add(
                            mappingHash(G_, state.mapping));
                    }
                    statistics_.addSpecialized<CollectStatistics>(
                        statistics_.completed_matches);
                    if (options_.exact_match_observer != nullptr) {
                        options_.exact_match_observer(state, options_.exact_match_observer_context);
                    }
                    write_match(state, match_index);
                    return;
                }

                int query_vertex = plan_.order[depth];
                OrderSelection order_selection;
                if (order_simulator_) {
                    order_selection = order_simulator_->select(depth, state, *direct_evaluator_);
                    query_vertex = order_selection.query_vertex;
                }
                if constexpr (CollectStatistics) {
                    const int parent_query_vertex =
                        plan_.parent[
                            static_cast<std::size_t>(query_vertex)];
                    if (parent_query_vertex >= 0) {
                        const int parent_data_vertex =
                            state.mapping[
                                static_cast<std::size_t>(
                                    parent_query_vertex)];
                        const std::vector<int>* raw_candidates =
                            td_tree_.candidatesForParent(
                                query_vertex, parent_data_vertex);
                        statistics_.addSpecialized<true>(
                            statistics_.raw_extension_candidates,
                            raw_candidates == nullptr
                                ? 0
                                : raw_candidates->size());
                    }
                }
                // The physical batch (including its history-handle rows) lives
                // in this DFS frame. Recursive children and parallel roots
                // never overwrite an executor-global "last candidate" slot.
                std::optional<PhysicalCandidateBatch> physical_batch;
                DagExecutor::CandidateSetPtr candidates;
                if (physical_candidates) {
                    physical_batch.emplace(executor_->physicalCandidates(query_vertex, state));
                } else {
                    candidates = order_simulator_ ? std::move(order_selection.candidates) : structuralCandidates<
                        CollectStatistics,
                        ExactDiagnostics>(
                        query_vertex, state);
                }
                const std::size_t candidate_count = physical_batch
                    ? physical_batch->size() : candidates->size();
                std::unique_ptr<JIndexSimulation::StateToken> jindex_token;
                if (options_.jindex_simulation) {
                    if (!physical_batch) throw std::logic_error("J-index observer requires S1 batch");
                    jindex_token = std::make_unique<JIndexSimulation::StateToken>(
                        options_.jindex_simulation->observe(query_vertex, state, *physical_batch));
                }
                statistics_.addSpecialized<CollectStatistics>(
                    statistics_.generated_candidates,
                    candidate_count);
                OPS_ADD(options_.executor.ops, candidates, candidate_count);
                if (order_simulator_) order_simulator_->recordActualCandidates(candidate_count);
                if (candidate_count == 0) {
                    statistics_.addSpecialized<CollectStatistics>(
                        statistics_.pruned_structural);
                    return;
                }

                for (std::size_t candidate_index = 0; candidate_index < candidate_count; ++candidate_index) {
                    const int candidate = physical_batch ? physical_batch->candidate(candidate_index)
                        : (*candidates)[candidate_index];
                    if (cancel.load(std::memory_order_relaxed)) return;
                    if (std::find(
                            state.mapping.begin(),
                            state.mapping.end(),
                            candidate) != state.mapping.end()) {
                        statistics_.addSpecialized<CollectStatistics>(
                            statistics_.pruned_injectivity);
                        if (physical_candidates) OPS_ADD(options_.executor.ops, physical_injectivity_rejections, 1);
                        if (observe_funnel) adaptive->recordCandidateOutcome(
                            query_vertex, AdaptiveDiagnostics::Outcome::InjectivityDrop);
                        continue;
                    }

                    const bool shadow_lmax_ok = !observe_funnel ||
                        shadow_lmax_passes(query_vertex, candidate, state);

                    // Merely retaining immutable IDs in a batch is not an
                    // edge-history payload read. Bind the explicit row only
                    // after injectivity; physical time evaluation reads here.
                    std::optional<ProbeHistoryRow> history_row;
                    if (physical_batch && physical_batch->hasHistoryRows()) {
                        history_row.emplace(physical_batch->historyRow(candidate_index));
                    }
                    const TemporalSupport support = physical_batch
                        ? executor_->temporalSupportPhysical(query_vertex, candidate, state,
                            history_row ? &*history_row : nullptr)
                        : temporalSupport<
                            CollectStatistics,
                            ExactDiagnostics>(
                            query_vertex, candidate, state);
                    if (!support.has_constraints ||
                        support.longest_run < k_threshold_ ||
                        support.intervals->empty()) {
                        statistics_.addSpecialized<CollectStatistics>(
                            statistics_.pruned_temporal_support);
                        if (physical_candidates) OPS_ADD(options_.executor.ops, physical_temporal_rejections, 1);
                        if (observe_funnel) adaptive->recordCandidateOutcome(query_vertex,
                            shadow_lmax_ok ? AdaptiveDiagnostics::Outcome::LocalTemporalDrop
                                           : AdaptiveDiagnostics::Outcome::LmaxDrop);
                        continue;
                    }

                    std::vector<TimeInterval> next_active;
                    if (support.includes_cumulative_support) {
                        // P1's compiled branch-state source and final κ
                        // already returned J'. Repeating J intersection here
                        // would undo the compiler rewrite's eliminated work.
                        next_active = *support.intervals;
                    } else if (state.has_active_intervals) {
                        next_active = options_.executor.ops == nullptr
                            ? intersectTimeIntervals(state.active_intervals,
                                *support.intervals, k_threshold_)
                            : OperatorKernels::intersectIntervals(state.active_intervals,
                                *support.intervals, k_threshold_, false, options_.executor.ops);
                    } else {
                        next_active = *support.intervals;
                    }
                    if (next_active.empty() &&
                        options_.early_prune) {
                        statistics_.addSpecialized<CollectStatistics>(
                            statistics_.pruned_active_support);
                        if (physical_candidates) OPS_ADD(options_.executor.ops, physical_temporal_rejections, 1);
                        if (observe_funnel) adaptive->recordCandidateOutcome(query_vertex,
                            shadow_lmax_ok ? AdaptiveDiagnostics::Outcome::CumulativeSupportDrop
                                           : AdaptiveDiagnostics::Outcome::LmaxDrop);
                        continue;
                    }

                    if (observe_funnel) {
                        if (!shadow_lmax_ok) throw std::logic_error("Lmax shadow rejected an accepted extension");
                        adaptive->recordCandidateOutcome(query_vertex, AdaptiveDiagnostics::Outcome::Extension);
                    }

                    statistics_.addSpecialized<CollectStatistics>(
                        statistics_.accepted_candidates);
                    statistics_.addSpecialized<CollectStatistics>(
                        statistics_.accepted_extensions);
                    OPS_ADD(options_.executor.ops, logical_extensions, 1);
                    if (jindex_token)
                        options_.jindex_simulation->recordAccepted(*jindex_token, candidate_index);
                    if (physical_candidates) OPS_ADD(options_.executor.ops, physical_extensions, 1);
                    if (order_simulator_) order_simulator_->recordActualExtension();
                    std::vector<TimeInterval> previous_active =
                        std::move(state.active_intervals);
                    const bool previous_has_active =
                        state.has_active_intervals;
                    state.active_intervals = std::move(next_active);
                    state.has_active_intervals = true;
                    state.mapping[
                        static_cast<std::size_t>(query_vertex)] =
                        candidate;

                    const std::size_t child_depth = depth + 1;
                    bool spawned = false;
                    if (prefix_queue != nullptr &&
                        child_depth <= prefix_depth_limit &&
                        prefix_queue->mayAcceptPrefix()) {
                        SearchTask task;
                        task.state = state;
                        task.depth = child_depth;
                        spawned =
                            prefix_queue->tryPushPrefix(
                                std::move(task));
                    }
                    if (!spawned) {
                        self(
                            self,
                            state,
                            child_depth,
                            result,
                            output_match_index,
                            worker_metrics,
                            prefix_queue);
                    }

                    state.mapping[
                        static_cast<std::size_t>(query_vertex)] = -1;
                    state.active_intervals =
                        std::move(previous_active);
                    state.has_active_intervals =
                        previous_has_active;
                    if (result.counter.overflowed) return;
                }
            };

    auto enumerate_root = [&](std::size_t root_index,
                              RootResult& result,
                              std::uint64_t& output_match_index,
                              WorkerMetrics& worker_metrics,
                              DynamicPrefixQueue<CollectStatistics>*
                                  prefix_queue) {
        SearchState state;
        if (!initialize_root(root_index, state)) return;
        dfs(
            dfs,
            state,
            1,
            result,
            output_match_index,
            worker_metrics,
            prefix_queue);
    };

    // Full output is intentionally streamed in deterministic root/DFS order.
    // Count-only mode can either assign complete roots or dynamically expose
    // bounded prefix subtrees to idle workers.
    std::vector<WorkerMetrics> worker_metrics;
    WorkerMetrics disabled_worker_metrics;
    auto worker_metric_for = [&](std::size_t worker) -> WorkerMetrics& {
        if constexpr (CollectStatistics) {
            return worker_metrics[worker];
        }
        return disabled_worker_metrics;
    };
    std::vector<RootResult> worker_results;
#if defined(DSM_E3E4_CORE_ONLY)
    if (e3e4_run_context_ != nullptr) {
        const QueryPlan& backend_plan = executor_ != nullptr
            ? executor_->e3e4ExecutablePlanReference()
            : direct_evaluator_->e3e4ExecutablePlanReference();
        if (&backend_plan != &plan_) {
            throw std::logic_error(
                "E3/E4 selected backend retained a different QueryPlan object");
        }
        e3e4_run_context_->observe(
            GopherE3E4NativePhase::Enumeration,
            backend_plan,
            executor_ != nullptr);
    }
#endif
#if !defined(DSM_E3E4_CORE_ONLY)
    if (mode == MatchOutputMode::Full) {
        summary.threads_used = 1;
        summary.parallel_mode_effective = "serial-output";
        if constexpr (CollectStatistics) {
            worker_metrics.resize(1);
        }
        worker_results.resize(1);
        std::uint64_t output_match_index = 0;
        for (std::size_t root_index = 0;
             root_index < roots.size();
             ++root_index) {
            const auto task_start =
                workerMeasurementStart<CollectStatistics>();
            if constexpr (CollectStatistics) {
                ++worker_metrics[0].tasks;
            }
            enumerate_root(
                root_index,
                worker_results[0],
                output_match_index,
                worker_metric_for(0),
                nullptr);
            if constexpr (CollectStatistics) {
                worker_metrics[0].active_nanoseconds +=
                    workerMeasurementElapsed<true>(task_start);
            }
            if (cancel.load(std::memory_order_relaxed)) break;
        }
        if constexpr (CollectStatistics) {
            summary.parallel_statistics.root_tasks_claimed =
                static_cast<std::uint64_t>(worker_metrics[0].tasks);
            summary.parallel_statistics.queue_claims =
                static_cast<std::uint64_t>(worker_metrics[0].tasks);
        }
        summary.parallel_statistics.prefix_depth_limit =
            prefix_depth_limit;
    } else
#endif
    if (options_.parallel_mode == ParallelMode::Roots ||
               options_.threads == 1) {
        summary.parallel_mode_effective =
            options_.parallel_mode == ParallelMode::Prefix
            ? "roots-single-worker"
            : "roots";
        const std::size_t worker_count =
#if defined(DSM_E3E4_CORE_ONLY)
            options_.threads;
#else
            roots.empty()
                ? 1
                : std::min(options_.threads, roots.size());
#endif
        summary.threads_used = worker_count;
        primary_worker_work_units.assign(worker_count, 0);
        if constexpr (CollectStatistics) {
            worker_metrics.resize(worker_count);
        }
        worker_results.resize(worker_count);
        std::atomic<std::size_t> next_root{0};
        std::mutex exception_mutex;
        std::exception_ptr worker_exception;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        try {
            for (std::size_t worker = 0;
                 worker < worker_count;
                 ++worker) {
                workers.emplace_back([&, worker]() {
                    const std::size_t live_now =
                        live_workers.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                    dsm::pure_online_v2::updateAtomicPeak(
                        peak_live_workers, live_now);
                    bool executing_work_unit = false;
                    auto begin_work_unit = [&]() {
                        executing_work_unit = true;
                        ++primary_worker_work_units[worker];
                        const std::size_t productive_now =
                            productive_workers.fetch_add(
                                1, std::memory_order_relaxed) + 1;
                        dsm::pure_online_v2::updateAtomicPeak(
                            peak_productive_workers,
                            productive_now);
                    };
                    auto end_work_unit = [&]() {
                        if (!executing_work_unit) return;
                        executing_work_unit = false;
                        productive_workers.fetch_sub(
                            1, std::memory_order_relaxed);
                    };
                    try {
#if defined(DSM_E3E4_CORE_ONLY)
                        if (e3e4_run_context_ != nullptr) {
                            e3e4_run_context_->bindWorker(worker);
                        }
#endif
                    std::uint64_t unused_output_index = 0;
                    while (!cancel.load(std::memory_order_relaxed)) {
                        const std::size_t root_index =
                            next_root.fetch_add(
                                1, std::memory_order_relaxed);
                        if (root_index >= roots.size()) break;
                        const auto task_start =
                            workerMeasurementStart<CollectStatistics>();
                        begin_work_unit();
                        if constexpr (CollectStatistics) {
                            ++worker_metrics[worker].tasks;
                        }
                        enumerate_root(
                            root_index,
                            worker_results[worker],
                            unused_output_index,
                            worker_metric_for(worker),
                            nullptr);
                        end_work_unit();
                        if constexpr (CollectStatistics) {
                            worker_metrics[worker].active_nanoseconds +=
                                workerMeasurementElapsed<true>(task_start);
                        }
                    }
                    } catch (...) {
                        end_work_unit();
                        {
                            std::lock_guard<std::mutex> lock(
                                exception_mutex);
                            if (!worker_exception) {
                                worker_exception =
                                    std::current_exception();
                            }
                        }
                        cancel.store(true, std::memory_order_relaxed);
                    }
                    live_workers.fetch_sub(1, std::memory_order_relaxed);
                });
                summary.primary_metrics.worker_threads_created =
                    workers.size();
            }
        } catch (...) {
            cancel.store(true, std::memory_order_relaxed);
            for (auto& worker : workers) {
                if (worker.joinable()) worker.join();
            }
            throw;
        }
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
        if (worker_exception) std::rethrow_exception(worker_exception);
        if constexpr (CollectStatistics) {
            summary.parallel_statistics.root_tasks_claimed =
                static_cast<std::uint64_t>(
                    std::min(next_root.load(std::memory_order_relaxed),
                             roots.size()));
            summary.parallel_statistics.queue_claims =
                summary.parallel_statistics.root_tasks_claimed;
        }
    } else {
        summary.parallel_mode_effective = "prefix";
        const std::size_t worker_count =
#if defined(DSM_E3E4_CORE_ONLY)
            options_.threads;
#else
            roots.empty() ? 1 : options_.threads;
#endif
        summary.threads_used = worker_count;
        primary_worker_work_units.assign(worker_count, 0);
        if constexpr (CollectStatistics) {
            worker_metrics.resize(worker_count);
        }
        worker_results.resize(worker_count);

        const std::size_t max_size =
            std::numeric_limits<std::size_t>::max();
        const std::size_t task_queue_capacity =
            options_.tasks_per_thread >
                    max_size / worker_count
            ? max_size
            : std::max<std::size_t>(
                  1, options_.tasks_per_thread * worker_count);
        DynamicPrefixQueue<CollectStatistics> prefix_queue(
            roots.size(),
            task_queue_capacity);
        std::mutex exception_mutex;
        std::exception_ptr worker_exception;
        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        for (std::size_t worker = 0;
             worker < worker_count;
             ++worker) {
            workers.emplace_back([&, worker]() {
                const std::size_t live_now =
                    live_workers.fetch_add(
                        1, std::memory_order_relaxed) + 1;
                dsm::pure_online_v2::updateAtomicPeak(
                    peak_live_workers, live_now);
                bool executing_work_unit = false;
                auto begin_work_unit = [&]() {
                    executing_work_unit = true;
                    ++primary_worker_work_units[worker];
                    const std::size_t productive_now =
                        productive_workers.fetch_add(
                            1, std::memory_order_relaxed) + 1;
                    dsm::pure_online_v2::updateAtomicPeak(
                        peak_productive_workers,
                        productive_now);
                };
                auto end_work_unit = [&]() {
                    if (!executing_work_unit) return;
                    executing_work_unit = false;
                    productive_workers.fetch_sub(
                        1, std::memory_order_relaxed);
                };
                try {
#if defined(DSM_E3E4_CORE_ONLY)
                    if (e3e4_run_context_ != nullptr) {
                        e3e4_run_context_->bindWorker(worker);
                    }
#endif
                    std::uint64_t unused_output_index = 0;
                    SearchTask task;
                    while (!cancel.load(std::memory_order_relaxed) &&
                           prefix_queue.take(task)) {
                        const auto task_start =
                            workerMeasurementStart<CollectStatistics>();
                        begin_work_unit();
                        if constexpr (CollectStatistics) {
                            ++worker_metrics[worker].tasks;
                        }
                        if (task.is_root) {
                            enumerate_root(
                                task.root_index,
                                worker_results[worker],
                                unused_output_index,
                                worker_metric_for(worker),
                                &prefix_queue);
                        } else {
                            dfs(
                                dfs,
                                task.state,
                                task.depth,
                                worker_results[worker],
                                unused_output_index,
                                worker_metric_for(worker),
                                &prefix_queue);
                        }
                        end_work_unit();
                        if constexpr (CollectStatistics) {
                            worker_metrics[worker].active_nanoseconds +=
                                workerMeasurementElapsed<true>(task_start);
                        }
                        if (worker_results[worker].counter.overflowed) {
                            cancel.store(
                                true, std::memory_order_relaxed);
                            prefix_queue.cancel();
                        } else {
                            prefix_queue.complete();
                        }
                    }
                } catch (...) {
                    end_work_unit();
                    {
                        std::lock_guard<std::mutex> lock(
                            exception_mutex);
                        if (!worker_exception) {
                            worker_exception =
                                std::current_exception();
                        }
                    }
                    cancel.store(true, std::memory_order_relaxed);
                    prefix_queue.cancel();
                }
                live_workers.fetch_sub(1, std::memory_order_relaxed);
            });
            summary.primary_metrics.worker_threads_created =
                workers.size();
        }
        for (auto& worker : workers) worker.join();
        if (worker_exception) std::rethrow_exception(worker_exception);
        summary.parallel_statistics =
            prefix_queue.snapshot(prefix_depth_limit);
        primary_prefix_work_units_spawned =
            prefix_queue.primaryPrefixTasksSpawned();
    }

#if defined(DSM_E3E4_CORE_ONLY)
    if (e3e4_run_context_ != nullptr) {
        const QueryPlan& backend_plan = executor_ != nullptr
            ? executor_->e3e4ExecutablePlanReference()
            : direct_evaluator_->e3e4ExecutablePlanReference();
        if (&backend_plan != &plan_) {
            throw std::logic_error(
                "E3/E4 selected backend QueryPlan reference drifted");
        }
        e3e4_run_context_->observe(
            GopherE3E4NativePhase::CountReduction,
            backend_plan,
            executor_ != nullptr);
    }
#endif
    CheckedMatchCounter total;
    DigestAccumulator digest;
    for (const RootResult& worker : worker_results) {
#if defined(DSM_E3E4_CORE_ONLY)
        if (worker.counter.overflowed) {
            // The closed receipt carries the exact uint64 saturation value and
            // a separate overflow bit.  A worker-local overflow therefore
            // saturates the reduction even when that worker is encountered
            // before any successfully merged worker.
            total.value = std::numeric_limits<std::uint64_t>::max();
            total.overflowed = true;
            break;
        }
#endif
        if (worker.counter.overflowed ||
            !checkedAdd(total, worker.counter.value)) {
            total.overflowed = true;
            break;
        }
        digest.merge(worker.digest);
    }

    const auto primary_delta =
        dsm::pure_online_v2::endInterval(primary_start);
    summary.enum_start_monotonic_ns = primary_start.wall_ns;
    summary.enum_end_monotonic_ns = primary_start.wall_ns + primary_delta.wall_ns;
    dsm::pure_online_v2::captureEnvironmentEnd(summary.primary_metrics);
    dsm::pure_online_v2::addInterval(
        summary.primary_metrics, primary_delta);
    summary.primary_metrics.peak_live_worker_threads =
        peak_live_workers.load(std::memory_order_relaxed);
    summary.primary_metrics.workers_with_tasks =
        peak_productive_workers.load(std::memory_order_relaxed);
    summary.primary_metrics.parallel_interval_count = 1;
    const std::uint64_t root_work_units =
        static_cast<std::uint64_t>(roots.size());
    std::uint64_t work_units_total = root_work_units;
    if (summary.parallel_mode_effective == "prefix") {
        dsm::pure_online_v2::checkedAdd(
            work_units_total,
            primary_prefix_work_units_spawned);
    }
    summary.primary_metrics.parallel_work_units_min = work_units_total;
    summary.primary_metrics.parallel_work_units_max = work_units_total;
    summary.primary_metrics.parallel_work_units_total = work_units_total;
    if (primary_worker_work_units.empty()) {
        summary.primary_metrics.parallel_work_units_claimed =
            root_work_units;
    } else {
        for (std::uint64_t work_units : primary_worker_work_units) {
            dsm::pure_online_v2::checkedAdd(
                summary.primary_metrics.parallel_work_units_claimed,
                work_units);
        }
    }
    if (work_units_total >= options_.threads) {
        summary.primary_metrics
            .parallel_intervals_with_at_least_requested_work = 1;
    }
    summary.primary_metrics.parallel_workers_with_tasks_min =
        summary.primary_metrics.workers_with_tasks;
    summary.primary_metrics.parallel_workers_with_tasks_max =
        summary.primary_metrics.workers_with_tasks;
    summary.primary_metrics.parallel_workers_with_tasks_sum =
        summary.primary_metrics.workers_with_tasks;
    summary.primary_metrics.parallelism_eligible =
        summary.primary_metrics
            .parallel_intervals_with_at_least_requested_work ==
        summary.primary_metrics.parallel_interval_count;

    if constexpr (CollectStatistics) {
        if (!worker_metrics.empty()) {
            std::uint64_t tasks_min =
                std::numeric_limits<std::uint64_t>::max();
            std::uint64_t dfs_min =
                std::numeric_limits<std::uint64_t>::max();
            std::uint64_t active_min =
                std::numeric_limits<std::uint64_t>::max();
            long double tasks_squares = 0.0L;
            long double dfs_squares = 0.0L;
            long double active_squares = 0.0L;
            for (const WorkerMetrics& worker : worker_metrics) {
                if (worker.tasks > 0) {
                    ++summary.parallel_statistics.workers_with_tasks;
                }
                tasks_min = std::min(tasks_min, worker.tasks);
                dfs_min = std::min(dfs_min, worker.dfs_states);
                active_min =
                    std::min(active_min, worker.active_nanoseconds);
                summary.parallel_statistics.worker_tasks_max =
                    std::max(
                        summary.parallel_statistics.worker_tasks_max,
                        worker.tasks);
                summary.parallel_statistics.worker_tasks_total +=
                    worker.tasks;
                summary.parallel_statistics.worker_dfs_states_max =
                    std::max(
                        summary.parallel_statistics.worker_dfs_states_max,
                        worker.dfs_states);
                summary.parallel_statistics.worker_dfs_states_total +=
                    worker.dfs_states;
                summary.parallel_statistics.worker_active_nanoseconds_max =
                    std::max(
                        summary.parallel_statistics
                            .worker_active_nanoseconds_max,
                        worker.active_nanoseconds);
                summary.parallel_statistics
                    .worker_active_nanoseconds_total +=
                    worker.active_nanoseconds;
                const long double tasks =
                    static_cast<long double>(worker.tasks);
                const long double dfs_count =
                    static_cast<long double>(worker.dfs_states);
                const long double active =
                    static_cast<long double>(worker.active_nanoseconds);
                tasks_squares += tasks * tasks;
                dfs_squares += dfs_count * dfs_count;
                active_squares += active * active;
            }
            summary.parallel_statistics.worker_tasks_min = tasks_min;
            summary.parallel_statistics.worker_dfs_states_min = dfs_min;
            summary.parallel_statistics.worker_active_nanoseconds_min =
                active_min;

            const long double count =
                static_cast<long double>(worker_metrics.size());
            auto coefficient_of_variation =
                [count](std::uint64_t value_total,
                        long double squares) {
                    if (value_total == 0 || count == 0.0L) return 0.0;
                    const long double mean =
                        static_cast<long double>(value_total) / count;
                    const long double variance = std::max(
                        0.0L,
                        squares / count - mean * mean);
                    return static_cast<double>(
                        std::sqrt(variance) / mean);
                };
            summary.parallel_statistics.worker_tasks_cv =
                coefficient_of_variation(
                    summary.parallel_statistics.worker_tasks_total,
                    tasks_squares);
            summary.parallel_statistics.worker_dfs_states_cv =
                coefficient_of_variation(
                    summary.parallel_statistics
                        .worker_dfs_states_total,
                    dfs_squares);
            summary.parallel_statistics
                .worker_active_nanoseconds_cv =
                coefficient_of_variation(
                    summary.parallel_statistics
                        .worker_active_nanoseconds_total,
                    active_squares);
        }
    }

    statistics_.setSpecialized<CollectStatistics>(
        statistics_.search_nanoseconds,
        summary.primary_metrics.wall_ns);

    summary.match_count = total.value;
    summary.count_overflow = total.overflowed;
    summary.enumeration_milliseconds = static_cast<long long>(
        summary.primary_metrics.wall_ns / UINT64_C(1000000));
    summary.enumeration_nanoseconds = summary.primary_metrics.wall_ns;
    if constexpr (CollectStatistics) {
        if (summary.threads_used > 0 &&
            summary.primary_metrics.wall_ns > 0) {
            const long double capacity =
                static_cast<long double>(summary.threads_used) *
                static_cast<long double>(
                    summary.primary_metrics.wall_ns);
            summary.parallel_statistics.worker_utilization =
                static_cast<double>(
                    static_cast<long double>(
                        summary.parallel_statistics
                            .worker_active_nanoseconds_total) /
                    capacity);
        }
    }
    summary.result_digest = options_.result_digest
        ? digest.text()
        : "disabled";
    if (order_simulator_) summary.order_simulation = order_simulator_->snapshot();
    summary.statistics = statistics_.snapshot();
    if (dag_ != nullptr) {
        summary.statistics.dag_nodes_attempted =
            dag_->nodes_attempted;
        summary.statistics.dag_nodes_materialized =
            dag_->nodes.size();
        summary.statistics.dag_nodes_merged =
            dag_->nodes_merged;
    }
    if (executor_) {
        executor_->augmentStatistics(summary.statistics);
        summary.cache_memory_bytes =
            executor_->cacheMemoryUsage();
    } else {
        summary.cache_memory_bytes =
            direct_evaluator_->cacheMemoryUsage();
    }
    return summary;
}

#if defined(DSM_GOPHER_OFF_TRANSLATION_UNIT)
template GopherMatchSummary
GopherMatcher::runSpecialized<false, false>(
    MatchOutputMode mode,
    std::ostream* match_rows);
#else
template GopherMatchSummary
GopherMatcher::runSpecialized<false, true>(
    MatchOutputMode mode,
    std::ostream* match_rows);
#if !defined(DSM_ERASE_SEARCH_STATISTICS)
template GopherMatchSummary
GopherMatcher::runSpecialized<true, true>(
    MatchOutputMode mode,
    std::ostream* match_rows);
template GopherMatchSummary
GopherMatcher::runSpecialized<true, false>(
    MatchOutputMode mode,
    std::ostream* match_rows);
#endif
#endif

#if !defined(DSM_GOPHER_OFF_TRANSLATION_UNIT) && \
    !defined(DSM_E3E4_CORE_ONLY)
GopherMatchSummary GopherMatcher::save_res(
    const std::string& filename,
    MatchOutputMode mode) {
    GopherMatchSummary summary;
    std::ofstream output(filename, std::ios::binary);
    if (!output.is_open()) return summary;

    output << "[Run]\n"
           << "engine: gopher\n"
           << "mode: " << matchOutputModeName(mode) << '\n'
           << "memo: "
           << memoModeName(options_.executor.memo_mode) << '\n'
           << "memo_policy: "
           << memoPolicyName(options_.executor.memo_policy) << '\n'
           << "order: " << orderModeName(options_.order_mode) << '\n'
           << "selectivity_mode: "
           << selectivityModeName(plan_.selectivity_mode) << '\n'
           << "selectivity_formula_id: "
           << selectivityFormulaId(plan_.selectivity_mode) << '\n'
           << "query_root: " << plan_.root << '\n'
           << "root_selectivity_score: "
           << std::setprecision(17)
           << plan_.root_selectivity_score << '\n'
           << "plan_id: " << plan_.plan_id << '\n'
           << "threads_requested: " << options_.threads << '\n'
           << "parallel_mode_requested: "
           << parallelModeName(options_.parallel_mode) << '\n'
           << "prefix_depth_requested: "
           << options_.prefix_depth << '\n'
           << "tasks_per_thread: "
           << options_.tasks_per_thread << '\n'
           << "early_prune: "
           << (options_.early_prune ? "on" : "off") << '\n'
           << "result_digest: "
           << (options_.result_digest ? "on" : "off") << '\n'
           << "statistics: "
           << (options_.executor.collect_statistics ? "on" : "off")
           << '\n'
           << "runtime_key_diagnostics: "
           << runtimeKeyDiagnosticsModeName(
                  options_.executor.runtime_key_diagnostics)
           << '\n'
           << "exact_runtime_key_cap: "
           << options_.executor.exact_runtime_key_cap << '\n'
           << "cache_budget_bytes: "
           << options_.executor.memo_options.budget_bytes << '\n'
           << "cache_max_entry_bytes: "
           << options_.executor.memo_options.max_entry_bytes << '\n'
           << "cache_minimum_work: "
           << options_.executor.memo_options.minimum_estimated_work << '\n'
           << "plan_order:";
    for (int query_vertex : plan_.order) {
        output << ' ' << query_vertex;
    }
    output << '\n'
           << "verify_dag: "
           << (options_.executor.verify_dag ? "true" : "false")
           << "\n\n[Selectivity Summary]\n";
    td_tree_.writeSelectivitySummary(output);
    output << "\n[Candidate Summary]\n";
    td_tree_.writeCandidateSummary(output);
    output << "\n[Final Matches]\nCount: ";
    const std::streampos count_position = output.tellp();
    output << std::setw(20) << 0 << '\n';

    summary = run(mode, &output);

    const std::streampos end_position = output.tellp();
    output.seekp(count_position);
    if (summary.count_overflow) {
        output << std::setw(20) << "OVERFLOW";
    } else {
        output << std::setw(20) << summary.match_count;
    }
    output.seekp(end_position);
    output << "\n[Statistics]\n"
           << "mode: " << matchOutputModeName(mode) << '\n'
           << "count_overflow: "
           << (summary.count_overflow ? "true" : "false") << '\n'
           << "result_digest: " << summary.result_digest << '\n'
           << "threads_used: " << summary.threads_used << '\n'
           << "parallel_mode_effective: "
           << summary.parallel_mode_effective << '\n'
           << "parallel_statistics_enabled: "
           << (options_.executor.collect_statistics ? "on" : "off")
           << '\n';
    if (options_.executor.collect_statistics) {
        output
           << "parallel_statistics_collected: true\n"
           << "parallel_root_tasks_claimed: "
           << summary.parallel_statistics.root_tasks_claimed << '\n'
           << "parallel_prefix_tasks_spawned: "
           << summary.parallel_statistics.prefix_tasks_spawned << '\n'
           << "parallel_prefix_split_attempts: "
           << summary.parallel_statistics.prefix_split_attempts << '\n'
           << "parallel_prefix_split_rejected: "
           << summary.parallel_statistics.prefix_split_rejected << '\n'
           << "parallel_queue_claims: "
           << summary.parallel_statistics.queue_claims << '\n'
           << "parallel_queue_peak: "
           << summary.parallel_statistics.queue_peak << '\n'
           << "parallel_queue_state_bytes_peak: "
           << summary.parallel_statistics.queue_state_bytes_peak
           << '\n'
           << "parallel_workers_with_tasks: "
           << summary.parallel_statistics.workers_with_tasks << '\n'
           << "parallel_worker_tasks_min: "
           << summary.parallel_statistics.worker_tasks_min << '\n'
           << "parallel_worker_tasks_max: "
           << summary.parallel_statistics.worker_tasks_max << '\n'
           << "parallel_worker_tasks_total: "
           << summary.parallel_statistics.worker_tasks_total << '\n'
           << "parallel_worker_dfs_states_min: "
           << summary.parallel_statistics.worker_dfs_states_min << '\n'
           << "parallel_worker_dfs_states_max: "
           << summary.parallel_statistics.worker_dfs_states_max << '\n'
           << "parallel_worker_dfs_states_total: "
           << summary.parallel_statistics.worker_dfs_states_total << '\n'
           << "parallel_worker_active_ns_min: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_min << '\n'
           << "parallel_worker_active_ns_max: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_max << '\n'
           << "parallel_worker_active_ns_total: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_total << '\n'
           << "parallel_worker_tasks_cv: "
           << summary.parallel_statistics.worker_tasks_cv << '\n'
           << "parallel_worker_dfs_states_cv: "
           << summary.parallel_statistics.worker_dfs_states_cv << '\n'
           << "parallel_worker_active_ns_cv: "
           << summary.parallel_statistics
                  .worker_active_nanoseconds_cv << '\n'
           << "parallel_worker_utilization: "
           << summary.parallel_statistics.worker_utilization << '\n'
           << "parallel_task_queue_capacity: "
           << summary.parallel_statistics.task_queue_capacity << '\n'
           << "parallel_prefix_depth_limit: "
           << summary.parallel_statistics.prefix_depth_limit << '\n';
    } else {
        output << "parallel_statistics_collected: false\n";
    }
    output << "root_candidate_count: "
           << td_tree_.rootCandidates().size() << '\n'
           << "candidate_relation_entries: "
           << td_tree_.candidateRelationCount() << '\n'
           << "dag_memory_bytes: "
           << (dag_ == nullptr ? 0 : dag_->getMemoryUsage()) << '\n'
           << "cache_memory_bytes: "
           << summary.cache_memory_bytes << '\n'
           << "enumeration_ms: "
           << summary.enumeration_milliseconds << '\n';
    if (options_.executor.collect_statistics) {
        writeSearchStatistics(output, summary.statistics);
    } else {
        output << "statistics_collected: false\n";
    }
    output.flush();
    summary.output_written = output.good();
    return summary;
}
#endif
