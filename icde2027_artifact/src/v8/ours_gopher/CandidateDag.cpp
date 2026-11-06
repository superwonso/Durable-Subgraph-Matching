#include "CandidateDag.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace {

std::uint64_t fnv1a(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::vector<int> mergeDependencies(
    const std::vector<int>& lhs,
    const std::vector<int>& rhs) {
    std::vector<int> result;
    result.reserve(lhs.size() + rhs.size());
    std::set_union(
        lhs.begin(), lhs.end(),
        rhs.begin(), rhs.end(),
        std::back_inserter(result));
    return result;
}

std::uint64_t queryFingerprint(const Graph& query) {
    std::ostringstream expression;
    expression << "QUERY_V1|V=" << query.num_vertices << "|L=";
    for (Label label : query.vertex_labels) {
        expression << static_cast<unsigned int>(label) << ',';
    }

    std::vector<std::pair<int, int>> arcs;
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const auto& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            arcs.emplace_back(source, edge.to);
        }
    }
    std::sort(arcs.begin(), arcs.end());
    arcs.erase(std::unique(arcs.begin(), arcs.end()), arcs.end());
    expression << "|E=" << arcs.size() << ':';
    for (const auto& arc : arcs) {
        expression << arc.first << '>' << arc.second << ',';
    }
    return fnv1a(expression.str());
}

std::uint64_t planFingerprint(
    std::uint64_t query_fingerprint,
    int root,
    const std::vector<int>& order,
    const std::vector<int>& parent) {
    std::ostringstream expression;
    expression << "PLAN_V2|Q=" << query_fingerprint
               << "|R=" << root
               << "|O=" << order.size() << ':';
    for (int vertex : order) expression << vertex << ',';
    expression << "|P=" << parent.size() << ':';
    for (int parent_vertex : parent) expression << parent_vertex << ',';
    return fnv1a(expression.str());
}

QueryPlan initializeQueryPlan(
    const QueryDecomposition& decomposition) {
    QueryPlan plan;
    plan.root = decomposition.root;
    plan.parent = decomposition.parent;
    plan.selectivity_mode = decomposition.selectivity_mode;
    if (decomposition.root >= 0 &&
        static_cast<std::size_t>(decomposition.root) <
            decomposition.temporal_selectivity.size()) {
        plan.root_selectivity_score =
            decomposition.temporal_selectivity[
                static_cast<std::size_t>(decomposition.root)];
    }
    return plan;
}

QueryPlan finalizeQueryPlan(
    const Graph& query,
    QueryPlan plan) {
    plan.earlier_neighbors.resize(
        static_cast<std::size_t>(query.num_vertices));
    std::vector<int> position(
        static_cast<std::size_t>(query.num_vertices), -1);
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        position[static_cast<std::size_t>(plan.order[i])] =
            static_cast<int>(i);
    }
    for (int query_vertex = 0;
         query_vertex < query.num_vertices;
         ++query_vertex) {
        auto& neighbors =
            plan.earlier_neighbors[
                static_cast<std::size_t>(query_vertex)];
        for (const auto& edge :
             query.adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex &&
                position[static_cast<std::size_t>(edge.to)] <
                    position[static_cast<std::size_t>(query_vertex)]) {
                neighbors.push_back(edge.to);
            }
        }
        for (const auto& edge :
             query.in_adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex &&
                position[static_cast<std::size_t>(edge.to)] <
                    position[static_cast<std::size_t>(query_vertex)]) {
                neighbors.push_back(edge.to);
            }
        }
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(
            std::unique(neighbors.begin(), neighbors.end()),
            neighbors.end());
    }
    plan.query_fingerprint = queryFingerprint(query);
    plan.plan_id = planFingerprint(
        plan.query_fingerprint,
        plan.root,
        plan.order,
        plan.parent);
    return plan;
}

} // namespace

const char* orderModeName(OrderMode mode) {
    return mode == OrderMode::ReuseAware ? "reuse" : "paper";
}

QueryPlan makeQueryPlan(
    const Graph& query,
    const QueryDecomposition& decomposition,
    OrderMode mode) {
    if (mode != OrderMode::Paper) {
        throw std::invalid_argument(
            "pre-TD query-plan construction supports paper order only");
    }
    QueryPlan plan = initializeQueryPlan(decomposition);
    plan.order = decomposition.dfs_order;
    return finalizeQueryPlan(query, std::move(plan));
}

QueryPlan makeQueryPlan(
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& td_tree,
    OrderMode mode) {
    if (mode == OrderMode::Paper) {
        return makeQueryPlan(query, decomposition, mode);
    }

    QueryPlan plan = initializeQueryPlan(decomposition);

    if (decomposition.root < 0 ||
        !decomposition.connected) {
        plan.order = decomposition.dfs_order;
    } else {
        const std::size_t vertex_count = static_cast<std::size_t>(query.num_vertices);
        std::vector<std::uint8_t> scheduled(vertex_count, 0);
        plan.order.reserve(vertex_count);
        plan.order.push_back(decomposition.root);
        scheduled[static_cast<std::size_t>(decomposition.root)] = 1;

        while (plan.order.size() < vertex_count) {
            int best = -1;
            double best_score = std::numeric_limits<double>::infinity();

            for (int candidate = 0; candidate < query.num_vertices; ++candidate) {
                if (scheduled[static_cast<std::size_t>(candidate)] != 0) continue;
                const int parent = decomposition.parent[static_cast<std::size_t>(candidate)];
                if (parent < 0 || scheduled[static_cast<std::size_t>(parent)] == 0) {
                    continue;
                }

                std::size_t earlier_arcs = 0;
                double reuse_potential = 0.0;
                for (const auto& edge : query.adj[static_cast<std::size_t>(candidate)]) {
                    if (edge.to == candidate ||
                        scheduled[static_cast<std::size_t>(edge.to)] == 0) {
                        continue;
                    }
                    ++earlier_arcs;
                    reuse_potential += static_cast<double>(
                        std::max<std::size_t>(
                            1,
                            query.in_adj[
                                static_cast<std::size_t>(
                                    edge.to)].size()));
                }
                for (const auto& edge : query.in_adj[static_cast<std::size_t>(candidate)]) {
                    if (edge.to == candidate ||
                        scheduled[static_cast<std::size_t>(edge.to)] == 0) {
                        continue;
                    }
                    ++earlier_arcs;
                    reuse_potential += static_cast<double>(
                        std::max<std::size_t>(
                            1,
                            query.adj[
                                static_cast<std::size_t>(
                                    edge.to)].size()));
                }
                if (GraphUtils::hasEdge(query.adj, candidate, candidate)) {
                    ++earlier_arcs;
                }

                const std::size_t blocks = std::max<std::size_t>(
                    1, td_tree.blockCount(candidate));
                const double average_candidates =
                    static_cast<double>(td_tree.relationEntryCount(candidate)) /
                    static_cast<double>(blocks);
                const double constraint_factor =
                    1.0 + 2.0 * static_cast<double>(earlier_arcs);
                const double reuse_factor = 1.0 + 0.25 * reuse_potential;
                const double score =
                    average_candidates / (constraint_factor * reuse_factor);

                if (best < 0 ||
                    score < best_score - 1e-12 ||
                    (std::abs(score - best_score) <= 1e-12 && candidate < best)) {
                    best = candidate;
                    best_score = score;
                }
            }

            if (best < 0) {
                // A malformed decomposition must not produce a partial plan.
                plan.order = decomposition.dfs_order;
                break;
            }
            scheduled[static_cast<std::size_t>(best)] = 1;
            plan.order.push_back(best);
        }
    }

    return finalizeQueryPlan(query, std::move(plan));
}

const char* dagOpName(DagOp op) {
    switch (op) {
        case DagOp::TdBlockLookup: return "TD_BLOCK";
        case DagOp::OutNeighborLookup: return "OUT_NEIGHBORS";
        case DagOp::InNeighborLookup: return "IN_NEIGHBORS";
        case DagOp::StructuralIntersection: return "STRUCT_INTERSECT";
        case DagOp::EdgeTimeLookup: return "EDGE_TIME";
        case DagOp::TemporalIntersection: return "TEMPORAL_INTERSECT";
        case DagOp::LongestConsecutiveRun: return "LONGEST_RUN";
        case DagOp::BranchSupportInput: return "BRANCH_SUPPORT_INPUT";
        case DagOp::KappaNormalize: return "KAPPA_NORMALIZE";
        case DagOp::GuardedEdgeTimeLookup: return "GUARDED_EDGE_TIME";
        case DagOp::TdBlockView: return "TD_BLOCK_VIEW";
        case DagOp::OutNeighborView: return "OUT_NEIGHBOR_VIEW";
        case DagOp::InNeighborView: return "IN_NEIGHBOR_VIEW";
        case DagOp::FusedEdgeProbe: return "FUSED_EDGE_PROBE";
        case DagOp::BoundEdgeTimeLookup: return "BOUND_EDGE_TIME";
        case DagOp::RuntimeStructuralChoice: return "RUNTIME_STRUCTURAL_CHOICE";
    }
    return "UNKNOWN";
}

const char* dagOpMetricName(DagOp op) {
    switch (op) {
        case DagOp::TdBlockLookup: return "td_block_lookup";
        case DagOp::OutNeighborLookup: return "out_neighbor_lookup";
        case DagOp::InNeighborLookup: return "in_neighbor_lookup";
        case DagOp::StructuralIntersection:
            return "structural_intersection";
        case DagOp::EdgeTimeLookup: return "edge_time_lookup";
        case DagOp::TemporalIntersection:
            return "temporal_intersection";
        case DagOp::LongestConsecutiveRun:
            return "longest_consecutive_run";
        case DagOp::BranchSupportInput:
            return "branch_support_input";
        case DagOp::KappaNormalize:
            return "kappa_normalize";
        case DagOp::GuardedEdgeTimeLookup:
            return "guarded_edge_time_lookup";
        case DagOp::TdBlockView: return "td_block_view";
        case DagOp::OutNeighborView: return "out_neighbor_view";
        case DagOp::InNeighborView: return "in_neighbor_view";
        case DagOp::FusedEdgeProbe: return "fused_edge_probe";
        case DagOp::BoundEdgeTimeLookup: return "bound_edge_time_lookup";
        case DagOp::RuntimeStructuralChoice: return "runtime_structural_choice";
    }
    return "unknown";
}

std::size_t dagOpIndex(DagOp op) {
    const std::size_t index = static_cast<std::size_t>(op);
    if (index >= kDagOpCount) {
        throw std::out_of_range("DAG operation index");
    }
    return index;
}

const char* dagResultKindName(DagResultKind kind) {
    switch (kind) {
        case DagResultKind::CandidateSet: return "candidate-set";
        case DagResultKind::IntervalSet: return "interval-set";
        case DagResultKind::Integer: return "integer";
        case DagResultKind::CandidateView: return "candidate-view";
        case DagResultKind::CandidateBatch: return "candidate-batch";
    }
    return "unknown";
}

const char* dagStructuralPolicyName(DagStructuralPolicy policy) {
    switch (policy) {
        case DagStructuralPolicy::LegacyMerge: return "legacy-merge";
        case DagStructuralPolicy::Probe: return "probe";
        case DagStructuralPolicy::StateCost: return "state-cost";
    }
    return "unknown";
}

const char* candidateDagInternModeName(CandidateDagInternMode mode) {
    return mode == CandidateDagInternMode::NoIntern
        ? "no-intern"
        : "hash-cons";
}

const DagNode& CandidateDag::node(DagNodeId id) const {
    if (id >= nodes.size()) throw std::out_of_range("DAG node ID");
    return nodes[id];
}

std::size_t CandidateDag::getMemoryUsage() const {
    std::size_t total = nodes.capacity() * sizeof(DagNode);
    total += structural_roots.capacity() * sizeof(DagNodeId);
    total += temporal_roots.capacity() * sizeof(DagNodeId);
    total += longest_run_roots.capacity() * sizeof(DagNodeId);
    for (const auto& dag_node : nodes) {
        total += dag_node.parents.capacity() * sizeof(DagNodeId);
        total += dag_node.dependencies.capacity() * sizeof(int);
        total += dag_node.history_slots.capacity() * sizeof(DagDirectedEdgeSlot);
        total += dag_node.canonical_expression.capacity();
    }
    return total;
}

CandidateDagTopology CandidateDag::topologySummary() const {
    CandidateDagTopology summary;
    summary.nodes_attempted = nodes_attempted;
    summary.nodes_materialized = nodes.size();
    summary.nodes_merged = nodes_merged;
    summary.nodes_eliminated_by_passes = nodes_eliminated_by_passes;
    summary.op_node_attempt_counts = op_node_attempt_counts;

    std::vector<std::uint64_t> fanout(nodes.size(), 0);
    for (const DagNode& dag_node : nodes) {
        ++summary.op_node_counts[dagOpIndex(dag_node.op)];
        summary.max_depth = std::max<std::uint64_t>(
            summary.max_depth, dag_node.depth);
        summary.total_parent_edges += dag_node.parents.size();
        for (DagNodeId parent : dag_node.parents) {
            node(parent);
            ++fanout[static_cast<std::size_t>(parent)];
        }
    }
    for (std::uint64_t consumers : fanout) {
        summary.max_fanout = std::max(summary.max_fanout, consumers);
        if (consumers >= 2) ++summary.shared_nodes;
    }

    std::vector<std::uint8_t> reachable(nodes.size(), 0);
    std::vector<DagNodeId> pending;
    auto add_roots = [&](const std::vector<DagNodeId>& roots) {
        for (DagNodeId root : roots) {
            if (root != kInvalidDagNode) pending.push_back(root);
        }
    };
    add_roots(structural_roots);
    add_roots(temporal_roots);
    add_roots(longest_run_roots);
    while (!pending.empty()) {
        const DagNodeId current = pending.back();
        pending.pop_back();
        node(current);
        if (reachable[static_cast<std::size_t>(current)] != 0) continue;
        reachable[static_cast<std::size_t>(current)] = 1;
        for (DagNodeId parent : node(current).parents) {
            pending.push_back(parent);
        }
    }

    std::vector<std::uint64_t> reachable_fanout(nodes.size(), 0);
    for (const DagNode& dag_node : nodes) {
        if (reachable[static_cast<std::size_t>(dag_node.id)] == 0) continue;
        ++summary.reachable_nodes;
        ++summary.reachable_op_node_counts[dagOpIndex(dag_node.op)];
        summary.reachable_max_depth = std::max<std::uint64_t>(
            summary.reachable_max_depth, dag_node.depth);
        summary.reachable_parent_edges += dag_node.parents.size();
        for (DagNodeId parent : dag_node.parents) {
            node(parent);
            ++reachable_fanout[static_cast<std::size_t>(parent)];
        }
    }
    for (std::uint64_t consumers : reachable_fanout) {
        summary.reachable_max_fanout = std::max(
            summary.reachable_max_fanout, consumers);
        if (consumers >= 2) ++summary.reachable_shared_nodes;
    }
    summary.unreachable_materialized_nodes =
        summary.nodes_materialized - summary.reachable_nodes;
    return summary;
}

CandidateDagCompiler::CandidateDagCompiler()
    : fingerprint_(nullptr) {}

CandidateDagCompiler::CandidateDagCompiler(
    FingerprintFunction fingerprint_for_test)
    : fingerprint_(std::move(fingerprint_for_test)) {
    if (!fingerprint_) {
        throw std::invalid_argument(
            "Candidate-DAG fingerprint function must be callable");
    }
}

CandidateDag CandidateDagCompiler::compile(
    const Graph& query,
    const QueryPlan& plan,
    const QueryDecomposition&,
    CandidateDagInternMode intern_mode,
    CandidateDagCompilationAudit* audit) {
    CandidateDag dag;
    const std::size_t vertex_count = static_cast<std::size_t>(query.num_vertices);
    dag.structural_roots.assign(vertex_count, kInvalidDagNode);
    dag.temporal_roots.assign(vertex_count, kInvalidDagNode);
    dag.longest_run_roots.assign(vertex_count, kInvalidDagNode);

    std::unordered_map<std::uint64_t, std::vector<DagNodeId>> expression_index;

    if (audit != nullptr) {
        *audit = CandidateDagCompilationAudit{};
    }

    auto normalize_commutative_parents = [
        &dag,
        audit](std::vector<DagNodeId>& parents) {
        std::sort(
            parents.begin(),
            parents.end(),
            [&](DagNodeId lhs, DagNodeId rhs) {
                const std::string& lhs_expression =
                    dag.node(lhs).canonical_expression;
                const std::string& rhs_expression =
                    dag.node(rhs).canonical_expression;
                if (lhs_expression != rhs_expression) {
                    return lhs_expression < rhs_expression;
                }
                return lhs < rhs;
            });
        const auto new_end = std::unique(
            parents.begin(),
            parents.end(),
            [&](DagNodeId lhs, DagNodeId rhs) {
                const bool equivalent =
                    dag.node(lhs).canonical_expression ==
                    dag.node(rhs).canonical_expression;
                if (equivalent && audit != nullptr) {
                    audit->canonical_parent_deduplicated_node_ids
                        .push_back(rhs);
                }
                return equivalent;
            });
        parents.erase(new_end, parents.end());
    };

    auto intern = [&](DagOp op,
                      DagResultKind result_kind,
                      std::vector<DagNodeId> parents,
                      std::vector<int> dependencies,
                      DagNodeParameter parameter,
                      const std::string& leaf_expression) -> DagNodeId {
        ++dag.nodes_attempted;
        ++dag.op_node_attempt_counts[dagOpIndex(op)];
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(
            std::unique(dependencies.begin(), dependencies.end()),
            dependencies.end());

        if (op == DagOp::StructuralIntersection ||
            op == DagOp::TemporalIntersection) {
            normalize_commutative_parents(parents);
        }

        std::ostringstream expression;
        expression << dagOpName(op) << '(';
        if (!leaf_expression.empty()) {
            expression << leaf_expression;
        } else {
            for (std::size_t i = 0; i < parents.size(); ++i) {
                if (i > 0) expression << ',';
                expression << dag.node(parents[i]).canonical_expression;
            }
        }
        expression << ')';
        const std::string canonical = expression.str();
        const std::uint64_t fingerprint = fingerprint_ == nullptr
            ? fnv1a(canonical)
            : fingerprint_(canonical);

        if (audit != nullptr) {
            CandidateDagAttemptRecord record;
            record.attempt_index = dag.nodes_attempted - 1;
            record.op = op;
            record.result_kind = result_kind;
            record.dependencies = dependencies;
            record.parameter = parameter;
            record.canonical_expression = canonical;
            record.static_fingerprint = fingerprint;
            record.parent_expressions.reserve(parents.size());
            for (DagNodeId parent : parents) {
                record.parent_expressions.push_back(
                    dag.node(parent).canonical_expression);
            }
            audit->attempts.push_back(std::move(record));
        }

        if (intern_mode == CandidateDagInternMode::Intern) {
            auto found_bucket = expression_index.find(fingerprint);
            if (found_bucket != expression_index.end()) {
                for (DagNodeId existing : found_bucket->second) {
                    if (dag.node(existing).canonical_expression == canonical) {
                        ++dag.nodes_merged;
                        return existing;
                    }
                }
            }
        }

        DagNode node;
        node.id = static_cast<DagNodeId>(dag.nodes.size());
        node.op = op;
        node.result_kind = result_kind;
        node.parents = std::move(parents);
        node.dependencies = std::move(dependencies);
        node.parameter = parameter;
        node.canonical_expression = canonical;
        node.static_fingerprint = fingerprint;
        node.depth = 1;
        for (DagNodeId parent : node.parents) {
            node.depth = std::max(node.depth, dag.node(parent).depth + 1);
        }
        dag.nodes.push_back(std::move(node));
        if (intern_mode == CandidateDagInternMode::Intern) {
            expression_index[fingerprint].push_back(dag.nodes.back().id);
        }
        return dag.nodes.back().id;
    };

    auto intersect_nodes = [&](DagNodeId lhs,
                               DagNodeId rhs,
                               DagOp op,
                               DagResultKind kind) {
        std::vector<DagNodeId> parents;
        auto append_operand = [&](DagNodeId operand) {
            const DagNode& operand_node = dag.node(operand);
            if (operand_node.op == op &&
                operand_node.result_kind == kind) {
                parents.insert(
                    parents.end(),
                    operand_node.parents.begin(),
                    operand_node.parents.end());
            } else {
                parents.push_back(operand);
            }
        };
        append_operand(lhs);
        append_operand(rhs);
        normalize_commutative_parents(parents);

        const auto dependencies = mergeDependencies(
            dag.node(lhs).dependencies,
            dag.node(rhs).dependencies);
        return intern(op, kind, std::move(parents), dependencies, {}, "");
    };

    std::vector<int> position(vertex_count, -1);
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        position[static_cast<std::size_t>(plan.order[i])] = static_cast<int>(i);
    }

    for (std::size_t order_index = 0; order_index < plan.order.size(); ++order_index) {
        const int u = plan.order[order_index];
        if (u < 0 || u >= query.num_vertices) {
            throw std::runtime_error("invalid query plan vertex");
        }

        if (order_index > 0) {
            const int parent = plan.parent[static_cast<std::size_t>(u)];
            DagNodeId structural = intern(
                DagOp::TdBlockLookup,
                DagResultKind::CandidateSet,
                {},
                {parent},
                {u, parent},
                "q" + std::to_string(u) + ",p" + std::to_string(parent));

            for (const auto& edge : query.adj[static_cast<std::size_t>(u)]) {
                const int x = edge.to;
                if (x == u || position[static_cast<std::size_t>(x)] >=
                                  position[static_cast<std::size_t>(u)]) {
                    continue;
                }
                const DagNodeId neighbors = intern(
                    DagOp::InNeighborLookup,
                    DagResultKind::CandidateSet,
                    {},
                    {x},
                    {x, -1},
                    "q" + std::to_string(x));
                structural = intersect_nodes(
                    structural,
                    neighbors,
                    DagOp::StructuralIntersection,
                    DagResultKind::CandidateSet);
            }
            for (const auto& edge : query.in_adj[static_cast<std::size_t>(u)]) {
                const int x = edge.to;
                if (x == u || position[static_cast<std::size_t>(x)] >=
                                  position[static_cast<std::size_t>(u)]) {
                    continue;
                }
                const DagNodeId neighbors = intern(
                    DagOp::OutNeighborLookup,
                    DagResultKind::CandidateSet,
                    {},
                    {x},
                    {x, -1},
                    "q" + std::to_string(x));
                structural = intersect_nodes(
                    structural,
                    neighbors,
                    DagOp::StructuralIntersection,
                    DagResultKind::CandidateSet);
            }
            dag.structural_roots[static_cast<std::size_t>(u)] = structural;
        }

        std::vector<DagNodeId> temporal_edges;
        if (GraphUtils::hasEdge(query.adj, u, u)) {
            temporal_edges.push_back(intern(
                DagOp::EdgeTimeLookup,
                DagResultKind::IntervalSet,
                {},
                {u},
                {u, u},
                "q" + std::to_string(u) + "->q" + std::to_string(u)));
        }
        for (const auto& edge : query.adj[static_cast<std::size_t>(u)]) {
            const int x = edge.to;
            if (x == u || position[static_cast<std::size_t>(x)] >=
                              position[static_cast<std::size_t>(u)]) {
                continue;
            }
            temporal_edges.push_back(intern(
                DagOp::EdgeTimeLookup,
                DagResultKind::IntervalSet,
                {},
                {u, x},
                {u, x},
                "q" + std::to_string(u) + "->q" + std::to_string(x)));
        }
        for (const auto& edge : query.in_adj[static_cast<std::size_t>(u)]) {
            const int x = edge.to;
            if (x == u || position[static_cast<std::size_t>(x)] >=
                              position[static_cast<std::size_t>(u)]) {
                continue;
            }
            temporal_edges.push_back(intern(
                DagOp::EdgeTimeLookup,
                DagResultKind::IntervalSet,
                {},
                {x, u},
                {x, u},
                "q" + std::to_string(x) + "->q" + std::to_string(u)));
        }

        if (!temporal_edges.empty()) {
            DagNodeId temporal = temporal_edges.front();
            for (std::size_t i = 1; i < temporal_edges.size(); ++i) {
                temporal = intersect_nodes(
                    temporal,
                    temporal_edges[i],
                    DagOp::TemporalIntersection,
                    DagResultKind::IntervalSet);
            }
            dag.temporal_roots[static_cast<std::size_t>(u)] = temporal;
            dag.longest_run_roots[static_cast<std::size_t>(u)] = intern(
                DagOp::LongestConsecutiveRun,
                DagResultKind::Integer,
                {temporal},
                dag.node(temporal).dependencies,
                {u, -1},
                "");
        }
    }

    if (audit != nullptr) {
        auto root_expressions = [&](
                                    const std::vector<DagNodeId>& roots,
                                    std::vector<std::string>& output) {
            output.clear();
            output.reserve(roots.size());
            for (DagNodeId root : roots) {
                output.push_back(
                    root == kInvalidDagNode
                    ? std::string{}
                    : dag.node(root).canonical_expression);
            }
        };
        root_expressions(
            dag.structural_roots,
            audit->structural_root_expressions);
        root_expressions(
            dag.temporal_roots,
            audit->temporal_root_expressions);
        root_expressions(
            dag.longest_run_roots,
            audit->longest_run_root_expressions);
        std::sort(
            audit->canonical_parent_deduplicated_node_ids.begin(),
            audit->canonical_parent_deduplicated_node_ids.end());
        audit->canonical_parent_deduplicated_node_ids.erase(
            std::unique(
                audit->canonical_parent_deduplicated_node_ids.begin(),
                audit->canonical_parent_deduplicated_node_ids.end()),
            audit->canonical_parent_deduplicated_node_ids.end());
    }

    return dag;
}
