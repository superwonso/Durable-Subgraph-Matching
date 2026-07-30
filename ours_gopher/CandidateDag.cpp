#include "CandidateDag.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

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

std::uint64_t planFingerprint(const std::vector<int>& order) {
    std::ostringstream expression;
    expression << "PLAN";
    for (int vertex : order) expression << ':' << vertex;
    return fnv1a(expression.str());
}

} // namespace

const char* orderModeName(OrderMode mode) {
    return mode == OrderMode::ReuseAware ? "reuse" : "paper";
}

QueryPlan makeQueryPlan(
    const Graph& query,
    const QueryDecomposition& decomposition,
    const TDTree& td_tree,
    OrderMode mode) {
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

    if (mode == OrderMode::Paper ||
        decomposition.root < 0 ||
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

    plan.earlier_neighbors.resize(static_cast<std::size_t>(query.num_vertices));
    std::vector<int> position(static_cast<std::size_t>(query.num_vertices), -1);
    for (std::size_t i = 0; i < plan.order.size(); ++i) {
        position[static_cast<std::size_t>(plan.order[i])] = static_cast<int>(i);
    }
    for (int query_vertex = 0; query_vertex < query.num_vertices; ++query_vertex) {
        auto& neighbors = plan.earlier_neighbors[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : query.adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex &&
                position[static_cast<std::size_t>(edge.to)] <
                    position[static_cast<std::size_t>(query_vertex)]) {
                neighbors.push_back(edge.to);
            }
        }
        for (const auto& edge : query.in_adj[static_cast<std::size_t>(query_vertex)]) {
            if (edge.to != query_vertex &&
                position[static_cast<std::size_t>(edge.to)] <
                    position[static_cast<std::size_t>(query_vertex)]) {
                neighbors.push_back(edge.to);
            }
        }
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    plan.plan_id = planFingerprint(plan.order);
    return plan;
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
    }
    return "UNKNOWN";
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
        total += dag_node.canonical_expression.capacity();
    }
    return total;
}

CandidateDag CandidateDagCompiler::compile(
    const Graph& query,
    const QueryPlan& plan,
    const QueryDecomposition&) {
    CandidateDag dag;
    const std::size_t vertex_count = static_cast<std::size_t>(query.num_vertices);
    dag.structural_roots.assign(vertex_count, kInvalidDagNode);
    dag.temporal_roots.assign(vertex_count, kInvalidDagNode);
    dag.longest_run_roots.assign(vertex_count, kInvalidDagNode);

    std::unordered_map<std::uint64_t, std::vector<DagNodeId>> expression_index;

    auto intern = [&](DagOp op,
                      DagResultKind result_kind,
                      std::vector<DagNodeId> parents,
                      std::vector<int> dependencies,
                      DagNodeParameter parameter,
                      const std::string& leaf_expression) -> DagNodeId {
        ++dag.nodes_attempted;
        std::sort(dependencies.begin(), dependencies.end());
        dependencies.erase(
            std::unique(dependencies.begin(), dependencies.end()),
            dependencies.end());

        if (op == DagOp::StructuralIntersection ||
            op == DagOp::TemporalIntersection) {
            std::sort(parents.begin(), parents.end(), [&](DagNodeId lhs, DagNodeId rhs) {
                return dag.node(lhs).canonical_expression <
                    dag.node(rhs).canonical_expression;
            });
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
        const std::uint64_t fingerprint = fnv1a(canonical);

        auto found_bucket = expression_index.find(fingerprint);
        if (found_bucket != expression_index.end()) {
            for (DagNodeId existing : found_bucket->second) {
                if (dag.node(existing).canonical_expression == canonical) {
                    ++dag.nodes_merged;
                    return existing;
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
        expression_index[fingerprint].push_back(dag.nodes.back().id);
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
        std::sort(parents.begin(), parents.end());
        parents.erase(std::unique(parents.begin(), parents.end()), parents.end());

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

    return dag;
}
