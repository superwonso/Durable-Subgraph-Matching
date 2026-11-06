#ifndef CANDIDATE_DAG_PASSES_H
#define CANDIDATE_DAG_PASSES_H

#include <string>

#include "CandidateDag.h"

// Accepted spellings: none/off or a comma-separated subset of p1,...,p6.
// Duplicates, unknown values and empty elements are errors.
DagPasses parseDagPasses(const std::string& value);
std::string dagPassesName(DagPasses passes);

// Same deterministic compile-time estimate contract is exposed to direct_opt;
// it is ordinary statistics, not a CandidateDag or DAG execution dependency.
struct DagCompileEstimates {
    std::array<double, kLabelCount> outgoing_by_label{};
    std::array<double, kLabelCount> incoming_by_label{};
    std::array<std::array<double, kLabelCount>, kLabelCount>
        history_by_endpoint_labels{};
    std::vector<double> td_block_by_query_vertex;
    std::vector<Label> query_labels;

    double cardinality(
        DagOp op, int first_query_vertex, int second_query_vertex = -1) const;
};

DagCompileEstimates computeDagCompileEstimates(
    const Graph& data_graph,
    const Graph& query_graph,
    const TDTree& td_tree);

// Exact deterministic leaf tie-break used by the compiler and direct_opt.
std::string dagOperandCanonicalName(
    DagOp op, int first_query_vertex, int second_query_vertex = -1);

// Pure compiler rewrite. An all-off invocation returns an unchanged P0 copy
// without reading graph statistics or changing any node/root/identity.
CandidateDag optimizeCandidateDag(
    const CandidateDag& base,
    const Graph& data_graph,
    const Graph& query_graph,
    const QueryPlan& plan,
    const TDTree& td_tree,
    DagPasses passes,
    double physical_alpha = 1.0);

// Writes prefix + ".dot", prefix + ".node_counts.csv", and
// prefix + ".nodes.csv". Existing outputs are rejected, never overwritten.
// stage_label identifies P0 / P0+P1 / P0+P1+P2 / P0+P1+P2+P3 (or a subset).
void dumpCandidateDag(
    const CandidateDag& dag,
    const QueryPlan& plan,
    const std::string& prefix,
    const std::string& stage_label);

#endif
