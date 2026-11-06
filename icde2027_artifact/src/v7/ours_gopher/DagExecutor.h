#ifndef DAG_EXECUTOR_H
#define DAG_EXECUTOR_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BoundedMemo.h"
#include "CandidateDag.h"
#include "SearchStatistics.h"
#include "TDTree.h"
#include "Utils.h"
#include "OperatorCounters.h"
#include "AdaptiveDiagnostics.h"
#include "PhysicalCandidateBatch.h"

class JIndexExecution;
namespace window_algebra { class WindowView; }

enum class MemoMode {
    None,
    Structural,
    Temporal,
    Dag
};

const char* memoModeName(MemoMode mode);
bool parseMemoMode(const std::string& text, MemoMode& mode);

// Legacy preserves the original all-operation memo path, including its
// cost-aware parent-before-probe behavior. StructuralV2 limits memoization to
// structural intersections and probes their exact dependency key before
// evaluating parent subtrees.
enum class MemoPolicy {
    Legacy,
    StructuralV2
};

const char* memoPolicyName(MemoPolicy policy);
bool parseMemoPolicy(const std::string& text, MemoPolicy& policy);

// Selects only the dynamic binding projection of a memo key. It does not
// change which DAG operators are memoized, their probe/admission order, or
// any cache budget, entry-size, or sharding policy.
enum class RuntimeKeyMode {
    FullPrefix,
    RequiredBindings
};

const char* runtimeKeyModeName(RuntimeKeyMode mode);
bool parseRuntimeKeyMode(const std::string& text, RuntimeKeyMode& mode);

// Exact diagnostics intentionally use equality-safe sets and are excluded
// from primary timing runs.  Off is the production/default mode.
enum class RuntimeKeyDiagnosticsMode {
    Off,
    Exact
};

const char* runtimeKeyDiagnosticsModeName(RuntimeKeyDiagnosticsMode mode);
bool parseRuntimeKeyDiagnosticsMode(
    const std::string& text,
    RuntimeKeyDiagnosticsMode& mode);

struct RuntimeKey {
    std::uint64_t static_fingerprint = 0;
    DagNodeId node_id = kInvalidDagNode;
    std::vector<int> bindings;

    bool operator==(const RuntimeKey& other) const {
        return static_fingerprint == other.static_fingerprint &&
            node_id == other.node_id &&
            bindings == other.bindings;
    }
};

struct RuntimeKeyHash {
    std::size_t operator()(const RuntimeKey& key) const noexcept;
};

struct SearchState {
    std::vector<int> mapping;
    std::vector<TimeInterval> active_intervals;
    bool has_active_intervals = false;
};

// Builds the exact memo identity used by DagExecutor. FullPrefix retains the
// complete positional query mapping (including the candidate overlay used by
// temporal operators); RequiredBindings projects that mapping onto the
// compiler-proven dependency set of the DAG node.
RuntimeKey buildRuntimeKey(
    RuntimeKeyMode mode,
    const DagNode& node,
    const SearchState& state,
    int overlay_query_vertex = -1,
    int overlay_data_vertex = -1);

struct DagExecutorOptions {
    MemoMode memo_mode = MemoMode::Dag;
    MemoPolicy memo_policy = MemoPolicy::Legacy;
    RuntimeKeyMode runtime_key_mode = RuntimeKeyMode::RequiredBindings;
    RuntimeKeyDiagnosticsMode runtime_key_diagnostics =
        RuntimeKeyDiagnosticsMode::Off;
    // Exact mode fails closed when this many distinct keys have already
    // been admitted and a new key is observed.  Zero is invalid in Exact
    // mode, which prevents an accidentally unbounded diagnostic run.
    std::uint64_t exact_runtime_key_cap = 0;
    MemoOptions memo_options;
    bool verify_dag = false;
    bool collect_statistics = true;
    OperatorCounters* ops = nullptr;
    AdaptiveDiagnostics* adaptive = nullptr;
    DagPasses direct_passes{};
    // V5 W compilation absorbs the Lmax predicate into exact WindowAnd.
    // Default false preserves every v4 physical structural operation.
    bool window_guards = false;
};

struct RuntimeKeyDiagnosticsSnapshot {
    bool exact = false;
    std::uint64_t cap = 0;
    std::uint64_t invocations = 0;
    std::uint64_t distinct_keys = 0;
    std::uint64_t key_construction_nanoseconds = 0;
    std::uint64_t cache_lookup_nanoseconds = 0;
    std::array<std::uint64_t, kDagOpCount> invocations_by_op{};
    std::array<std::uint64_t, kDagOpCount> distinct_keys_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        key_construction_nanoseconds_by_op{};
    std::array<std::uint64_t, kDagOpCount>
        cache_lookup_nanoseconds_by_op{};

    std::uint64_t reuseOpportunities() const {
        return invocations - distinct_keys;
    }
    double reuseFraction() const {
        return invocations == 0
            ? 0.0
            : static_cast<double>(reuseOpportunities()) /
                  static_cast<double>(invocations);
    }
};

template <typename Value>
class DagValue {
public:
    DagValue() = default;
    explicit DagValue(Value value)
        : local_(std::move(value)) {}
    explicit DagValue(std::shared_ptr<const Value> shared)
        : shared_(std::move(shared)) {}
    explicit DagValue(const Value* borrowed)
        : borrowed_(borrowed) {}

    const Value& get() const {
        if (shared_) return *shared_;
        return borrowed_ ? *borrowed_ : local_;
    }
    const Value& operator*() const { return get(); }
    const Value* operator->() const { return &get(); }

private:
    std::shared_ptr<const Value> shared_;
    const Value* borrowed_ = nullptr;
    Value local_;
};

struct TemporalSupport {
    DagValue<std::vector<TimeInterval>> intervals;
    int longest_run = 0;
    bool has_constraints = false;
    bool includes_cumulative_support = false;
};

class DagExecutor {
public:
    using CandidateSet = std::vector<int>;
    using IntervalSet = std::vector<TimeInterval>;
    using CandidateSetPtr = DagValue<CandidateSet>;
    using IntervalSetPtr = DagValue<IntervalSet>;

    DagExecutor(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryPlan& plan,
        const CandidateDag& dag,
        const TDTree& td_tree,
        int minimum_duration,
        DagExecutorOptions options,
        SearchStatisticsCollector& statistics);
    ~DagExecutor();

    CandidateSetPtr structuralCandidates(
        int query_vertex,
        const SearchState& state);

    template <bool CollectStatistics, bool ExactDiagnostics>
    CandidateSetPtr structuralCandidatesSpecialized(
        int query_vertex,
        const SearchState& state);

    TemporalSupport temporalSupport(
        int query_vertex,
        int candidate,
        const SearchState& state);

    // Physical IR results are owned by the caller's DFS frame. Temporal
    // payload evaluation must happen after injectivity and consume its row
    // explicitly; no shared mutable executor cursor/handle cache is used.
    PhysicalCandidateBatch physicalCandidates(int query_vertex, const SearchState& state);
    // P8 is an opt-in producer overlay; the original overload above remains
    // unchanged. The branch's immutable W support is passed explicitly, never
    // stored in this shared executor.
    PhysicalCandidateBatch physicalCandidates(
        int query_vertex, const SearchState& state, JIndexExecution& index,
        window_algebra::WindowView prior);
    void registerIndexDrivers(JIndexExecution& index) const;
    TemporalSupport temporalSupportPhysical(int query_vertex, int candidate,
        const SearchState& state, const ProbeHistoryRow* row);

    template <bool CollectStatistics, bool ExactDiagnostics>
    TemporalSupport temporalSupportSpecialized(
        int query_vertex,
        int candidate,
        const SearchState& state);

    void augmentStatistics(SearchStatistics& statistics) const;
    RuntimeKeyDiagnosticsSnapshot runtimeKeyDiagnostics() const;
    std::size_t cacheMemoryUsage() const;
#if defined(DSM_E3E4_CORE_ONLY)
    // Closed-source-profile proof hook: returns the exact reference retained
    // by the selected native backend, never a rebuilt or copied QueryPlan.
    [[nodiscard]] const QueryPlan&
    e3e4ExecutablePlanReference() const noexcept {
        return plan_;
    }
#endif

private:
    const Graph& G_;
    const Graph& Q_;
    const QueryPlan& plan_;
    const CandidateDag& dag_;
    const TDTree& td_tree_;
    int k_threshold_;
    DagExecutorOptions options_;
    SearchStatisticsCollector& statistics_;

    class ExactRuntimeKeyCollector;
    std::unique_ptr<ExactRuntimeKeyCollector> exact_runtime_keys_;

    BoundedMemo<RuntimeKey, CandidateSet, RuntimeKeyHash> structural_memo_;
    BoundedMemo<RuntimeKey, IntervalSet, RuntimeKeyHash> temporal_memo_;
    BoundedMemo<RuntimeKey, int, RuntimeKeyHash> longest_memo_;
    std::vector<std::uint8_t> structural_v2_static_cacheable_;
    std::uint64_t structural_v2_intersection_nodes_ = 0;
    std::uint64_t structural_v2_eligible_nodes_ = 0;

    bool structuralMemoEnabled() const;
    bool temporalMemoEnabled() const;
    bool longestMemoEnabled() const;
    bool structuralV2ProjectionCanRepeat(
        const DagNode& node,
        const SearchState& state) const;

    int resolveBinding(
        int query_vertex,
        const SearchState& state,
        int overlay_query_vertex,
        int overlay_data_vertex) const;

    CandidateSetPtr evaluateOptimizedCandidateNode(
        DagNodeId node_id, const SearchState& state);
    IntervalSetPtr evaluateOptimizedIntervalNode(
        DagNodeId node_id, int query_vertex, int candidate,
        const SearchState& state, const ProbeHistoryRow* row = nullptr);
    PhysicalCandidateBatch evaluatePhysicalCandidateNode(DagNodeId node_id, const SearchState& state);
    PhysicalCandidateBatch evaluatePhysicalChoiceNode(const DagNode& node, const SearchState& state);
    CandidateReadView physicalDescriptor(const DagNode& node, const SearchState& state);
    void recordSkippedSubtree(DagNodeId node_id) const;

    RuntimeKey makeRuntimeKey(
        const DagNode& node,
        const SearchState& state,
        int overlay_query_vertex,
        int overlay_data_vertex) const;

    template <bool CollectStatistics, bool ExactDiagnostics>
    RuntimeKey makeMeasuredRuntimeKey(
        const DagNode& node,
        const SearchState& state,
        int overlay_query_vertex,
        int overlay_data_vertex);

    template <bool CollectStatistics, bool ExactDiagnostics>
    void recordCacheLookupTime(
        DagOp op,
        std::uint64_t nanoseconds);

    template <bool CollectStatistics>
    void recordOperatorInvocation(DagOp op);
    template <bool CollectStatistics>
    void recordCacheProbe(DagOp op, bool hit);
    template <bool CollectStatistics>
    void recordOperatorCompute(
        DagOp op,
        std::uint64_t nanoseconds,
        bool cache_probe_missed);
    template <bool CollectStatistics>
    void recordOperatorResult(
        DagOp op,
        std::uint64_t result_units,
        int returned_run_length = -1);
    template <bool CollectStatistics>
    void recordCacheAdmission(
        DagOp op,
        const MemoInsertOutcome& outcome);
    void validateOperatorStatistics(
        SearchStatistics& statistics,
        const MemoSnapshot& structural,
        const MemoSnapshot& temporal,
        const MemoSnapshot& longest) const;

    template <bool CollectStatistics, bool ExactDiagnostics>
    CandidateSetPtr evaluateCandidateNode(
        DagNodeId node_id,
        const SearchState& state);

    template <bool CollectStatistics, bool ExactDiagnostics>
    IntervalSetPtr evaluateIntervalNode(
        DagNodeId node_id,
        int query_vertex,
        int candidate,
        const SearchState& state);

    template <bool CollectStatistics, bool ExactDiagnostics>
    int evaluateLongestNode(
        DagNodeId node_id,
        int query_vertex,
        int candidate,
        const SearchState& state,
        const IntervalSet& known_intervals);

    CandidateSet verifyStructural(
        int query_vertex,
        const SearchState& state) const;

    IntervalSet verifyTemporal(
        int query_vertex,
        int candidate,
        const SearchState& state) const;
};

// This exact specialization is the timed statistics-OFF path.  It is emitted
// once in a dedicated OFF object.  The E3/E4 target keeps CORE_ONLY enabled
// on that object and adds DSM_GOPHER_OFF_TRANSLATION_UNIT; it is not a
// profile-independent escape from the closed runtime contract.
extern template DagExecutor::CandidateSetPtr
DagExecutor::structuralCandidatesSpecialized<false, false>(
    int query_vertex,
    const SearchState& state);
extern template TemporalSupport
DagExecutor::temporalSupportSpecialized<false, false>(
    int query_vertex,
    int candidate,
    const SearchState& state);

#endif
