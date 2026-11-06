#ifndef TDTREE_H
#define TDTREE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "Utils.h"
#include "PrimaryMetricsV2.h"
#include "query_decomposition.h"

#if defined(DSM_E3E4_CORE_ONLY)
namespace dsm::e3e4::adapter_v1::td_tree {
class AdapterProductionAccess;
namespace test_support {
#if defined(DSM_E3E4_TEST_HOOKS)
class TestOnlyCoreAccess;
#endif
}  // namespace test_support
}  // namespace dsm::e3e4::adapter_v1::td_tree
#endif
#include "../benchmark/e3_e4_adapter_v1/E3E4RuntimeHooksV1.h"

// Clock-bearing extension of the historical phase observer.  The sealed
// runtime-hook reading preserves provenance across the native callback while
// leaving the pre-existing observer ABI available for non-authoritative
// callers.  Only the closed adapter converts a production reading into an
// authority timestamp.
class RuntimeHookedOriginalNativePhaseObserver
    : public OriginalNativePhaseObserver {
public:
    explicit RuntimeHookedOriginalNativePhaseObserver(
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource&
            clock) noexcept
        : clock_(clock) {}

    void onOriginalNativePhase(OriginalNativePhase phase) final {
        onRuntimeHookedOriginalNativePhase(phase, clock_.now());
    }

    [[nodiscard]] bool usesClockSource(
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource&
            clock) const noexcept {
        return &clock_ == &clock;
    }

protected:
    virtual void onRuntimeHookedOriginalNativePhase(
        OriginalNativePhase phase,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::
            MonotonicClockReading& reading) = 0;

private:
    const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource& clock_;
};

struct TDTreeCell {
    int data_vertex = -1;
    // Algorithm 1 stores the root-to-leaf tree-path time-instance set at a
    // leaf cell. Internal cells leave this empty.
    std::vector<TimeInterval> leaf_time_instances;
    // One block index per query-tree child, in spanning_tree_adj order.
    // This is the paper's auxiliary pointer from a parent cell to a child
    // block. Indexes are local to the corresponding child TD-tree node.
    std::vector<std::size_t> child_block_indices;
};

struct TDTreeBlock {
    int v_par = -1;
    std::vector<TDTreeCell> candidates;
    // Section 7.2 compaction for leaf blocks: this is the union of the
    // candidates' original leaf sets, while each cell stores union \ original.
    std::vector<TimeInterval> leaf_time_union;
    bool leaf_time_compacted = false;
};

struct TDTreeNode {
    int query_vertex_id = -1;
    bool isRoot = false;
    bool isLeaf = false;
    std::vector<TDTreeCell> root_candidates;
    std::vector<TDTreeBlock> blocks;
};

struct MatchSummary {
    std::uint64_t match_count = 0;
    // Historical aggregate wall fields retained for legacy diagnostics.
    // The observed E3/E4 path deliberately does not promote either field as
    // a pure enumeration measurement: use its PhaseLedgerSnapshot for the
    // authoritative enumeration/output-finalization split.
    long long enumeration_milliseconds = 0;
    std::uint64_t enumeration_nanoseconds = 0;
    std::uint64_t matching_plan_nanoseconds = 0;
    dsm::pure_online_v2::PrimaryMetrics primary_metrics;
    bool output_written = false;
    bool count_overflow = false;
    std::size_t threads_requested = 1;
    std::size_t threads_used = 1;
    std::size_t workers_with_tasks = 0;
    std::size_t root_tasks_claimed = 0;
    std::size_t worker_root_tasks_min = 0;
    std::size_t worker_root_tasks_max = 0;
    // Populated only by the observed E3/E4 count-only entrypoint. Entry i is
    // the exact singleton CPU to which worker i was bound and re-observed
    // before it began search work.
    bool exact_worker_affinity_observed = false;
    std::vector<std::uint32_t> affinity_cpu_set;
    std::string worker_affinity_error;
    // Unforgeable observations returned by the injected binder.  Raw CPU IDs
    // above remain for legacy diagnostics; closed receipt authority is
    // derived only by aggregating this vector in the production adapter.
    std::vector<dsm::e3e4::adapter_v1::runtime_hooks_v1::
        WorkerAffinityObservation> worker_affinity_observations;
    std::size_t root_candidate_count = 0;
    std::uint64_t dfs_states = 0;
    std::uint64_t generated_candidates = 0;
    std::uint64_t accepted_candidates = 0;
    std::uint64_t completed_matches = 0;
    std::uint64_t pruned_injectivity = 0;
    std::uint64_t pruned_temporal_support = 0;
    // Section 7.2 compaction first tests the union stored by a compacted leaf
    // block.  These counters distinguish that whole-block optimization from
    // the exact per-cell reconstruction that follows when the test passes.
    std::uint64_t leaf_block_precheck_attempts = 0;
    std::uint64_t leaf_blocks_pruned = 0;
    std::uint64_t leaf_block_candidates_pruned = 0;
    // The exact expansion plan consumed by enumerateMatches.  The existing
    // `plan_order` result field remains the decomposition traversal for
    // backward compatibility; these fields expose the Section 8.5 path-order
    // ablation without conflating the two orders.
    std::vector<int> generated_matching_order;
    std::vector<std::vector<int>> query_path_order;
};

enum class MatchOutputMode {
    Full,
    CountOnly
};

const char* matchOutputModeName(MatchOutputMode mode);

struct CheckedMatchCounter {
    std::uint64_t value = 0;
    bool overflowed = false;

    bool increment();
};

enum class PathOrderMode {
    Sorted,
    Random
};

const char* pathOrderModeName(PathOrderMode mode);

class TDTree {
public:
    TDTree(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration,
        DurationMode duration_mode = DurationMode::Cardinality,
        PathOrderMode path_order_mode = PathOrderMode::Sorted,
        std::uint64_t path_order_seed = UINT64_C(2022),
        bool time_compaction = true);

#if !defined(DSM_E3E4_CORE_ONLY)
    // Versioned integration overload.  The observer is notified immediately
    // before native TD construction begins.  Existing construction remains
    // source-compatible and has no observer side effects.
    TDTree(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration,
        OriginalNativePhaseObserver& phase_observer,
        DurationMode duration_mode = DurationMode::Cardinality,
        PathOrderMode path_order_mode = PathOrderMode::Sorted,
        std::uint64_t path_order_seed = UINT64_C(2022),
        bool time_compaction = true);
#endif

#if !defined(DSM_E3E4_CORE_ONLY)
    void print_res() const;
    MatchSummary save_res(
        const std::string& filename,
        MatchOutputMode mode = MatchOutputMode::Full,
        std::size_t threads = 1) const;
#endif  // !defined(DSM_E3E4_CORE_ONLY)
    // Direct count-only entrypoints.  They never construct a filename, open a
    // stream backed by a file, or serialize legacy result text.
#if !defined(DSM_E3E4_CORE_ONLY)
    MatchSummary countMatches(std::size_t threads = 1) const;
    MatchSummary countMatches(
        std::size_t threads,
        OriginalNativePhaseObserver& phase_observer) const;
#endif  // !defined(DSM_E3E4_CORE_ONLY)
    std::size_t getMemoryUsage() const;
    // Logical live bytes in the TD-tree representation. Unlike
    // getMemoryUsage(), this uses vector sizes rather than capacities and is
    // excludes construction-only query requirements, and is therefore the
    // allocator-independent index metric used for the DSM/DSM_nc Section 8.6
    // comparison.
    std::size_t getLogicalMemoryUsageBytes() const;
    std::size_t candidateRelationCount() const;

private:
#if defined(DSM_E3E4_CORE_ONLY)
    TDTree(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration,
        RuntimeHookedOriginalNativePhaseObserver& phase_observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource&
            clock,
        DurationMode duration_mode,
        PathOrderMode path_order_mode,
        std::uint64_t path_order_seed,
        bool time_compaction);
    MatchSummary countMatchesAdapterOwnedWithRuntimeHooks(
        std::size_t threads,
        RuntimeHookedOriginalNativePhaseObserver& phase_observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::MonotonicNowSource&
            clock,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder&
            affinity_binder) const;
    friend class dsm::e3e4::adapter_v1::td_tree::AdapterProductionAccess;
#if defined(DSM_E3E4_TEST_HOOKS)
    friend class dsm::e3e4::adapter_v1::td_tree::test_support::
        TestOnlyCoreAccess;
#endif
#endif
    const Graph& G;
    const Graph& Q;
    const QueryDecomposition& QD;
    int k_threshold;
    DurationMode duration_mode;
    PathOrderMode path_order_mode;
    std::uint64_t path_order_seed;
    bool time_compaction;

    std::vector<TDTreeNode> nodes;
    std::vector<std::array<int, kLabelCount>> query_out_neighbor_label_requirements;
    std::vector<std::array<int, kLabelCount>> query_in_neighbor_label_requirements;

    void build();
    void initializeNodes();
    void initializeQueryRequirements();
    void growAndTrim();

    bool isDataVertexCandidate(int data_vertex, int query_vertex) const;
    bool buildChildren(
        int query_vertex,
        TDTreeCell& parent_cell,
        std::vector<int>& path_mapping,
        const std::vector<TimeInterval>& path_time_instances,
        bool has_path_time_instances);
    bool passesMappedNonTreeConstraints(
        int data_vertex,
        int query_vertex,
        const std::vector<int>& path_mapping) const;
    bool treeEdgeTimeInstances(
        int parent_query_vertex,
        int child_query_vertex,
        int parent_data_vertex,
        int child_data_vertex,
        std::vector<TimeInterval>& time_instances) const;
    std::vector<std::size_t> captureBlockSizes() const;
    void rollbackBlocks(const std::vector<std::size_t>& sizes);
    void compactLeafBlock(TDTreeBlock& block) const;
    struct MatchingPlan {
        std::vector<int> matching_order;
        std::vector<std::vector<int>> query_paths;
    };
    MatchingPlan generateMatchingPlan() const;

    std::vector<int> uniqueCandidates(const TDTreeNode& node) const;
    std::size_t uniqueCandidateCount(const TDTreeNode& node) const;
    CheckedMatchCounter enumerateMatches(
        std::ostream& output,
        MatchOutputMode mode,
        std::size_t requested_threads,
        MatchSummary& summary,
        const MatchingPlan* prepared_matching_plan = nullptr,
        OriginalNativePhaseObserver* phase_observer = nullptr,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder*
            affinity_binder = nullptr) const;
    MatchSummary countMatchesImpl(
        std::size_t threads,
        OriginalNativePhaseObserver* phase_observer,
        const dsm::e3e4::adapter_v1::runtime_hooks_v1::WorkerAffinityBinder*
            affinity_binder) const;
};

#if defined(DSM_E3E4_CORE_ONLY) && defined(DSM_E3E4_TEST_HOOKS)
namespace dsm::e3e4::adapter_v1::td_tree::test_support {

class TestOnlyCoreAccess final {
public:
    static TDTree construct(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration,
        RuntimeHookedOriginalNativePhaseObserver& phase_observer,
        const runtime_hooks_v1::MonotonicNowSource& clock,
        DurationMode duration_mode,
        PathOrderMode path_order_mode,
        std::uint64_t path_order_seed,
        bool time_compaction);
    static MatchSummary countMatches(
        const TDTree& tree,
        std::size_t threads,
        RuntimeHookedOriginalNativePhaseObserver& phase_observer,
        const runtime_hooks_v1::MonotonicNowSource& clock,
        const runtime_hooks_v1::WorkerAffinityBinder& affinity_binder);
};

}  // namespace dsm::e3e4::adapter_v1::td_tree::test_support
#endif

#endif // TDTREE_H
