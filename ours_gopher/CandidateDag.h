#ifndef CANDIDATE_DAG_H
#define CANDIDATE_DAG_H

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
    std::uint64_t plan_id = 0;
};

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
    LongestConsecutiveRun
};

enum class DagResultKind : std::uint8_t {
    CandidateSet,
    IntervalSet,
    Integer
};

const char* dagOpName(DagOp op);

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
};

struct CandidateDag {
    std::vector<DagNode> nodes;
    std::vector<DagNodeId> structural_roots;
    std::vector<DagNodeId> temporal_roots;
    std::vector<DagNodeId> longest_run_roots;
    std::uint64_t nodes_attempted = 0;
    std::uint64_t nodes_merged = 0;

    const DagNode& node(DagNodeId id) const;
    std::size_t getMemoryUsage() const;
};

class CandidateDagCompiler {
public:
    CandidateDag compile(
        const Graph& query,
        const QueryPlan& plan,
        const QueryDecomposition& decomposition);
};

#endif
