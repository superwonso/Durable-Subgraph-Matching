#include "CandidateDagPasses.h"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

std::uint64_t expressionFingerprint(const std::string& text) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

bool isIntersection(DagOp op) {
    return op == DagOp::StructuralIntersection ||
           op == DagOp::TemporalIntersection;
}

bool isEdgeLookup(DagOp op) {
    return op == DagOp::EdgeTimeLookup ||
           op == DagOp::GuardedEdgeTimeLookup;
}

bool isTdSource(DagOp op) {
    return op == DagOp::TdBlockLookup || op == DagOp::TdBlockView;
}

bool isNeighborSource(DagOp op) {
    return op == DagOp::OutNeighborLookup || op == DagOp::InNeighborLookup ||
           op == DagOp::OutNeighborView || op == DagOp::InNeighborView;
}

bool isPhysicalProducer(DagOp op) {
    return op == DagOp::FusedEdgeProbe || op == DagOp::RuntimeStructuralChoice;
}

const char* physicalAlphaName(double alpha) {
    if (alpha == 0.5) return "0.5";
    if (alpha == 1.0) return "1";
    if (alpha == 2.0) return "2";
    throw std::invalid_argument("physical alpha must be one of 0.5,1,2");
}

std::vector<int> parentDependencies(
    const CandidateDag& dag, const std::vector<DagNodeId>& parents) {
    std::vector<int> dependencies;
    for (DagNodeId parent : parents) {
        const auto& source = dag.node(parent).dependencies;
        dependencies.insert(dependencies.end(), source.begin(), source.end());
    }
    std::sort(dependencies.begin(), dependencies.end());
    dependencies.erase(
        std::unique(dependencies.begin(), dependencies.end()),
        dependencies.end());
    return dependencies;
}

DagNodeId appendNode(CandidateDag& dag, DagNode node) {
    if (dag.nodes.size() >= static_cast<std::size_t>(kInvalidDagNode)) {
        throw std::overflow_error("operator-opt DAG node ID overflow");
    }
    node.id = static_cast<DagNodeId>(dag.nodes.size());
    ++dag.nodes_attempted;
    ++dag.op_node_attempt_counts[dagOpIndex(node.op)];
    dag.nodes.push_back(std::move(node));
    return dag.nodes.back().id;
}

// Canonical identity and physical evaluation order are distinct. Rebuilding
// an identity sorts expressions, not node.parents, so P3 can change scheduling
// without accidentally making a commutative expression non-canonical.
void refreshIdentities(CandidateDag& dag) {
    for (DagNode& node : dag.nodes) {
        node.depth = 1;
        for (DagNodeId parent : node.parents) {
            if (parent >= node.id) {
                throw std::logic_error("operator-opt IR is not topological");
            }
            node.depth = std::max(node.depth, dag.node(parent).depth + 1);
        }
        if (node.parents.empty()) {
            node.canonical_expression = dagOperandCanonicalName(
                node.op,
                node.parameter.first_query_vertex,
                node.parameter.second_query_vertex);
            if (node.op == DagOp::BranchSupportInput) {
                std::ostringstream expression;
                expression << "BRANCH_SUPPORT_INPUT(stage=q"
                           << node.parameter.first_query_vertex << ";prefix=";
                for (int binding : node.dependencies) expression << binding << ',';
                expression << ')';
                node.canonical_expression = expression.str();
            }
        } else {
            std::vector<std::string> expressions;
            expressions.reserve(node.parents.size());
            for (DagNodeId parent : node.parents) {
                expressions.push_back(dag.node(parent).canonical_expression);
            }
            if (isIntersection(node.op)) {
                std::sort(expressions.begin(), expressions.end());
            }
            std::ostringstream expression;
            expression << dagOpName(node.op) << '(';
            for (std::size_t index = 0; index < expressions.size(); ++index) {
                if (index != 0) expression << ',';
                expression << expressions[index];
            }
            expression << ')';
            node.canonical_expression = expression.str();
        }
        if (node.raw_intervals) node.canonical_expression += "|raw-intervals";
        if (isPhysicalProducer(node.op)) {
            std::ostringstream physical;
            physical << "|stage=q" << node.parameter.first_query_vertex
                     << "|policy=" << dagStructuralPolicyName(node.structural_policy);
            if (node.structural_policy == DagStructuralPolicy::StateCost) {
                physical << "|alpha=" << physicalAlphaName(node.physical_alpha)
                         << "|probe=R*sum-ceil-log2-L|merge=R+sum-L|probe-if-le";
            }
            if (node.op == DagOp::FusedEdgeProbe) {
                physical << "|lmax-guard|slots=";
                for (const auto& slot : node.history_slots) {
                    physical << 'q' << slot.from_query_vertex << "->q"
                             << slot.to_query_vertex << ';';
                }
            }
            node.canonical_expression += physical.str();
        } else if (node.op == DagOp::BoundEdgeTimeLookup) {
            // The parent's canonical identity is stable across topological
            // renumbering; never put a mutable numeric producer ID in identity.
            node.canonical_expression += "|edge=q" +
                std::to_string(node.parameter.first_query_vertex) + "->q" +
                std::to_string(node.parameter.second_query_vertex) +
                "|slot=" + std::to_string(node.bound_slot) +
                (node.lmax_proven ? "|lmax-proven" : "");
        }
        node.static_fingerprint = expressionFingerprint(node.canonical_expression);
    }
}

// P4 introduces an already-materialized producer input to old time leaves.
// Stable Kahn order fixes those forward IDs without changing operand order.
// This routine is NEVER called on all-off or P1/P2/P3-only compilations.
void topologicallyRemapPhysical(CandidateDag& dag) {
    std::vector<std::size_t> indegree(dag.nodes.size(), 0);
    std::vector<std::vector<DagNodeId>> users(dag.nodes.size());
    for (const DagNode& node : dag.nodes) {
        for (DagNodeId parent : node.parents) {
            if (parent >= dag.nodes.size()) throw std::logic_error("physical parent outside DAG");
            ++indegree[node.id];
            users[parent].push_back(node.id);
        }
    }
    std::priority_queue<DagNodeId, std::vector<DagNodeId>, std::greater<DagNodeId>> ready;
    for (const DagNode& node : dag.nodes) {
        if (indegree[node.id] == 0) ready.push(node.id);
    }
    std::vector<DagNodeId> order;
    while (!ready.empty()) {
        const DagNodeId id = ready.top(); ready.pop();
        order.push_back(id);
        for (DagNodeId user : users[id]) {
            if (--indegree[user] == 0) ready.push(user);
        }
    }
    if (order.size() != dag.nodes.size()) throw std::logic_error("physical rewrite introduced a cycle");
    std::vector<DagNodeId> remap(order.size(), kInvalidDagNode);
    for (std::size_t index = 0; index < order.size(); ++index) {
        remap[order[index]] = static_cast<DagNodeId>(index);
    }
    std::vector<DagNode> reordered;
    reordered.reserve(order.size());
    for (DagNodeId id : order) {
        DagNode node = std::move(dag.nodes[id]);
        node.id = remap[id];
        for (DagNodeId& parent : node.parents) parent = remap.at(parent);
        if (node.bound_producer != kInvalidDagNode) {
            node.bound_producer = remap.at(node.bound_producer);
        }
        reordered.push_back(std::move(node));
    }
    for (auto* roots : {&dag.structural_roots, &dag.temporal_roots, &dag.longest_run_roots}) {
        for (DagNodeId& root : *roots) {
            if (root != kInvalidDagNode) root = remap.at(root);
        }
    }
    dag.nodes = std::move(reordered);
    // Bindings flow through producer inputs as through any other dependency.
    // Bound lookup additionally consumes the current candidate overlay q.
    for (DagNode& node : dag.nodes) {
        if (!node.parents.empty()) {
            node.dependencies = parentDependencies(dag, node.parents);
            if (node.op == DagOp::BoundEdgeTimeLookup) {
                node.dependencies.push_back(node.parameter.first_query_vertex);
                node.dependencies.push_back(node.parameter.second_query_vertex);
                std::sort(node.dependencies.begin(), node.dependencies.end());
                node.dependencies.erase(std::unique(node.dependencies.begin(), node.dependencies.end()), node.dependencies.end());
            }
        }
    }
}

void rewritePhysical(CandidateDag& dag, const QueryPlan& plan, double alpha) {
    const DagPasses passes = dag.passes;
    // P6: immutable Graph/TD storage outlives enumeration, so projecting Edge.to
    // or borrowing a TD block preserves the same sorted IDs without copying.
    // This changes lookup-result representation, not the intersection policy.
    if (passes.p6) {
        for (DagNode& node : dag.nodes) {
            if (node.op == DagOp::TdBlockLookup) node.op = DagOp::TdBlockView;
            else if (node.op == DagOp::OutNeighborLookup) node.op = DagOp::OutNeighborView;
            else if (node.op == DagOp::InNeighborLookup) node.op = DagOp::InNeighborView;
            else continue;
            node.result_kind = DagResultKind::CandidateView;
        }
    }

    if (passes.p4 || passes.p5) {
        std::vector<int> position(plan.order.size(), -1);
        for (std::size_t step = 0; step < plan.order.size(); ++step) {
            position.at(static_cast<std::size_t>(plan.order[step])) = static_cast<int>(step);
        }
        std::vector<DagNodeId> producers(plan.order.size(), kInvalidDagNode);
        const std::size_t old_node_count = dag.nodes.size();
        for (int q : plan.order) {
            const DagNodeId previous = dag.structural_roots.at(static_cast<std::size_t>(q));
            if (previous == kInvalidDagNode) continue; // root/self-loop remains legacy
            std::vector<DagNodeId> leaves;
            std::function<void(DagNodeId)> flatten = [&](DagNodeId id) {
                const DagNode& node = dag.node(id);
                if (node.op == DagOp::StructuralIntersection) {
                    for (DagNodeId parent : node.parents) flatten(parent);
                } else if (isTdSource(node.op) || isNeighborSource(node.op)) {
                    if (std::find(leaves.begin(), leaves.end(), id) == leaves.end()) leaves.push_back(id);
                } else {
                    throw std::logic_error("physical rewrite requires structural lookup operands");
                }
            };
            flatten(previous);
            const auto seed = std::find_if(leaves.begin(), leaves.end(), [&](DagNodeId id) { return isTdSource(dag.node(id).op); });
            if (seed == leaves.end() || std::count_if(leaves.begin(), leaves.end(), [&](DagNodeId id) { return isTdSource(dag.node(id).op); }) != 1) {
                throw std::logic_error("physical step requires one exact TD seed");
            }
            // The state cost R is specifically the TD seed, not a prior binary
            // intersection result. Keep remaining operand order deterministic.
            std::rotate(leaves.begin(), seed, std::next(seed));
            DagNode producer;
            producer.op = passes.p4 ? DagOp::FusedEdgeProbe : DagOp::RuntimeStructuralChoice;
            producer.result_kind = passes.p4 ? DagResultKind::CandidateBatch : DagResultKind::CandidateSet;
            producer.parameter.first_query_vertex = q;
            producer.parents = std::move(leaves);
            producer.dependencies = parentDependencies(dag, producer.parents);
            producer.structural_policy = passes.p5 ? DagStructuralPolicy::StateCost : DagStructuralPolicy::Probe;
            producer.physical_alpha = passes.p5 ? alpha : 1.0;
            if (passes.p4) {
                // Every edge is owned by the stage that binds its last endpoint.
                // Reciprocal arcs are distinct; nonroot self-loops get slots but
                // no fixed-anchor adjacency operand. The executor handles them
                // after other structural constraints, before reading payloads.
                for (std::size_t id = 0; id < old_node_count; ++id) {
                    const DagNode& edge = dag.nodes[id];
                    if (!isEdgeLookup(edge.op)) continue;
                    const int from = edge.parameter.first_query_vertex;
                    const int to = edge.parameter.second_query_vertex;
                    const int owner = position.at(from) >= position.at(to) ? from : to;
                    if (owner == q) producer.history_slots.push_back({from, to});
                }
                std::sort(producer.history_slots.begin(), producer.history_slots.end());
                producer.history_slots.erase(std::unique(producer.history_slots.begin(), producer.history_slots.end()), producer.history_slots.end());
                if (producer.history_slots.empty()) throw std::logic_error("P4 structural step has no closed edge slots");
            }
            // P4: membership and immutable edge identity arise from the same
            // directed adjacency entry; Lmax<k cannot participate in any valid
            // common length-k interval. P5 selects equivalent full-state probe
            // or merge set operations by fixed alpha, without enabling P3/P6.
            const DagNodeId id = appendNode(dag, std::move(producer));
            producers.at(static_cast<std::size_t>(q)) = id;
            dag.structural_roots.at(static_cast<std::size_t>(q)) = id;
        }
        if (passes.p4) {
            for (std::size_t id = 0; id < old_node_count; ++id) {
                DagNode& edge = dag.nodes[id];
                if (!isEdgeLookup(edge.op)) continue;
                const int from = edge.parameter.first_query_vertex;
                const int to = edge.parameter.second_query_vertex;
                const int owner = position.at(from) >= position.at(to) ? from : to;
                const DagNodeId producer = producers.at(static_cast<std::size_t>(owner));
                if (producer == kInvalidDagNode) continue; // root-only history
                const auto& slots = dag.node(producer).history_slots;
                const DagDirectedEdgeSlot identity{from, to};
                const auto slot = std::lower_bound(slots.begin(), slots.end(), identity);
                if (slot == slots.end() || !(*slot == identity) ||
                    static_cast<std::size_t>(slot - slots.begin()) >= kInvalidDagHistorySlot) {
                    throw std::logic_error("history consumer lost its exact directed slot");
                }
                edge.op = DagOp::BoundEdgeTimeLookup;
                edge.parents = {producer};
                edge.bound_producer = producer;
                edge.bound_slot = static_cast<std::uint32_t>(slot - slots.begin());
                edge.lmax_proven = true; // P2 must not recheck this proved guard.
            }
        }
        topologicallyRemapPhysical(dag);
    }
    refreshIdentities(dag);
}

// P1 invalidates every scalar-longest root: its explicit KappaNormalize root
// already proves the threshold fact used by the matcher. Scalar LONGEST_RUN
// is NOT a kappa node; this is dead diagnostic-node elimination, reported as
// such. Do not claim its removal alone implements the normalization fusion.
void removeUnusedLongestNodes(CandidateDag& dag) {
    std::vector<DagNodeId> remap(dag.nodes.size(), kInvalidDagNode);
    std::vector<DagNode> retained;
    retained.reserve(dag.nodes.size());
    for (DagNode& node : dag.nodes) {
        if (node.op == DagOp::LongestConsecutiveRun) {
            ++dag.nodes_eliminated_by_passes;
            continue;
        }
        remap[node.id] = static_cast<DagNodeId>(retained.size());
        node.id = static_cast<DagNodeId>(retained.size());
        retained.push_back(std::move(node));
    }
    auto translate = [&](DagNodeId id) {
        if (id == kInvalidDagNode) return id;
        if (id >= remap.size() || remap[id] == kInvalidDagNode) {
            throw std::logic_error("removed longest node remains referenced");
        }
        return remap[id];
    };
    for (DagNode& node : retained) {
        for (DagNodeId& parent : node.parents) parent = translate(parent);
    }
    for (auto* roots : {&dag.structural_roots, &dag.temporal_roots,
                        &dag.longest_run_roots}) {
        for (DagNodeId& root : *roots) root = translate(root);
    }
    dag.nodes = std::move(retained);
}

std::vector<bool> reachableFrom(
    const CandidateDag& dag, const std::vector<DagNodeId>& roots) {
    std::vector<bool> reachable(dag.nodes.size(), false);
    std::vector<DagNodeId> pending = roots;
    while (!pending.empty()) {
        const DagNodeId id = pending.back();
        pending.pop_back();
        if (id == kInvalidDagNode) continue;
        const DagNode& node = dag.node(id);
        if (reachable[id]) continue;
        reachable[id] = true;
        pending.insert(pending.end(), node.parents.begin(), node.parents.end());
    }
    return reachable;
}

std::string csvField(const std::string& value) {
    std::string quoted = "\"";
    for (char character : value) {
        if (character == '"') quoted += '"';
        quoted += character;
    }
    quoted += '"';
    return quoted;
}

std::string dotText(const std::string& value) {
    std::string escaped;
    for (char character : value) {
        if (character == '\\' || character == '"') escaped += '\\';
        if (character == '\n') {
            escaped += "\\n";
        } else {
            escaped += character;
        }
    }
    return escaped;
}

const char* nodeCategory(DagOp op) {
    switch (op) {
        case DagOp::TdBlockLookup:
        case DagOp::OutNeighborLookup:
        case DagOp::InNeighborLookup:
        case DagOp::EdgeTimeLookup:
            return "lookup";
        case DagOp::TdBlockView:
        case DagOp::OutNeighborView:
        case DagOp::InNeighborView:
            return "readonly_view";
        case DagOp::GuardedEdgeTimeLookup:
            return "guarded_lookup";
        case DagOp::FusedEdgeProbe:
            return "fused_guarded_producer";
        case DagOp::BoundEdgeTimeLookup:
            return "bound_lookup";
        case DagOp::BranchSupportInput:
            return "branch_input";
        case DagOp::StructuralIntersection:
        case DagOp::TemporalIntersection:
        case DagOp::LongestConsecutiveRun:
        case DagOp::KappaNormalize:
        case DagOp::RuntimeStructuralChoice:
            return "operator";
    }
    throw std::logic_error("unknown operator-opt node type");
}

void validateRewrite(const CandidateDag& dag, const QueryPlan& plan) {
    for (std::size_t index = 0; index < dag.nodes.size(); ++index) {
        const DagNode& node = dag.nodes[index];
        if (node.id != index) throw std::logic_error("non-dense IR node IDs");
        if (!std::isfinite(node.estimated_cardinality) ||
            node.estimated_cardinality < 0.0) {
            throw std::logic_error("invalid compiler cardinality estimate");
        }
        for (DagNodeId parent : node.parents) {
            if (parent >= node.id) throw std::logic_error("IR dependency cycle");
        }
        if (node.op == DagOp::BranchSupportInput && !node.parents.empty()) {
            throw std::logic_error("branch input cannot depend on another node");
        }
        if (node.op == DagOp::KappaNormalize && node.parents.size() != 1) {
            throw std::logic_error("kappa normalization must have one parent");
        }
        if (isEdgeLookup(node.op) && !node.parents.empty()) {
            throw std::logic_error("history lookup must be a leaf");
        }
        if (isPhysicalProducer(node.op)) {
            if (node.parents.empty() || !isTdSource(dag.node(node.parents.front()).op)) {
                throw std::logic_error("physical producer must start with exact TD seed");
            }
            for (std::size_t i = 1; i < node.parents.size(); ++i) {
                if (!isNeighborSource(dag.node(node.parents[i]).op)) {
                    throw std::logic_error("physical producer requires adjacency descriptors after seed");
                }
            }
            if (node.dependencies != parentDependencies(dag, node.parents) ||
                std::find(node.dependencies.begin(), node.dependencies.end(),
                          node.parameter.first_query_vertex) != node.dependencies.end()) {
                throw std::logic_error("physical producer depends on its unbound candidate");
            }
            const auto expected_policy = dag.passes.p5 ? DagStructuralPolicy::StateCost : DagStructuralPolicy::Probe;
            if (node.structural_policy != expected_policy ||
                (node.op == DagOp::FusedEdgeProbe) != dag.passes.p4) {
                throw std::logic_error("physical pass/policy mismatch");
            }
            physicalAlphaName(node.physical_alpha);
            if (node.op == DagOp::FusedEdgeProbe) {
                if (node.result_kind != DagResultKind::CandidateBatch || node.history_slots.empty() ||
                    !std::is_sorted(node.history_slots.begin(), node.history_slots.end()) ||
                    std::adjacent_find(node.history_slots.begin(), node.history_slots.end()) != node.history_slots.end()) {
                    throw std::logic_error("invalid P4 batch slot schema");
                }
                for (const auto& slot : node.history_slots) {
                    if (slot.from_query_vertex != node.parameter.first_query_vertex &&
                        slot.to_query_vertex != node.parameter.first_query_vertex) {
                        throw std::logic_error("P4 slot is not incident to its candidate stage");
                    }
                }
            } else if (node.result_kind != DagResultKind::CandidateSet || !node.history_slots.empty()) {
                throw std::logic_error("P5-only must not produce bound history handles");
            }
        }
        if (node.op == DagOp::BoundEdgeTimeLookup) {
            if (node.parents.size() != 1 || node.parents.front() != node.bound_producer || !node.lmax_proven) {
                throw std::logic_error("bound history lacks its proved producer input");
            }
            const DagNode& producer = dag.node(node.bound_producer);
            const DagDirectedEdgeSlot expected{node.parameter.first_query_vertex, node.parameter.second_query_vertex};
            if (producer.op != DagOp::FusedEdgeProbe || node.bound_slot >= producer.history_slots.size() ||
                !(producer.history_slots[node.bound_slot] == expected) || node.result_kind != DagResultKind::IntervalSet) {
                throw std::logic_error("bound history slot/endpoint mismatch");
            }
        }
    }
    if (!dag.passes.p1) return;
    if (!dag.cumulative_support) throw std::logic_error("P1 lost support contract");
    for (int query_vertex : plan.order) {
        const DagNodeId root = dag.temporal_roots.at(
            static_cast<std::size_t>(query_vertex));
        if (root == kInvalidDagNode) continue;
        const DagNode& kappa = dag.node(root);
        if (kappa.op != DagOp::KappaNormalize ||
            kappa.result_kind != DagResultKind::IntervalSet) {
            throw std::logic_error("P1 temporal root is not explicit final kappa");
        }
        const DagNode& intersection = dag.node(kappa.parents.front());
        if (intersection.op != DagOp::TemporalIntersection ||
            !intersection.raw_intervals || !intersection.short_circuit ||
            intersection.parents.empty() ||
            dag.node(intersection.parents.front()).op !=
                DagOp::BranchSupportInput) {
            throw std::logic_error("P1 must begin a raw intersection with J");
        }
    }
}

} // namespace

DagPasses parseDagPasses(const std::string& value) {
    DagPasses result;
    if (value == "none" || value == "off") return result;
    if (value.empty()) throw std::invalid_argument("--dag-passes cannot be empty");
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(',', start);
        const std::string token = value.substr(
            start, separator == std::string::npos ? separator : separator - start);
        bool* flag = token == "p1" ? &result.p1 :
                     token == "p2" ? &result.p2 :
                     token == "p3" ? &result.p3 :
                     token == "p4" ? &result.p4 :
                     token == "p5" ? &result.p5 :
                     token == "p6" ? &result.p6 : nullptr;
        if (flag == nullptr || *flag) {
            throw std::invalid_argument(
                "--dag-passes requires a non-repeating subset of p1,p2,p3,p4,p5,p6");
        }
        *flag = true;
        if (separator == std::string::npos) break;
        start = separator + 1;
    }
    return result;
}

std::string dagPassesName(DagPasses passes) {
    std::string name;
    if (passes.p1) name = "p1";
    if (passes.p2) name += (name.empty() ? "" : ",") + std::string("p2");
    if (passes.p3) name += (name.empty() ? "" : ",") + std::string("p3");
    if (passes.p4) name += (name.empty() ? "" : ",") + std::string("p4");
    if (passes.p5) name += (name.empty() ? "" : ",") + std::string("p5");
    if (passes.p6) name += (name.empty() ? "" : ",") + std::string("p6");
    return name.empty() ? "none" : name;
}

std::string dagOperandCanonicalName(DagOp op, int first, int second) {
    std::ostringstream expression;
    expression << dagOpName(op) << '(';
    switch (op) {
        case DagOp::TdBlockLookup:
        case DagOp::TdBlockView:
            expression << 'q' << first << ",p" << second;
            break;
        case DagOp::OutNeighborLookup:
        case DagOp::InNeighborLookup:
        case DagOp::OutNeighborView:
        case DagOp::InNeighborView:
            expression << 'q' << first;
            break;
        case DagOp::EdgeTimeLookup:
        case DagOp::GuardedEdgeTimeLookup:
            expression << 'q' << first << "->q" << second;
            break;
        case DagOp::BranchSupportInput:
            expression << "stage=q" << first;
            break;
        default:
            throw std::invalid_argument("requested canonical name of non-leaf op");
    }
    expression << ')';
    return expression.str();
}

DagCompileEstimates computeDagCompileEstimates(
    const Graph& data, const Graph& query, const TDTree& tree) {
    DagCompileEstimates estimates;
    estimates.query_labels = query.vertex_labels;
    for (Label label : estimates.query_labels) {
        if (label >= kLabelCount) {
            throw std::logic_error("query estimate label is outside runtime alphabet");
        }
    }
    estimates.td_block_by_query_vertex.resize(
        static_cast<std::size_t>(query.num_vertices), 0.0);
    for (int query_vertex = 0; query_vertex < query.num_vertices; ++query_vertex) {
        const std::size_t blocks = tree.blockCount(query_vertex);
        estimates.td_block_by_query_vertex[static_cast<std::size_t>(query_vertex)] =
            blocks == 0 ? 0.0 :
            static_cast<double>(tree.relationEntryCount(query_vertex)) /
                static_cast<double>(blocks);
    }
    std::array<std::uint64_t, kLabelCount> vertices{};
    std::array<long double, kLabelCount> outgoing{};
    std::array<long double, kLabelCount> incoming{};
    std::array<std::array<std::uint64_t, kLabelCount>, kLabelCount> edges{};
    std::array<std::array<long double, kLabelCount>, kLabelCount> histories{};
    for (int vertex = 0; vertex < data.num_vertices; ++vertex) {
        const std::size_t index = static_cast<std::size_t>(vertex);
        const Label label = data.vertex_labels.at(index);
        // Explicit query label projection deliberately leaves irrelevant data
        // vertices unlabelled. They remain in the graph but cannot bind query
        // vertices; do not treat those sentinel labels as an invalid graph.
        if (label >= kLabelCount) continue;
        ++vertices[label];
        outgoing[label] += data.adj.at(index).size();
        incoming[label] += data.in_adj.at(index).size();
    }
    for (const TemporalEdge& edge : data.temporal_edges) {
        const Label source = data.vertex_labels.at(static_cast<std::size_t>(edge.u));
        const Label target = data.vertex_labels.at(static_cast<std::size_t>(edge.v));
        if (source >= kLabelCount || target >= kLabelCount) continue;
        ++edges.at(source).at(target);
        histories.at(source).at(target) += edge.activeIntervals().size();
    }
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (vertices[label] != 0) {
            estimates.outgoing_by_label[label] =
                static_cast<double>(outgoing[label] / vertices[label]);
            estimates.incoming_by_label[label] =
                static_cast<double>(incoming[label] / vertices[label]);
        }
        for (std::size_t target = 0; target < kLabelCount; ++target) {
            if (edges[label][target] != 0) {
                estimates.history_by_endpoint_labels[label][target] =
                    static_cast<double>(histories[label][target] / edges[label][target]);
            }
        }
    }
    return estimates;
}

double DagCompileEstimates::cardinality(DagOp op, int first, int second) const {
    if (first < 0 || static_cast<std::size_t>(first) >= query_labels.size()) {
        throw std::out_of_range("query vertex for compiler estimate");
    }
    const Label label = query_labels[static_cast<std::size_t>(first)];
    switch (op) {
        case DagOp::TdBlockLookup:
            return td_block_by_query_vertex.at(static_cast<std::size_t>(first));
        case DagOp::OutNeighborLookup:
            return outgoing_by_label.at(label);
        case DagOp::InNeighborLookup:
            return incoming_by_label.at(label);
        case DagOp::EdgeTimeLookup:
        case DagOp::GuardedEdgeTimeLookup:
            if (second < 0 || static_cast<std::size_t>(second) >= query_labels.size()) {
                throw std::out_of_range("history endpoint for compiler estimate");
            }
            return history_by_endpoint_labels.at(label).at(
                query_labels[static_cast<std::size_t>(second)]);
        case DagOp::BranchSupportInput:
            // J is deliberately first, independently of its unknown runtime size.
            return 0.0;
        default:
            throw std::invalid_argument("compiler estimate requested for non-leaf");
    }
}

CandidateDag optimizeCandidateDag(
    const CandidateDag& base,
    const Graph& data,
    const Graph& query,
    const QueryPlan& plan,
    const TDTree& tree,
    DagPasses passes,
    double physical_alpha) {
    physicalAlphaName(physical_alpha);
    if (!passes.p5 && physical_alpha != 1.0) {
        throw std::invalid_argument("non-default physical alpha requires P5");
    }
    // This early return is the off contract: no new nodes, changed attributes,
    // order, canonical strings, counters, or scans of the graph.
    if (!passes.any()) return base;
    if (base.passes.any()) {
        throw std::invalid_argument("optimizeCandidateDag requires an unmodified P0 DAG");
    }
    CandidateDag dag = base;
    dag.passes = passes;

    // P2: Lmax(T(e)) < k is a necessary-condition failure. Change the lookup
    // opcode at compile time; the executor can reject before reading history.
    // The immutable TemporalEdge already stores Lmax, so no new array is built.
    if (passes.p2) {
        for (DagNode& node : dag.nodes) {
            if (node.op == DagOp::EdgeTimeLookup) {
                node.op = DagOp::GuardedEdgeTimeLookup;
            } else if (node.op == DagOp::TemporalIntersection) {
                // Propagate a failing guard immediately through its consuming
                // intersection: later history operands must stay unevaluated.
                // Preserve P0's operand order; P3 alone chooses a new order.
                node.short_circuit = true;
            }
        }
    }

    // P1 applies only to no-memo executions (enforced by the caller). Original
    // loads/intersections fuse kappa internally; raw_intervals disables that
    // implicit normalization. kappa(J & kappa(X)) == kappa(J & X) follows
    // because a final length-k run must lie inside a qualifying run of X.
    if (passes.p1) {
        dag.cumulative_support = true;
        for (DagNode& node : dag.nodes) {
            if (isEdgeLookup(node.op) || node.op == DagOp::TemporalIntersection) {
                node.raw_intervals = true;
            }
        }
        std::vector<int> prefix_bindings;
        for (int vertex : plan.order) {
            const std::size_t index = static_cast<std::size_t>(vertex);
            const DagNodeId previous_root = dag.temporal_roots.at(index);
            if (previous_root != kInvalidDagNode) {
                DagNode input;
                input.op = DagOp::BranchSupportInput;
                input.result_kind = DagResultKind::IntervalSet;
                input.parameter.first_query_vertex = vertex;
                input.dependencies = prefix_bindings;
                std::sort(input.dependencies.begin(), input.dependencies.end());
                const DagNodeId input_id = appendNode(dag, std::move(input));

                DagNode intersection;
                intersection.op = DagOp::TemporalIntersection;
                intersection.result_kind = DagResultKind::IntervalSet;
                intersection.raw_intervals = true;
                intersection.short_circuit = true;
                intersection.parents.push_back(input_id);
                const DagNode& previous = dag.node(previous_root);
                if (previous.op == DagOp::TemporalIntersection) {
                    intersection.parents.insert(intersection.parents.end(),
                        previous.parents.begin(), previous.parents.end());
                } else {
                    intersection.parents.push_back(previous_root);
                }
                intersection.dependencies = parentDependencies(dag, intersection.parents);
                const DagNodeId intersection_id = appendNode(dag, std::move(intersection));

                DagNode normalization;
                normalization.op = DagOp::KappaNormalize;
                normalization.result_kind = DagResultKind::IntervalSet;
                normalization.parameter.first_query_vertex = vertex;
                normalization.parents = {intersection_id};
                normalization.dependencies = dag.node(intersection_id).dependencies;
                dag.temporal_roots[index] = appendNode(dag, std::move(normalization));
            }
            dag.longest_run_roots.at(index) = kInvalidDagNode;
            prefix_bindings.push_back(vertex);
        }
        removeUnusedLongestNodes(dag);
    }

    refreshIdentities(dag);
    if (passes.p1 || passes.p3) {
        const DagCompileEstimates estimates = computeDagCompileEstimates(data, query, tree);
        for (DagNode& node : dag.nodes) {
            if (node.parents.empty()) {
                node.estimated_cardinality = estimates.cardinality(node.op,
                    node.parameter.first_query_vertex, node.parameter.second_query_vertex);
            } else if (node.op == DagOp::LongestConsecutiveRun) {
                node.estimated_cardinality = 1.0;
            } else {
                node.estimated_cardinality = std::numeric_limits<double>::max();
                for (DagNodeId parent : node.parents) {
                    node.estimated_cardinality = std::min(node.estimated_cardinality,
                        dag.node(parent).estimated_cardinality);
                }
            }

            // P3: associative intersection flattening is already present in P0.
            // Re-flatten defensively, preserving raw-vs-normalized semantics.
            // Ordering independent operands does not change the set result;
            // empty intersection is absorbing, so remaining parents are dead.
            const bool p1_cumulative_intersection = passes.p1 &&
                node.op == DagOp::TemporalIntersection &&
                !node.parents.empty() &&
                dag.node(node.parents.front()).op == DagOp::BranchSupportInput;
            if (!isIntersection(node.op) || (!passes.p3 && !p1_cumulative_intersection)) {
                continue;
            }
            if (passes.p3) {
                std::vector<DagNodeId> flattened;
                for (DagNodeId parent : node.parents) {
                    const DagNode& operand = dag.node(parent);
                    if (operand.op == node.op && operand.raw_intervals == node.raw_intervals) {
                        flattened.insert(flattened.end(), operand.parents.begin(), operand.parents.end());
                    } else {
                        flattened.push_back(parent);
                    }
                }
                node.parents = std::move(flattened);
                node.short_circuit = true;
                node.adaptive_intersection = true;
            }
            std::stable_sort(node.parents.begin(), node.parents.end(),
                [&](DagNodeId lhs, DagNodeId rhs) {
                    const DagNode& left = dag.node(lhs);
                    const DagNode& right = dag.node(rhs);
                    const bool left_branch = left.op == DagOp::BranchSupportInput;
                    const bool right_branch = right.op == DagOp::BranchSupportInput;
                    if (left_branch != right_branch) return left_branch;
                    if (left.estimated_cardinality != right.estimated_cardinality) {
                        return left.estimated_cardinality < right.estimated_cardinality;
                    }
                    if (left.canonical_expression != right.canonical_expression) {
                        return left.canonical_expression < right.canonical_expression;
                    }
                    return lhs < rhs;
                });
        }
        refreshIdentities(dag);
    }
    if (passes.physicalAny()) rewritePhysical(dag, plan, physical_alpha);
    validateRewrite(dag, plan);
    return dag;
}

void dumpCandidateDag(
    const CandidateDag& dag,
    const QueryPlan& plan,
    const std::string& prefix,
    const std::string& stage_label) {
    const std::array<std::string, 3> paths = {
        prefix + ".dot", prefix + ".node_counts.csv", prefix + ".nodes.csv"};
    for (const std::string& path : paths) {
        if (std::filesystem::exists(path)) {
            throw std::runtime_error("refusing to overwrite DAG dump: " + path);
        }
    }
    const auto parent_path = std::filesystem::path(prefix).parent_path();
    if (!parent_path.empty()) std::filesystem::create_directories(parent_path);
    std::ofstream dot(paths[0]);
    std::ofstream counts(paths[1]);
    std::ofstream nodes(paths[2]);
    if (!dot || !counts || !nodes) throw std::runtime_error("cannot create DAG dump files");

    std::vector<DagNodeId> all_roots;
    for (const auto* roots : {&dag.structural_roots, &dag.temporal_roots,
                              &dag.longest_run_roots}) {
        all_roots.insert(all_roots.end(), roots->begin(), roots->end());
    }
    const auto reachable = reachableFrom(dag, all_roots);
    // Appended opcodes must not add six zero rows or new columns to legacy
    // P0/P123 dumps. The 96 archived all-off exports remain byte references.
    const bool physical_dump = std::any_of(dag.nodes.begin(), dag.nodes.end(),
        [](const DagNode& node) { return dagOpIndex(node.op) >= kLegacyDagOpCount; });
    const std::size_t dumped_op_count = physical_dump ? kDagOpCount : kLegacyDagOpCount;
    dot << "digraph CandidateDAG {\n  rankdir=BT;\n  labelloc=\"t\";\n  label=\""
        << dotText(stage_label) << " | plan=" << plan.plan_id
        << " | gray=dormant materialized node | edge number=evaluation order"
        << " | eliminated scalar-longest nodes=" << dag.nodes_eliminated_by_passes
        << "\";\n";
    nodes << "query_fingerprint,plan_id,pass_stage,node_id,op,result_kind,parents_in_evaluation_order,dependencies,raw_intervals,short_circuit,adaptive_intersection,estimated_cardinality,reachable,canonical_expression";
    if (physical_dump) nodes << ",structural_policy,physical_alpha,history_slots,bound_producer,bound_slot,lmax_proven";
    nodes << '\n';
    for (const DagNode& node : dag.nodes) {
        std::ostringstream label;
        label << node.id << ": " << dagOpName(node.op);
        if (node.parents.empty()) {
            label << " q" << node.parameter.first_query_vertex;
            if (node.parameter.second_query_vertex >= 0) label << ",q" << node.parameter.second_query_vertex;
        }
        if ((isEdgeLookup(node.op) || node.op == DagOp::BoundEdgeTimeLookup || node.op == DagOp::TemporalIntersection) &&
            !node.raw_intervals) label << "\nincludes implicit kappa";
        if (node.op == DagOp::LongestConsecutiveRun) label << "\nscalar max, NOT kappa";
        if (node.raw_intervals) label << "\nraw intervals";
        if (node.short_circuit) label << "\nshort circuit";
        if (node.adaptive_intersection) label << "\nadaptive ratio >= 32";
        if (dag.passes.p1 || dag.passes.p3) label << "\nest=" << node.estimated_cardinality;
        if (isPhysicalProducer(node.op)) {
            label << "\nq" << node.parameter.first_query_vertex << " / "
                  << dagStructuralPolicyName(node.structural_policy);
            if (node.structural_policy == DagStructuralPolicy::StateCost) {
                label << " alpha=" << physicalAlphaName(node.physical_alpha)
                      << "\nR*sum(ceilLog2 L) <= alpha*(R+sum L)";
            }
            if (node.op == DagOp::FusedEdgeProbe) {
                label << "\nframe-owned handles; Lmax guard; slots=" << node.history_slots.size();
            }
        }
        if (node.op == DagOp::BoundEdgeTimeLookup) {
            label << "\nq" << node.parameter.first_query_vertex << "->q"
                  << node.parameter.second_query_vertex << " / slot " << node.bound_slot
                  << "\nconsumes existing frame row; no producer re-evaluation";
            if (node.lmax_proven) label << "\nLmax already proved";
        }
        dot << "  n" << node.id << " [label=\"" << dotText(label.str()) << "\"";
        if (!reachable[node.id]) dot << ",color=gray,fontcolor=gray,style=dashed";
        if (node.op == DagOp::BranchSupportInput) dot << ",shape=oval";
        else if (node.op == DagOp::GuardedEdgeTimeLookup) dot << ",shape=diamond";
        else dot << ",shape=box";
        dot << "];\n";
        std::ostringstream parent_list, dependency_list;
        for (std::size_t position = 0; position < node.parents.size(); ++position) {
            if (position != 0) parent_list << ';';
            parent_list << node.parents[position];
            dot << "  n" << node.id << " -> n" << node.parents[position]
                << " [label=\"" << position + 1 << "\"];\n";
        }
        for (std::size_t position = 0; position < node.dependencies.size(); ++position) {
            if (position != 0) dependency_list << ';';
            dependency_list << node.dependencies[position];
        }
        nodes << plan.query_fingerprint << ',' << plan.plan_id << ',' << csvField(stage_label)
              << ',' << node.id << ',' << dagOpMetricName(node.op) << ','
              << dagResultKindName(node.result_kind) << ',' << csvField(parent_list.str())
              << ',' << csvField(dependency_list.str()) << ',' << node.raw_intervals
              << ',' << node.short_circuit << ',' << node.adaptive_intersection
              << ',' << node.estimated_cardinality << ',' << reachable[node.id]
              << ',' << csvField(node.canonical_expression);
        if (physical_dump) {
            std::ostringstream slots;
            for (std::size_t index = 0; index < node.history_slots.size(); ++index) {
                if (index != 0) slots << ';';
                slots << node.history_slots[index].from_query_vertex << "->"
                      << node.history_slots[index].to_query_vertex;
            }
            nodes << ',';
            if (isPhysicalProducer(node.op)) nodes << dagStructuralPolicyName(node.structural_policy);
            nodes << ',';
            if (node.structural_policy == DagStructuralPolicy::StateCost) nodes << physicalAlphaName(node.physical_alpha);
            nodes << ',' << csvField(slots.str()) << ',';
            if (node.bound_producer != kInvalidDagNode) nodes << node.bound_producer;
            nodes << ',';
            if (node.bound_slot != kInvalidDagHistorySlot) nodes << node.bound_slot;
            nodes << ',' << node.lmax_proven;
        }
        nodes << '\n';
    }
    for (std::size_t step = 0; step < plan.order.size(); ++step) {
        const auto vertex = static_cast<std::size_t>(plan.order[step]);
        for (const auto& root_kind : {
                 std::make_pair("structure", dag.structural_roots.at(vertex)),
                 std::make_pair("time", dag.temporal_roots.at(vertex)),
                 std::make_pair("longest", dag.longest_run_roots.at(vertex))}) {
            if (root_kind.second == kInvalidDagNode) continue;
            dot << "  root_" << step << '_' << root_kind.first
                << " [shape=plaintext,label=\"step " << step << " / q" << vertex
                << " / " << root_kind.first << "\"];\n  root_" << step << '_'
                << root_kind.first << " -> n" << root_kind.second << ";\n";
        }
    }
    dot << "}\n";

    counts << "query_fingerprint,plan_id,pass_stage,query_step,query_vertex,op,node_category,materialized_nodes,reachable_nodes\n";
    const auto topology = dag.topologySummary();
    for (std::size_t op = 0; op < dumped_op_count; ++op) {
        const auto type = static_cast<DagOp>(op);
        counts << plan.query_fingerprint << ',' << plan.plan_id << ',' << csvField(stage_label)
               << ",ALL,ALL," << dagOpMetricName(type) << ',' << nodeCategory(type)
               << ',' << topology.op_node_counts[op] << ','
               << topology.reachable_op_node_counts[op] << '\n';
    }
    for (std::size_t step = 0; step < plan.order.size(); ++step) {
        const auto vertex = static_cast<std::size_t>(plan.order[step]);
        const auto stage_reachable = reachableFrom(dag, {
            dag.structural_roots.at(vertex), dag.temporal_roots.at(vertex),
            dag.longest_run_roots.at(vertex)});
        std::array<std::uint64_t, kDagOpCount> stage_counts{};
        for (const DagNode& node : dag.nodes) {
            if (stage_reachable[node.id]) ++stage_counts[dagOpIndex(node.op)];
        }
        for (std::size_t op = 0; op < dumped_op_count; ++op) {
            const auto type = static_cast<DagOp>(op);
            counts << plan.query_fingerprint << ',' << plan.plan_id << ',' << csvField(stage_label)
                   << ',' << step << ',' << vertex << ',' << dagOpMetricName(type)
                   << ',' << nodeCategory(type) << ",," << stage_counts[op] << '\n';
        }
    }
    dot.flush();
    counts.flush();
    nodes.flush();
    if (!dot || !counts || !nodes) throw std::runtime_error("failed writing DAG dumps");
}
