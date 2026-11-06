#ifndef CANDIDATE_DAG_H
#define CANDIDATE_DAG_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "TDTree.h"
#include "Utils.h"
#include "query_decomposition.h"

using DagNodeId = std::uint32_t;
constexpr DagNodeId kInvalidDagNode = static_cast<DagNodeId>(-1);

enum class OrderMode {
    Paper,
    ReuseAware
};

const char* orderModeName(OrderMode mode);

struct QueryPlan {
    int root = -1;
    std::vector<int> order;
    std::vector<int> parent;
    std::vector<std::vector<int>> earlier_neighbors;
    SelectivityMode selectivity_mode = SelectivityMode::Paper;
    double root_selectivity_score = 0.0;
    std::uint64_t query_fingerprint = 0;
    std::uint64_t plan_id = 0;
};

// Paper ordering is completely determined by the live query and its freshly
// computed decomposition.  It does not inspect a TD-tree.  This overload is
// the pre-TD hook used by the E3/E4 typed-plan authority path; non-paper modes
// are rejected because their scoring genuinely depends on TD-tree relations.
QueryPlan makeQueryPlan(
    const Graph& query,
    const QueryDecomposition& decomposition,
    OrderMode mode);

// Legacy/product API retained for source compatibility.  Paper calls delegate
// to the pre-TD overload above; reuse-aware calls keep their TD-dependent
// behavior.
QueryPlan makeQueryPlan(
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& td_tree,
    OrderMode mode);

enum class DagOp : std::uint8_t {
    TdBlockLookup,
    OutNeighborLookup,
    InNeighborLookup,
    StructuralIntersection,
    EdgeTimeLookup,
    TemporalIntersection,
    LongestConsecutiveRun,
    // Operator-opt extension; retain numeric identities of all seven P0 ops.
    BranchSupportInput,
    KappaNormalize,
    GuardedEdgeTimeLookup,
    // Adaptive study: append only; identities 0..9 remain byte-compatible.
    TdBlockView,
    OutNeighborView,
    InNeighborView,
    FusedEdgeProbe,
    BoundEdgeTimeLookup,
    RuntimeStructuralChoice
};

enum class DagResultKind : std::uint8_t {
    CandidateSet,
    IntervalSet,
    Integer,
    CandidateView,
    CandidateBatch
};

constexpr std::size_t kLegacyDagOpCount = 10;
constexpr std::size_t kDagOpCount = 16;

struct DagPasses {
    bool p1 = false;
    bool p2 = false;
    bool p3 = false;
    bool p4 = false;
    bool p5 = false;
    bool p6 = false;

    bool temporalRewriteAny() const noexcept { return p1 || p2 || p3; }
    bool physicalAny() const noexcept { return p4 || p5 || p6; }
    bool any() const noexcept { return temporalRewriteAny() || physicalAny(); }
};

// P5 compares complete state costs, not the old P3 pair-ratio threshold.
// The fixed policy is c_probe <= alpha*c_merge with alpha in {0.5,1,2}.
enum class DagStructuralPolicy : std::uint8_t {
    LegacyMerge,
    Probe,
    StateCost
};

const char* dagStructuralPolicyName(DagStructuralPolicy policy);

struct DagDirectedEdgeSlot {
    int from_query_vertex = -1;
    int to_query_vertex = -1;

    bool operator==(const DagDirectedEdgeSlot& other) const noexcept {
        return from_query_vertex == other.from_query_vertex &&
               to_query_vertex == other.to_query_vertex;
    }
    bool operator<(const DagDirectedEdgeSlot& other) const noexcept {
        return from_query_vertex < other.from_query_vertex ||
               (from_query_vertex == other.from_query_vertex &&
                to_query_vertex < other.to_query_vertex);
    }
};

constexpr std::uint32_t kInvalidDagHistorySlot = static_cast<std::uint32_t>(-1);

// Fixed before timing; used by both DAG and direct optimized kernels.
inline constexpr std::size_t kDagAdaptiveIntersectionRatio = 32;

const char* dagOpName(DagOp op);
const char* dagOpMetricName(DagOp op);
std::size_t dagOpIndex(DagOp op);

const char* dagResultKindName(DagResultKind kind);

// This is deliberately a compiler-only, closed experimental factor.  It does
// not alter expression construction or normalization; NoIntern merely skips
// the collision-checked expression-index lookup and insertion.
enum class CandidateDagInternMode : std::uint8_t {
    Intern,
    NoIntern
};

const char* candidateDagInternModeName(CandidateDagInternMode mode);

struct DagNodeParameter {
    int first_query_vertex = -1;
    int second_query_vertex = -1;
};

struct DagNode {
    DagNodeId id = kInvalidDagNode;
    DagOp op = DagOp::TdBlockLookup;
    DagResultKind result_kind = DagResultKind::CandidateSet;
    std::vector<DagNodeId> parents;
    std::vector<int> dependencies;
    DagNodeParameter parameter;
    std::string canonical_expression;
    std::uint64_t static_fingerprint = 0;
    std::uint32_t depth = 0;
    // Compiler-selected semantics; default values preserve P0's operator work.
    // raw_intervals disables implicit k-normalization on time load/intersect.
    bool raw_intervals = false;
    bool short_circuit = false;
    bool adaptive_intersection = false;
    double estimated_cardinality = 0.0;
    // Physical producers have seed parent 0 and fixed-anchor adjacency parents
    // thereafter. P4 owns directed slots in lexicographic query-ID order;
    // runtime candidate index selects a frame-owned row of immutable edge IDs.
    DagStructuralPolicy structural_policy = DagStructuralPolicy::LegacyMerge;
    double physical_alpha = 1.0;
    std::vector<DagDirectedEdgeSlot> history_slots;
    // This is an already-materialized producer input, NOT permission to invoke
    // the producer again during time evaluation or skipped-subtree accounting.
    DagNodeId bound_producer = kInvalidDagNode;
    std::uint32_t bound_slot = kInvalidDagHistorySlot;
    bool lmax_proven = false;
};

struct CandidateDagTopology {
    std::uint64_t nodes_attempted = 0;
    std::uint64_t nodes_materialized = 0;
    std::uint64_t nodes_merged = 0;
    std::uint64_t nodes_eliminated_by_passes = 0;
    std::uint64_t total_parent_edges = 0;
    std::uint64_t shared_nodes = 0;
    std::uint64_t max_fanout = 0;
    std::uint64_t max_depth = 0;
    std::uint64_t reachable_nodes = 0;
    std::uint64_t unreachable_materialized_nodes = 0;
    std::uint64_t reachable_parent_edges = 0;
    std::uint64_t reachable_shared_nodes = 0;
    std::uint64_t reachable_max_fanout = 0;
    std::uint64_t reachable_max_depth = 0;
    std::array<std::uint64_t, kDagOpCount> op_node_attempt_counts{};
    std::array<std::uint64_t, kDagOpCount> op_node_counts{};
    std::array<std::uint64_t, kDagOpCount> reachable_op_node_counts{};
};

struct CandidateDagAttemptRecord {
    std::uint64_t attempt_index = 0;
    DagOp op = DagOp::TdBlockLookup;
    DagResultKind result_kind = DagResultKind::CandidateSet;
    std::vector<std::string> parent_expressions;
    std::vector<int> dependencies;
    DagNodeParameter parameter;
    std::string canonical_expression;
    std::uint64_t static_fingerprint = 0;
};

// Populated only for untimed correctness preflights.  Primary compilation
// passes a null pointer and therefore retains no attempt stream or discarded
// parent IDs.
struct CandidateDagCompilationAudit {
    std::vector<CandidateDagAttemptRecord> attempts;
    std::vector<DagNodeId> canonical_parent_deduplicated_node_ids;
    std::vector<std::string> structural_root_expressions;
    std::vector<std::string> temporal_root_expressions;
    std::vector<std::string> longest_run_root_expressions;
};

struct CandidateDag {
    std::vector<DagNode> nodes;
    std::vector<DagNodeId> structural_roots;
    std::vector<DagNodeId> temporal_roots;
    std::vector<DagNodeId> longest_run_roots;
    std::uint64_t nodes_attempted = 0;
    std::uint64_t nodes_merged = 0;
    std::uint64_t nodes_eliminated_by_passes = 0;
    std::array<std::uint64_t, kDagOpCount> op_node_attempt_counts{};
    DagPasses passes;
    // P1 temporal roots return the next branch support J', not local S-tilde.
    bool cumulative_support = false;

    const DagNode& node(DagNodeId id) const;
    std::size_t getMemoryUsage() const;
    CandidateDagTopology topologySummary() const;
};

class CandidateDagCompiler {
public:
    using FingerprintFunction =
        std::uint64_t (*)(const std::string&);

    CandidateDagCompiler();

    // Test-only injection point used to prove that full canonical-expression
    // equality, rather than a 64-bit hash alone, governs interning.  Product
    // CLI construction always uses the default constructor.
    explicit CandidateDagCompiler(FingerprintFunction fingerprint_for_test);

    CandidateDag compile(
        const Graph& query,
        const QueryPlan& plan,
        const QueryDecomposition& decomposition,
        CandidateDagInternMode intern_mode =
            CandidateDagInternMode::Intern,
        CandidateDagCompilationAudit* audit = nullptr);

private:
    FingerprintFunction fingerprint_;
};

#endif
