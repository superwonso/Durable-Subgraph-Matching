#ifndef GOPHER_MATCHER_H
#define GOPHER_MATCHER_H

#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(DSM_E3E4_CORE_ONLY)
#include "../benchmark/e3_e4_adapter_v1/E3E4RuntimeHooksV1.h"
#endif

#include "CandidateDag.h"
#include "DagExecutor.h"
#include "DirectEvaluator.h"
#include "OrderSimulator.h"
#include "../original/PrimaryMetricsV2.h"
#include "SearchStatistics.h"
#include "TDTree.h"

class WindowStore;
class DirectP4Evaluator;
class WindowProgram;
class JIndexSimulation;
class JIndexExecution;
class BIndexExecution;
class UnionBlockGuard;

enum class JIndexMode { None, Adaptive, Always };
enum class BIndexMode { None, Adaptive, Always };

enum class ParallelMode {
    Roots,
    Prefix
};

const char* parallelModeName(ParallelMode mode);
bool parseParallelMode(const std::string& text, ParallelMode& mode);

struct ParallelExecutionStatistics {
    std::uint64_t root_tasks_claimed = 0;
    std::uint64_t prefix_tasks_spawned = 0;
    std::uint64_t prefix_split_attempts = 0;
    std::uint64_t prefix_split_rejected = 0;
    std::uint64_t queue_claims = 0;
    std::uint64_t queue_peak = 0;
    std::uint64_t queue_state_bytes_peak = 0;
    std::uint64_t worker_tasks_min = 0;
    std::uint64_t worker_tasks_max = 0;
    std::uint64_t worker_tasks_total = 0;
    std::uint64_t worker_dfs_states_min = 0;
    std::uint64_t worker_dfs_states_max = 0;
    std::uint64_t worker_dfs_states_total = 0;
    std::uint64_t worker_active_nanoseconds_min = 0;
    std::uint64_t worker_active_nanoseconds_max = 0;
    std::uint64_t worker_active_nanoseconds_total = 0;
    double worker_tasks_cv = 0.0;
    double worker_dfs_states_cv = 0.0;
    double worker_active_nanoseconds_cv = 0.0;
    double worker_utilization = 0.0;
    std::size_t workers_with_tasks = 0;
    std::size_t task_queue_capacity = 0;
    std::size_t prefix_depth_limit = 0;
};

struct GopherMatcherOptions {
    std::size_t threads = 1;
    ParallelMode parallel_mode = ParallelMode::Roots;
    // Zero selects the bounded automatic policy (root plus one query vertex).
    std::size_t prefix_depth = 0;
    // Maximum number of queued prefix subtrees per requested worker.
    std::size_t tasks_per_thread = 8;
    OrderMode order_mode = OrderMode::Paper;
    bool early_prune = true;
    bool result_digest = false;
    OrderSimulationMode order_simulation = OrderSimulationMode::Disabled;
    // V5 experimental switches. All off uses the original runSpecialized path.
    bool window_algebra = false;
    // V8-only control: loaded k=1 rows are raw T(e); query k remains unchanged.
    bool raw_bitset = false;
    bool window_pad_raw_words = false;
    bool window_block_union = false;
    bool window_lazy = false;
    bool direct_p4 = false;
    JIndexMode jindex_mode = JIndexMode::None;
    double jindex_alpha = 1.0;
    std::size_t jindex_block_size = 16;
    bool jindex_verify_superset = false; // Test-only exact necessary-condition check.
    // V6 switches are opt-in; an empty path retains the existing query W build.
    std::string wstore_path;
    std::string graph_file_sha256; // Computed from the actual input, not a CLI assertion.
    BIndexMode bindex_mode = BIndexMode::None;
    double bindex_alpha = 0.5;
    std::size_t bindex_block_size = 16;
    std::size_t bindex_min_size = 64;
    std::size_t bindex_reuse_threshold = 2;
    bool bindex_verify_superset = false;
    std::optional<std::uint64_t> graph_adjacency_temporal_capacity_bytes;
    JIndexSimulation* jindex_simulation = nullptr;
    // Test-only in-memory observation, including parallel CountOnly runs.
    // No CLI switch exposes this hook. A concurrent observer must synchronize
    // its own collection; the referenced state remains owned by its DFS frame.
    void (*exact_match_observer)(const SearchState&, void*) = nullptr;
    void* exact_match_observer_context = nullptr;
    DagExecutorOptions executor;
};

struct GopherMatchSummary {
    std::uint64_t match_count = 0;
    bool count_overflow = false;
    bool output_written = false;
    long long enumeration_milliseconds = 0;
    std::uint64_t enumeration_nanoseconds = 0;
    dsm::pure_online_v2::PrimaryMetrics primary_metrics;
    std::size_t threads_used = 0;
    std::string result_digest;
    ParallelMode parallel_mode_requested = ParallelMode::Roots;
    std::string parallel_mode_effective = "roots";
    ParallelExecutionStatistics parallel_statistics;
    SearchStatistics statistics;
    std::size_t cache_memory_bytes = 0;
    OrderSimulationSnapshot order_simulation;
    std::uint64_t enum_start_monotonic_ns = 0;
    std::uint64_t enum_end_monotonic_ns = 0;
    std::uint64_t v5_build_nanoseconds = 0;
    std::uint64_t v5_lazy_build_thread_nanoseconds = 0;
    std::uint64_t v5_window_bytes = 0;
    std::uint64_t v5_window_relevant_edges = 0;
    std::uint64_t v5_window_built_edges = 0;
    bool v5_window_forced_intervals = false;
    std::string jindex_execution_json;
    std::string bindex_execution_json;
    std::string wstore_loading_json;
    std::uint64_t v9_window_words = 0;
    std::uint64_t v9_padding_nanoseconds = 0;
    std::uint64_t v9_union_blocks_checked = 0, v9_union_blocks_skipped = 0;
    std::uint64_t v9_union_candidates_skipped = 0, v9_union_blocks_built = 0, v9_union_bytes = 0;
};

#if defined(DSM_E3E4_CORE_ONLY)
// The source-receipt-bound E3/E4 build has a deliberately smaller native
// observation surface than the legacy CLI.  Only the two boundaries that
// must originate inside the matcher core are observable here; planning, TD
// construction, and DAG compilation are owned by the closed adapter.
enum class GopherE3E4NativePhase : std::uint8_t {
    Enumeration,
    CountReduction,
};

class GopherE3E4NativeObserver {
public:
    virtual ~GopherE3E4NativeObserver() = default;

    virtual void onGopherE3E4NativePhase(
        GopherE3E4NativePhase phase,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            MonotonicClockReading& reading,
        const QueryPlan& executable_plan,
        bool dag_backend) = 0;
};

struct GopherE3E4ObservedRun final {
    GopherMatchSummary summary;
    std::vector<dsm::e3e4::adapter_v1::runtime_hooks_v1::
        WorkerAffinityObservation> worker_affinity;
    const QueryPlan* executable_plan_address = nullptr;
    bool dag_backend = false;
};

namespace dsm::e3e4::gopher_runtime_v1::test_support {

#if defined(DSM_E3E4_TEST_HOOKS)
class TestOnlyHookRunner;
#endif

}  // namespace dsm::e3e4::gopher_runtime_v1::test_support

namespace dsm::e3e4::gopher_adapter_v1 {

class AdapterProductionAccess;

}  // namespace dsm::e3e4::gopher_adapter_v1
#endif

class GopherMatcher {
public:
    ~GopherMatcher();
    GopherMatcher(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const CandidateDag& dag,
        const TDTree& td_tree,
        int minimum_duration,
        GopherMatcherOptions options);

    // Same-stack DurableDFS path.  This overload intentionally does not take
    // a CandidateDag, so callers can exclude DAG compilation from the direct
    // executor's phase boundary.
    GopherMatcher(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const TDTree& td_tree,
        int minimum_duration,
        GopherMatcherOptions options);

#if !defined(DSM_E3E4_CORE_ONLY)
    GopherMatchSummary run(
        MatchOutputMode mode = MatchOutputMode::CountOnly,
        std::ostream* match_rows = nullptr);
    GopherMatchSummary save_res(
        const std::string& filename,
        MatchOutputMode mode = MatchOutputMode::Full);
#endif

private:
    const Graph& G_;
    const Graph& Q_;
    const QueryPlan& plan_;
    const CandidateDag* dag_ = nullptr;
    const TDTree& td_tree_;
    int k_threshold_;
    GopherMatcherOptions options_;

    SearchStatisticsCollector statistics_;
    std::unique_ptr<DagExecutor> executor_;
    std::unique_ptr<DirectEvaluator> direct_evaluator_;
    std::unique_ptr<OrderSimulator> order_simulator_;
    std::unique_ptr<WindowStore> window_store_;
    std::unique_ptr<DirectP4Evaluator> direct_p4_;
    std::unique_ptr<WindowProgram> window_program_;
    std::unique_ptr<JIndexExecution> jindex_execution_;
    std::unique_ptr<BIndexExecution> bindex_execution_;
    std::unique_ptr<UnionBlockGuard> union_guard_;
    void prepareV5();
    GopherMatchSummary runV5(MatchOutputMode mode, std::ostream* match_rows);

#if defined(DSM_E3E4_CORE_ONLY)
    struct E3E4RunContext;
    E3E4RunContext* e3e4_run_context_ = nullptr;

    GopherMatchSummary run(
        MatchOutputMode mode,
        std::ostream* match_rows);
    GopherE3E4ObservedRun runE3E4CountOnlyWithHooks(
        GopherE3E4NativeObserver& observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            WorkerAffinityBinder& binder,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            MonotonicNowSource& clock);
    // Only the source-bound adapter TU's incomplete friend bridge can select
    // the sealed production runtime hooks.  No public one-argument matcher
    // entrypoint exposes production observations to an arbitrary caller.
    GopherE3E4ObservedRun runAdapterOwnedE3E4ClosedCountOnly(
        GopherE3E4NativeObserver& observer);

#if defined(DSM_E3E4_TEST_HOOKS)
    friend class dsm::e3e4::gopher_runtime_v1::test_support::
        TestOnlyHookRunner;
#endif
    friend class dsm::e3e4::gopher_adapter_v1::AdapterProductionAccess;
#endif

    template <bool CollectStatistics, bool ExactDiagnostics>
    DagExecutor::CandidateSetPtr structuralCandidates(
        int query_vertex,
        const SearchState& state);
    template <bool CollectStatistics, bool ExactDiagnostics>
    TemporalSupport temporalSupport(
        int query_vertex,
        int candidate,
        const SearchState& state);

    template <bool CollectStatistics, bool ExactDiagnostics>
    GopherMatchSummary runSpecialized(
        MatchOutputMode mode,
        std::ostream* match_rows);
};

// The timed statistics-OFF search kernel has a single explicit definition in
// the dedicated OFF object.  The E3/E4 build compiles that object with both
// DSM_E3E4_CORE_ONLY and DSM_GOPHER_OFF_TRANSLATION_UNIT so its closed native
// callbacks, affinity proof, and saturated-count semantics remain present.
extern template GopherMatchSummary
GopherMatcher::runSpecialized<false, false>(
    MatchOutputMode mode,
    std::ostream* match_rows);

#if defined(DSM_E3E4_CORE_ONLY) && defined(DSM_E3E4_TEST_HOOKS)
namespace dsm::e3e4::gopher_runtime_v1::test_support {

// Explicitly test-only access.  Scripted hook values can exercise the native
// state grammar but retain non-production provenance and therefore cannot be
// promoted to an authoritative ThreadObservation or phase receipt.
class TestOnlyHookRunner final {
public:
    [[nodiscard]] static GopherE3E4ObservedRun runCountOnly(
        GopherMatcher& matcher,
        GopherE3E4NativeObserver& observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            WorkerAffinityBinder& binder,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            MonotonicNowSource& clock);
};

}  // namespace dsm::e3e4::gopher_runtime_v1::test_support
#endif  // DSM_E3E4_CORE_ONLY && DSM_E3E4_TEST_HOOKS

#endif
