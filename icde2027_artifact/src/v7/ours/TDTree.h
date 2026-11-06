#ifndef TDTREE_H
#define TDTREE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <string>
#include <vector>

#include "Utils.h"
#include "fixed_plan_feasibility.h"
#include "query_decomposition.h"

struct CoverAwareBuildResult;
enum class CoverAwareBuildFailurePoint;
class ValidatedGraphSnapshot;

struct TDTreeBlock {
    int v_par = -1;
    std::vector<int> V_cand;
};

struct TDTreeNode {
    int query_vertex_id = -1;
    bool isRoot = false;
    bool isLeaf = false;
    std::vector<int> root_candidates;
    std::vector<TDTreeBlock> blocks;

    const TDTreeBlock* findBlock(int parent_vertex) const;
    void rebuildBlockIndex();
};

// Exact native owning-capacity accounting for one retained TDTree.  Inline
// owner handles are included through sizeof(TDTree); the shared snapshot
// pointee/control block and graph storage are deliberately not counted here.
// Every dynamic component is derived from an actual std::vector::capacity().
struct TDTreeCapacityReceipt {
    bool complete = false;
    bool overflowed = false;

    std::uint64_t object_bytes = 0;
    std::uint64_t node_count = 0;
    std::uint64_t node_capacity = 0;
    std::uint64_t nodes_bytes = 0;
    std::uint64_t root_candidate_count = 0;
    std::uint64_t root_candidate_capacity = 0;
    std::uint64_t root_candidates_bytes = 0;
    std::uint64_t block_count = 0;
    std::uint64_t block_capacity = 0;
    std::uint64_t blocks_bytes = 0;
    std::uint64_t block_candidate_count = 0;
    std::uint64_t block_candidate_capacity = 0;
    std::uint64_t block_candidates_bytes = 0;

    std::uint64_t query_out_requirements_count = 0;
    std::uint64_t query_out_requirements_capacity = 0;
    std::uint64_t query_out_requirements_bytes = 0;
    std::uint64_t query_in_requirements_count = 0;
    std::uint64_t query_in_requirements_capacity = 0;
    std::uint64_t query_in_requirements_bytes = 0;

    std::uint64_t decomposition_spanning_outer_count = 0;
    std::uint64_t decomposition_spanning_outer_capacity = 0;
    std::uint64_t decomposition_spanning_outer_bytes = 0;
    std::uint64_t decomposition_spanning_nested_count = 0;
    std::uint64_t decomposition_spanning_nested_capacity = 0;
    std::uint64_t decomposition_spanning_nested_bytes = 0;
    std::uint64_t decomposition_non_tree_edges_count = 0;
    std::uint64_t decomposition_non_tree_edges_capacity = 0;
    std::uint64_t decomposition_non_tree_edges_bytes = 0;
    std::uint64_t decomposition_vertex_labels_count = 0;
    std::uint64_t decomposition_vertex_labels_capacity = 0;
    std::uint64_t decomposition_vertex_labels_bytes = 0;
    std::uint64_t decomposition_parent_count = 0;
    std::uint64_t decomposition_parent_capacity = 0;
    std::uint64_t decomposition_parent_bytes = 0;
    std::uint64_t decomposition_dfs_order_count = 0;
    std::uint64_t decomposition_dfs_order_capacity = 0;
    std::uint64_t decomposition_dfs_order_bytes = 0;
    std::uint64_t decomposition_temporal_selectivity_count = 0;
    std::uint64_t decomposition_temporal_selectivity_capacity = 0;
    std::uint64_t decomposition_temporal_selectivity_bytes = 0;
    std::uint64_t decomposition_structural_candidate_counts_count = 0;
    std::uint64_t decomposition_structural_candidate_counts_capacity = 0;
    std::uint64_t decomposition_structural_candidate_counts_bytes = 0;
    std::uint64_t decomposition_compatible_edge_counts_count = 0;
    std::uint64_t decomposition_compatible_edge_counts_capacity = 0;
    std::uint64_t decomposition_compatible_edge_counts_bytes = 0;
    std::uint64_t decomposition_durable_compatible_edge_counts_count = 0;
    std::uint64_t decomposition_durable_compatible_edge_counts_capacity = 0;
    std::uint64_t decomposition_durable_compatible_edge_counts_bytes = 0;
    std::uint64_t decomposition_durability_survival_rates_count = 0;
    std::uint64_t decomposition_durability_survival_rates_capacity = 0;
    std::uint64_t decomposition_durability_survival_rates_bytes = 0;
    std::uint64_t query_decomposition_bytes = 0;

    std::uint64_t dynamic_bytes = 0;
    std::uint64_t total_bytes = 0;

    bool invariantHolds() const noexcept;
};

#ifdef DSM_TCC_V3_TESTING
// Exercises the same checked product-plus-add primitive with impossible
// synthetic values; real vector capacities cannot safely approach uint64 max.
bool checkedTDTreeCapacityAddProductForTesting(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result) noexcept;
#endif

struct MatchSummary {
    std::uint64_t match_count = 0;
    long long enumeration_milliseconds = 0;
    bool output_written = false;
    bool count_overflow = false;
    // Optional, order-independent exactness witness over complete mappings.
    // It uses the same mapping hash and xor/sum reduction as the
    // Candidate-DAG CPU/GPU executors. Legacy calls leave it disabled so
    // historical timing boundaries do not acquire extra per-answer work.
    bool result_digest_collected = false;
    std::uint64_t mapping_digest_xor = 0;
    std::uint64_t mapping_digest_sum = 0;
    std::size_t threads_requested = 1;
    // Number of executor workers whose std::thread construction completed.
    // The closed V3 path requires this to equal the requested pool size;
    // it is distinct from workers_with_tasks for small root domains.
    std::size_t threads_created = 0;
    std::size_t threads_used = 1;
    std::size_t workers_with_tasks = 0;
    std::size_t root_tasks_claimed = 0;
    std::size_t worker_root_tasks_min = 0;
    std::size_t worker_root_tasks_max = 0;
    std::size_t root_candidate_count = 0;
    std::uint64_t dfs_states = 0;
    std::uint64_t generated_candidates = 0;
    std::uint64_t accepted_candidates = 0;
    std::uint64_t completed_matches = 0;
    std::uint64_t pruned_injectivity = 0;
    std::uint64_t pruned_temporal_support = 0;
    std::vector<std::uint64_t> generated_candidates_by_depth;
    std::vector<std::uint64_t> accepted_candidates_by_depth;
    std::vector<std::uint64_t> pruned_injectivity_by_depth;
    std::vector<std::uint64_t> pruned_temporal_support_by_depth;
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

class TDTree {
public:
    static std::unique_ptr<TDTree> buildValidatedIncumbent(
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
        const QueryDecomposition& decomposition,
        int minimum_duration);

    TDTree(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration);
    void print_res() const;
    MatchSummary save_res(
        const std::string& filename,
        MatchOutputMode mode = MatchOutputMode::Full,
        std::size_t threads = 1,
        bool collect_result_digest = false) const;
    // Closed V3 exact executor: no result/report file is opened.  It always
    // collects the order-independent mapping digest and leaves timing to the
    // caller's nanosecond boundary chain.
    MatchSummary countMatchesV3(std::size_t threads) const;
    TDTreeCapacityReceipt capacityReceipt() const noexcept;
    std::size_t getMemoryUsage() const;
    std::size_t candidateRelationCount() const;
    const std::vector<int>& rootCandidates() const;
    const std::vector<int>* candidatesForParent(
        int query_vertex,
        int parent_data_vertex) const;
    std::size_t relationEntryCount(int query_vertex) const;
    std::size_t blockCount(int query_vertex) const;
    std::size_t queryCandidateCount(int query_vertex) const;
    std::uint64_t executionPlanFingerprint() const;
    // Stable content witness for the executable TD relations. It binds the
    // plan, k, sorted root domain, and every sorted parent-to-child block in
    // canonical query-vertex order, using external data-vertex identifiers.
    // A protocol receipt must bind this 64-bit witness inside its canonical
    // SHA-256 envelope; this value alone is not authorization.
    std::uint64_t candidateContentFingerprint() const noexcept;
    int minimumDuration() const noexcept;
    bool hasOnlySortedFlatBlocks() const;
    bool hasValidatedSnapshotAuthority() const noexcept;
    std::uint64_t validationSnapshotToken() const noexcept;
    bool isBoundToValidatedSnapshot(
        const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
        const noexcept;
    // True only when this validated incumbent was constructed from a
    // canonical Paper/Intended decomposition whose root, order, parent map,
    // and scores were independently recomputed from the sealed snapshot.
    // V3 uses this origin attestation so a merely topology-valid TD-tree plan
    // cannot masquerade as the corrected S_dur incumbent.
    bool hasCorrectedPaperIncumbentOrigin(
        const FixedPlan& incumbent_plan) const noexcept;
    CandidateCover exportCandidateCover() const;
    // Refresh reporting-only selectivity metadata after a planner handoff.
    // The executable root/order/parent must remain byte-identical.
    void setDecompositionMetadata(
        const QueryDecomposition& decomposition);

private:
    friend class CoverAwareTDTreeBuilder;
    friend CoverAwareBuildResult buildCoverAwareTDTree(
        const Graph&,
        const Graph&,
        const CandidateCover&,
        const FixedPlan&,
        std::unique_ptr<CandidateBuildReservation>);
    friend CoverAwareBuildResult buildCoverAwareTDTree(
        const std::shared_ptr<const ValidatedGraphSnapshot>&,
        const CandidateCover&,
        const FixedPlan&,
        std::unique_ptr<CandidateBuildReservation>);
    friend CoverAwareBuildResult buildCoverAwareTDTreeForTesting(
        const Graph&,
        const Graph&,
        const CandidateCover&,
        const FixedPlan&,
        std::unique_ptr<CandidateBuildReservation>,
        CoverAwareBuildFailurePoint);
    struct CoverAwareConstructionTag {};

    TDTree(
        const Graph& temporal_graph,
        const Graph& query_graph,
        QueryDecomposition decomposition,
        int minimum_duration,
        CoverAwareConstructionTag);
    TDTree(
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
        QueryDecomposition decomposition,
        int minimum_duration,
        bool sealed_flat_candidate);

    // Declared before G/Q: those references always bind into this retained
    // owner on the V3 path and therefore survive source/scope destruction.
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_owner_;
    const Graph& G;
    const Graph& Q;
    // Own the decomposition so planner-built trees remain valid when handed
    // off after the planner's local decomposition object is destroyed.
    QueryDecomposition QD;
    int k_threshold;
    bool sealed_flat_candidate_ = false;
    bool corrected_paper_incumbent_origin_ = false;
    std::uint64_t corrected_paper_incumbent_plan_fingerprint_ = 0;

    std::vector<TDTreeNode> nodes;
    std::vector<std::array<int, kLabelCount>> query_out_neighbor_label_requirements;
    std::vector<std::array<int, kLabelCount>> query_in_neighbor_label_requirements;

    void build();
    void initializeNodes();
    void initializeQueryRequirements();
    void fillRoot();
    void buildCandidateRelations();
    void trimBottomUp();
    void trimTopDown();
    void rebuildBlockIndexes();

    bool isDataVertexCandidate(int data_vertex, int query_vertex) const;
    bool passesAvailableNonTreeConstraints(
        int data_vertex,
        int query_vertex,
        const std::vector<int>& order_position,
        const std::vector<std::vector<std::uint8_t>>& candidate_flags) const;

    std::vector<int> uniqueCandidates(const TDTreeNode& node) const;
    std::size_t uniqueCandidateCount(const TDTreeNode& node) const;
    std::uint64_t flatPayloadCapacityBytes() const;
    CheckedMatchCounter enumerateMatches(
        std::ostream& output,
        MatchOutputMode mode,
        std::size_t requested_threads,
        bool collect_result_digest,
        MatchSummary& summary) const;
};

#endif // TDTREE_H
