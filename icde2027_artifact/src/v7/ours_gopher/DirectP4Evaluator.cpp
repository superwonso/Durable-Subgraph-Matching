#include "DirectP4Evaluator.h"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string>
#include <utility>

#include "OpKernels.h"
#include "PhysicalKernels.h"
#include "JIndexExecution.h"

DirectP4Evaluator::DirectP4Evaluator(
    const Graph& temporal_graph, const Graph& query_graph,
    const QueryPlan& plan, const TDTree& td_tree, int minimum_duration,
    OperatorCounters* ops, bool window_mode)
    : G_(temporal_graph), plan_(plan), td_tree_(td_tree),
      k_threshold_(std::max(1, minimum_duration)), ops_(ops),
      window_mode_(window_mode),
      recipes_(static_cast<std::size_t>(query_graph.num_vertices)) {
    const auto count = static_cast<std::size_t>(query_graph.num_vertices);
    if (plan_.order.size() != count || plan_.parent.size() != count)
        throw std::invalid_argument("DirectP4 requires the complete existing QueryPlan");
    std::vector<int> position(count, -1);
    for (std::size_t i = 0; i < count; ++i) {
        const int q = plan_.order[i];
        if (q < 0 || static_cast<std::size_t>(q) >= count || position[q] >= 0)
            throw std::invalid_argument("DirectP4 query order is not a permutation");
        position[static_cast<std::size_t>(q)] = static_cast<int>(i);
    }
    for (int q : plan_.order) {
        auto& r = recipes_[static_cast<std::size_t>(q)];
        if (GraphUtils::hasEdge(query_graph.adj, q, q)) r.slots.push_back({q, q});
        for (const auto& edge : query_graph.adj[static_cast<std::size_t>(q)]) {
            if (edge.to != q && position[static_cast<std::size_t>(edge.to)] < position[q])
                r.slots.push_back({q, edge.to});
        }
        for (const auto& edge : query_graph.in_adj[static_cast<std::size_t>(q)]) {
            if (edge.to != q && position[static_cast<std::size_t>(edge.to)] < position[q])
                r.slots.push_back({edge.to, q});
        }
        std::sort(r.slots.begin(), r.slots.end(), [](const DirectedSlot& a, const DirectedSlot& b) {
            return std::pair<int, int>{a.from, a.to} < std::pair<int, int>{b.from, b.to};
        });
        r.slots.erase(std::unique(r.slots.begin(), r.slots.end(),
            [](const DirectedSlot& a, const DirectedSlot& b) {
                return a.from == b.from && a.to == b.to;
            }), r.slots.end());
        for (std::size_t i = 0; i < r.slots.size(); ++i) {
            const auto& slot = r.slots[i];
            if (slot.from == q && slot.to == q) r.self_slot = i;
            else if (slot.from == q) r.probes.push_back({slot.to, false, i});
            else r.probes.push_back({slot.from, true, i});
        }
        // Match P0/P4's canonical operand order, not integer-label order.
        // In particular q10 sorts before q2, and IN precedes OUT. This is
        // constructor-only plain recipe preparation, not DAG compilation.
        auto probe_key = [](const Probe& p) {
            return std::string(p.outgoing ? "OUT_NEIGHBORS(q" : "IN_NEIGHBORS(q") +
                std::to_string(p.anchor) + ")";
        };
        std::sort(r.probes.begin(), r.probes.end(), [&](const Probe& a, const Probe& b) {
            return probe_key(a) < probe_key(b);
        });
        r.temporal_order.resize(r.slots.size());
        std::iota(r.temporal_order.begin(), r.temporal_order.end(), std::size_t{0});
        auto time_key = [&](std::size_t index) {
            const auto& s = r.slots[index];
            return "EDGE_TIME(q" + std::to_string(s.from) + "->q" + std::to_string(s.to) + ")";
        };
        std::sort(r.temporal_order.begin(), r.temporal_order.end(),
                  [&](std::size_t a, std::size_t b) { return time_key(a) < time_key(b); });
    }
}

int DirectP4Evaluator::binding(int q, const SearchState& state, int overlay_q, int overlay_candidate) {
    if (q == overlay_q) return overlay_candidate;
    return q < 0 || static_cast<std::size_t>(q) >= state.mapping.size()
        ? -1 : state.mapping[static_cast<std::size_t>(q)];
}

const DirectP4Evaluator::Recipe& DirectP4Evaluator::recipe(int q) const {
    if (q < 0 || static_cast<std::size_t>(q) >= recipes_.size())
        throw std::out_of_range("DirectP4 query vertex");
    return recipes_[static_cast<std::size_t>(q)];
}

PhysicalCandidateBatch DirectP4Evaluator::physicalCandidates(
    int q, const SearchState& state) const {
    const auto& r = recipe(q);
    if (q == plan_.root) return {};  // root candidates remain the matcher's TD root path
    const int parent = binding(plan_.parent[static_cast<std::size_t>(q)], state);
    const auto* block = td_tree_.candidatesForParent(q, parent);
    const CandidateReadView seed = block ? CandidateReadView(*block) : CandidateReadView{};
    OPS_ADD(ops_, td_block_lookups_executed, 1);
    OPS_ADD(ops_, physical_private_descriptors, 1);
    OPS_ADD(ops_, physical_input_candidates, seed.size());
    if (ops_) ops_->physical_profile.recordOperand(
        {StructuralProfile::Backend::Direct, static_cast<DagNodeId>(q), q},
        StructuralProfile::Operand::TdBlock, seed.size());

    std::vector<CandidateReadView> neighbors;
    neighbors.reserve(r.probes.size());
    for (const auto& probe : r.probes) {
        const int anchor = binding(probe.anchor, state);
        const auto* edges = anchor < 0 || anchor >= G_.num_vertices ? nullptr :
            &(probe.outgoing ? G_.adj : G_.in_adj)[static_cast<std::size_t>(anchor)];
        neighbors.push_back(edges ? CandidateReadView(*edges) : CandidateReadView{});
        OPS_ADD(ops_, neighbor_lookups_executed, 1);
        OPS_ADD(ops_, physical_private_descriptors, 1);
        if (ops_) ops_->physical_profile.recordOperand(
            {StructuralProfile::Backend::Direct, static_cast<DagNodeId>(q), probe.anchor},
            probe.outgoing ? StructuralProfile::Operand::OutNeighbor :
                             StructuralProfile::Operand::InNeighbor, neighbors.back().size());
    }
    const auto stride = r.slots.size();
    if (stride && seed.size() > std::numeric_limits<std::size_t>::max() / stride)
        throw std::overflow_error("DirectP4 history row size overflow");
    auto result = PhysicalCandidateBatch::owned({});
    result.owned_candidates.reserve(seed.size());
    // The shared batch's producer field is only a row-owner tag here: q is
    // the stable plain recipe ID, never an interpreted Candidate-DAG node.
    result.producer = static_cast<DagNodeId>(q);
    result.query_vertex = q;
    result.history_stride = stride;
    std::vector<int> row(stride, -1);
    std::size_t lmax_rejected = 0;
    auto bind_edge = [&](const Edge* entry, std::size_t index, int candidate) {
        if (!entry || entry->temporal_edge_id < 0 ||
            static_cast<std::size_t>(entry->temporal_edge_id) >= G_.temporal_edges.size())
            throw std::logic_error("DirectP4 adjacency has invalid temporal edge ID");
        const auto& edge = G_.temporal_edges[static_cast<std::size_t>(entry->temporal_edge_id)];
        const auto& slot = r.slots[index];
        if (edge.u != binding(slot.from, state, q, candidate) ||
            edge.v != binding(slot.to, state, q, candidate))
            throw std::logic_error("DirectP4 adjacency direction disagrees with recipe");
        // For the W arm, the exact WindowAnd absorbs this necessary Lmax
        // guard. Merely creating a structural row still never reads history.
        if (!window_mode_) {
            OPS_ADD(ops_, physical_lmax_checks, 1);
            OPS_ADD(ops_, history_guard_checks, 1);
            if (edge.maximumConsecutiveRun() < k_threshold_) {
                if (ops_) ++lmax_rejected;
                OPS_ADD(ops_, physical_lmax_rejections, 1);
                OPS_ADD(ops_, history_guard_failures, 1);
                return false;
            }
        }
        row[index] = entry->temporal_edge_id;
        OPS_ADD(ops_, physical_handle_writes, 1);
        return true;
    };
    for (std::size_t i = 0; i < seed.size(); ++i) {
        const int candidate = seed[i];
        bool accepted = true;
        for (std::size_t j = 0; j < neighbors.size(); ++j) {
            OPS_ADD(ops_, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(neighbors[j], candidate);
            if (found == neighbors[j].size() || neighbors[j][found] != candidate) {
                accepted = false;
                break;
            }
            if (!bind_edge(neighbors[j].edge(found), r.probes[j].slot, candidate)) {
                accepted = false;
                break;
            }
        }
        if (accepted && r.self_slot != static_cast<std::size_t>(-1)) {
            if (candidate < 0 || candidate >= G_.num_vertices)
                throw std::logic_error("DirectP4 invalid candidate");
            const CandidateReadView adjacency(G_.adj[static_cast<std::size_t>(candidate)]);
            OPS_ADD(ops_, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(adjacency, candidate);
            accepted = found < adjacency.size() && adjacency[found] == candidate &&
                bind_edge(adjacency.edge(found), r.self_slot, candidate);
        }
        if (!accepted) continue;
        result.owned_candidates.push_back(candidate);
        if (stride) {
            result.temporal_edge_ids.insert(result.temporal_edge_ids.end(), row.begin(), row.end());
            OPS_ADD(ops_, physical_handle_copy_bytes, stride * sizeof(int));
        }
    }
    result.lmax_rejected_candidates = lmax_rejected;
    if (result.temporal_edge_ids.size() != result.size() * stride)
        throw std::logic_error("DirectP4 incomplete history row");
    OPS_ADD(ops_, physical_probe_selections, 1);
    OPS_ADD(ops_, physical_output_elements_materialized, result.size());
    OPS_ADD(ops_, physical_raw_candidates, seed.size());
    OPS_ADD(ops_, physical_structure_rejections, seed.size() - result.size() - lmax_rejected);
    OPS_ADD(ops_, physical_output_candidates, result.size());
    if (ops_) {
        const auto cost = PhysicalKernels::stateCosts(seed.size(), neighbors);
        ops_->physical_execution.record(static_cast<DagNodeId>(q), true,
                                        seed.size(), result.size(), cost.probe, cost.merge);
    }
    return result;
}

void DirectP4Evaluator::registerIndexDrivers(JIndexExecution& index) const {
    if (!window_mode_)
        throw std::invalid_argument("Direct P8 drivers require W mode");
    for (int q : plan_.order) {
        const auto& r = recipe(q);
        if (q == plan_.root || r.probes.empty()) continue;
        const auto& driver = r.probes.front();
        // q is a plain recipe identity, not a Candidate-DAG node. The index
        // persists this origin distinction in its static eligibility log.
        index.registerDriver(q, static_cast<std::uint32_t>(q), driver.anchor,
                             driver.outgoing, JIndexExecution::DriverOrigin::DirectRecipe);
    }
}

PhysicalCandidateBatch DirectP4Evaluator::physicalCandidates(
    int q, const SearchState& state, JIndexExecution& index,
    window_algebra::WindowView prior) const {
    if (!window_mode_)
        throw std::invalid_argument("Direct P8 candidate execution requires W mode");
    const auto& r = recipe(q);
    if (q == plan_.root || r.probes.empty()) return physicalCandidates(q, state);
    const int parent = binding(plan_.parent[static_cast<std::size_t>(q)], state);
    const auto* block = td_tree_.candidatesForParent(q, parent);
    const CandidateReadView original_seed = block ? CandidateReadView(*block) : CandidateReadView{};
    std::vector<CandidateReadView> neighbors;
    neighbors.reserve(r.probes.size());
    for (const auto& probe : r.probes) {
        const int anchor = binding(probe.anchor, state);
        const auto* edges = anchor < 0 || anchor >= G_.num_vertices ? nullptr :
            &(probe.outgoing ? G_.adj : G_.in_adj)[static_cast<std::size_t>(anchor)];
        neighbors.push_back(edges ? CandidateReadView(*edges) : CandidateReadView{});
    }
    const auto& driver = r.probes.front();
    auto selected = index.select(q, static_cast<std::uint32_t>(q),
        binding(driver.anchor, state), driver.outgoing, original_seed, neighbors, prior);
    // Selection's descriptor setup is P8 overhead. The fallback invokes the
    // untouched Direct+P4 kernel and increments its existing counters once.
    if (!selected.used) return physicalCandidates(q, state);
    if (selected.candidates.size() != selected.driver_entries.size() ||
        selected.candidates.size() > original_seed.size())
        throw std::logic_error("Direct P8 invalid parallel driver entries");
    const CandidateReadView seed(selected.candidates);
    const auto stride = r.slots.size();
    if (stride && seed.size() > std::numeric_limits<std::size_t>::max() / stride)
        throw std::overflow_error("Direct P8 history row size overflow");
    if (r.probes.size() + (r.self_slot != static_cast<std::size_t>(-1) ? 1 : 0) != stride)
        throw std::logic_error("Direct P8 recipe has unbound history slots");

    OPS_ADD(ops_, td_block_lookups_executed, 1);
    OPS_ADD(ops_, physical_private_descriptors, 1);
    OPS_ADD(ops_, physical_input_candidates, seed.size());
    if (ops_) ops_->physical_profile.recordOperand(
        {StructuralProfile::Backend::Direct, static_cast<DagNodeId>(q), q},
        StructuralProfile::Operand::TdBlock, original_seed.size());
    for (std::size_t j = 0; j < neighbors.size(); ++j) {
        const auto& probe = r.probes[j];
        OPS_ADD(ops_, neighbor_lookups_executed, 1);
        OPS_ADD(ops_, physical_private_descriptors, 1);
        if (ops_) ops_->physical_profile.recordOperand(
            {StructuralProfile::Backend::Direct, static_cast<DagNodeId>(q), probe.anchor},
            probe.outgoing ? StructuralProfile::Operand::OutNeighbor :
                             StructuralProfile::Operand::InNeighbor, neighbors[j].size());
    }
    auto result = PhysicalCandidateBatch::owned({});
    result.owned_candidates.reserve(seed.size());
    result.producer = static_cast<DagNodeId>(q);
    result.query_vertex = q;
    result.history_stride = stride;
    std::vector<int> row(stride, -1);
    auto bind_edge = [&](const Edge* entry, std::size_t slot_index, int candidate) {
        if (!entry || entry->to != candidate || entry->temporal_edge_id < 0 ||
            static_cast<std::size_t>(entry->temporal_edge_id) >= G_.temporal_edges.size())
            throw std::logic_error("Direct P8 invalid adjacency entry");
        const auto& edge = G_.temporal_edges[static_cast<std::size_t>(entry->temporal_edge_id)];
        const auto& slot = r.slots[slot_index];
        if (edge.u != binding(slot.from, state, q, candidate) ||
            edge.v != binding(slot.to, state, q, candidate))
            throw std::logic_error("Direct P8 bound edge disagrees with recipe");
        // Exact W intersection after injectivity absorbs the Lmax guard.
        row[slot_index] = entry->temporal_edge_id;
        OPS_ADD(ops_, physical_handle_writes, 1);
    };
    for (std::size_t i = 0; i < seed.size(); ++i) {
        const int candidate = seed[i];
        if (candidate < 0 || candidate >= G_.num_vertices ||
            (i && seed[i - 1] >= candidate))
            throw std::logic_error("Direct P8 candidates must be valid, sorted, and unique");
        // Index lookup already resolved the immutable first-driver edge.
        // Its identity is checked, but its membership is never probed twice.
        bind_edge(selected.driver_entries[i], driver.slot, candidate);
        bool accepted = true;
        for (std::size_t j = 1; j < neighbors.size(); ++j) {
            OPS_ADD(ops_, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(neighbors[j], candidate);
            if (found == neighbors[j].size() || neighbors[j][found] != candidate) {
                accepted = false; break;
            }
            bind_edge(neighbors[j].edge(found), r.probes[j].slot, candidate);
        }
        if (accepted && r.self_slot != static_cast<std::size_t>(-1)) {
            const CandidateReadView adjacency(G_.adj[static_cast<std::size_t>(candidate)]);
            OPS_ADD(ops_, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(adjacency, candidate);
            accepted = found < adjacency.size() && adjacency[found] == candidate;
            if (accepted) bind_edge(adjacency.edge(found), r.self_slot, candidate);
        }
        if (!accepted) continue;
        result.owned_candidates.push_back(candidate);
        result.temporal_edge_ids.insert(result.temporal_edge_ids.end(), row.begin(), row.end());
        OPS_ADD(ops_, physical_handle_copy_bytes, stride * sizeof(int));
    }
    if (result.temporal_edge_ids.size() != result.size() * stride)
        throw std::logic_error("Direct P8 incomplete frame-owned history row");
    OPS_ADD(ops_, physical_probe_selections, 1);
    OPS_ADD(ops_, physical_output_elements_materialized, result.size());
    OPS_ADD(ops_, physical_raw_candidates, seed.size());
    OPS_ADD(ops_, physical_structure_rejections, seed.size() - result.size());
    OPS_ADD(ops_, physical_output_candidates, result.size());
    if (ops_) {
        const auto cost = PhysicalKernels::stateCosts(original_seed.size(), neighbors);
        ops_->physical_execution.record(static_cast<DagNodeId>(q), true,
                                        seed.size(), result.size(), cost.probe, cost.merge);
    }
    return result;
}

PhysicalCandidateBatch DirectP4Evaluator::physicalCandidates(
    int q, const SearchState& state, BIndexExecution& index,
    BIndexExecution::WorkerScratch& scratch, window_algebra::WindowView prior) const {
    if (!window_mode_)
        throw std::invalid_argument("Direct B-index requires exact W mode");
    const auto& r = recipe(q);
    if (q == plan_.root) return physicalCandidates(q, state);
    const int parent_query = plan_.parent[static_cast<std::size_t>(q)];
    const int parent_data = binding(parent_query, state);
    const auto* block = td_tree_.candidatesForParent(q, parent_data);
    static const std::vector<int> empty;
    auto selected = index.select(q, parent_data, block ? *block : empty, prior, scratch);
    // Every scan branch, including sticky budget denial, executes the original
    // fixed Direct+P4 kernel exactly once. Index bookkeeping is extra work, not
    // duplicate legacy ops_* execution counters.
    if (!selected.used) return physicalCandidates(q, state);
    if (!block || selected.parent_query != parent_query ||
        selected.candidates.size() != selected.parent_entries.size() ||
        selected.candidates.size() > block->size())
        throw std::logic_error("Direct B-index invalid TD/parent selection");
    std::size_t parent_probe = r.probes.size();
    for (std::size_t j = 0; j < r.probes.size(); ++j)
        if (r.probes[j].anchor == parent_query && r.probes[j].outgoing == selected.parent_outgoing) {
            if (parent_probe != r.probes.size()) throw std::logic_error("Direct B-index duplicate parent probe");
            parent_probe = j;
        }
    if (parent_probe == r.probes.size()) throw std::logic_error("Direct B-index parent direction absent from recipe");
    // This is deliberately NOT probes.front(): reciprocal TD relations choose
    // parent -> child even when canonical P4 orders the reverse IN probe first.
    const auto& parent = r.probes[parent_probe];
    std::vector<CandidateReadView> neighbors;
    neighbors.reserve(r.probes.size());
    for (const auto& probe : r.probes) {
        const int anchor = binding(probe.anchor, state);
        const auto* adjacent = anchor < 0 || anchor >= G_.num_vertices ? nullptr :
            &(probe.outgoing ? G_.adj : G_.in_adj)[static_cast<std::size_t>(anchor)];
        neighbors.push_back(adjacent ? CandidateReadView(*adjacent) : CandidateReadView{});
    }
    const CandidateReadView seed(selected.candidates);
    const auto stride = r.slots.size();
    if (stride && seed.size() > std::numeric_limits<std::size_t>::max() / stride)
        throw std::overflow_error("Direct B-index history row size overflow");
    if (r.probes.size() + (r.self_slot != static_cast<std::size_t>(-1) ? 1 : 0) != stride)
        throw std::logic_error("Direct B-index recipe has unbound history slots");
    OPS_ADD(ops_, td_block_lookups_executed, 1);
    OPS_ADD(ops_, physical_private_descriptors, 1);
    OPS_ADD(ops_, physical_input_candidates, seed.size());
    if (ops_) ops_->physical_profile.recordOperand(
        {StructuralProfile::Backend::Direct, static_cast<DagNodeId>(q), q},
        StructuralProfile::Operand::TdBlock, block->size());
    for (std::size_t j = 0; j < neighbors.size(); ++j) {
        const auto& probe = r.probes[j];
        OPS_ADD(ops_, neighbor_lookups_executed, 1);
        OPS_ADD(ops_, physical_private_descriptors, 1);
        if (ops_) ops_->physical_profile.recordOperand(
            {StructuralProfile::Backend::Direct, static_cast<DagNodeId>(q), probe.anchor},
            probe.outgoing ? StructuralProfile::Operand::OutNeighbor :
                             StructuralProfile::Operand::InNeighbor, neighbors[j].size());
    }
    auto result = PhysicalCandidateBatch::owned({});
    result.owned_candidates.reserve(seed.size());
    result.producer = static_cast<DagNodeId>(q);
    result.query_vertex = q;
    result.history_stride = stride;
    std::vector<int> row(stride, -1);
    auto bind_edge = [&](const Edge* entry, std::size_t slot_index, int candidate) {
        if (!entry || entry->to != candidate || entry->temporal_edge_id < 0 ||
            static_cast<std::size_t>(entry->temporal_edge_id) >= G_.temporal_edges.size())
            throw std::logic_error("Direct B-index invalid parent/constraint handle");
        const auto& edge = G_.temporal_edges[static_cast<std::size_t>(entry->temporal_edge_id)];
        const auto& slot = r.slots[slot_index];
        if (edge.u != binding(slot.from, state, q, candidate) ||
            edge.v != binding(slot.to, state, q, candidate))
            throw std::logic_error("Direct B-index bound direction differs from recipe");
        // WindowAnd after injectivity is the exact temporal test. There is no
        // duplicate Lmax/history read here, and a bucket hit is not an answer.
        row[slot_index] = entry->temporal_edge_id;
        OPS_ADD(ops_, physical_handle_writes, 1);
    };
    for (std::size_t i = 0; i < seed.size(); ++i) {
        const int candidate = seed[i];
        if (candidate < 0 || candidate >= G_.num_vertices || (i && seed[i - 1] >= candidate))
            throw std::logic_error("Direct B-index candidates are not sorted unique R positions");
        // No TD membership probe and no second parent adjacency search.
        bind_edge(selected.parent_entries[i], parent.slot, candidate);
        bool accepted = true;
        for (std::size_t j = 0; j < neighbors.size(); ++j) {
            if (j == parent_probe) continue;
            OPS_ADD(ops_, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(neighbors[j], candidate);
            if (found == neighbors[j].size() || neighbors[j][found] != candidate) {
                accepted = false;
                break;
            }
            bind_edge(neighbors[j].edge(found), r.probes[j].slot, candidate);
        }
        if (accepted && r.self_slot != static_cast<std::size_t>(-1)) {
            const CandidateReadView adjacent(G_.adj[static_cast<std::size_t>(candidate)]);
            OPS_ADD(ops_, physical_structural_probes, 1);
            const auto found = PhysicalKernels::lowerBound(adjacent, candidate);
            accepted = found < adjacent.size() && adjacent[found] == candidate;
            if (accepted) bind_edge(adjacent.edge(found), r.self_slot, candidate);
        }
        if (!accepted) continue;
        result.owned_candidates.push_back(candidate);
        result.temporal_edge_ids.insert(result.temporal_edge_ids.end(), row.begin(), row.end());
        OPS_ADD(ops_, physical_handle_copy_bytes, stride * sizeof(int));
    }
    if (result.temporal_edge_ids.size() != result.size() * stride)
        throw std::logic_error("Direct B-index incomplete frame-owned history row");
    OPS_ADD(ops_, physical_probe_selections, 1);
    OPS_ADD(ops_, physical_output_elements_materialized, result.size());
    OPS_ADD(ops_, physical_raw_candidates, seed.size());
    OPS_ADD(ops_, physical_structure_rejections, seed.size() - result.size());
    OPS_ADD(ops_, physical_output_candidates, result.size());
    if (ops_) {
        const auto costs = PhysicalKernels::stateCosts(block->size(), neighbors);
        ops_->physical_execution.record(static_cast<DagNodeId>(q), true,
            seed.size(), result.size(), costs.probe, costs.merge);
    }
    return result;
}

DirectP4Evaluator::IntervalValue DirectP4Evaluator::readDurableHistory(
    const DirectedSlot& slot, std::size_t index, int q, int candidate,
    const SearchState& state, const ProbeHistoryRow* row) const {
    const int from = binding(slot.from, state, q, candidate);
    const int to = binding(slot.to, state, q, candidate);
    const TemporalEdge* edge = nullptr;
    if (row) {
        if (row->producer != static_cast<DagNodeId>(q) || row->query_vertex != q ||
            row->candidate != candidate || index >= row->size || !row->temporal_edge_ids)
            throw std::logic_error("DirectP4 missing or foreign frame row");
        const int id = row->temporal_edge_ids[index];
        if (id < 0 || static_cast<std::size_t>(id) >= G_.temporal_edges.size())
            throw std::logic_error("DirectP4 invalid bound history ID");
        edge = &G_.temporal_edges[static_cast<std::size_t>(id)];
        if (edge->u != from || edge->v != to)
            throw std::logic_error("DirectP4 history direction disagrees with recipe");
        OPS_ADD(ops_, physical_history_handle_reads, 1);
    } else {
        // Only the root's original self-loop reaches this path.
        edge = G_.findTemporalEdge(from, to);
    }
    OPS_ADD(ops_, history_lookups_executed, 1);
    if (!edge) return IntervalValue(Intervals{});
    OPS_ADD(ops_, physical_history_payload_reads, 1);
    const auto& intervals = edge->activeIntervals();
    OPS_ADD(ops_, kappa_executed, 1);
    OPS_ADD(ops_, time_input_elements, intervals.size());
    const bool all_durable = std::all_of(intervals.begin(), intervals.end(),
        [&](const TimeInterval& interval) {
            OPS_ADD(ops_, time_elements_read, 1);
            return interval.length() >= k_threshold_;
        });
    if (all_durable) return IntervalValue(&intervals);
    Intervals durable;
    durable.reserve(intervals.size());
    for (const auto& interval : intervals) {
        OPS_ADD(ops_, time_elements_read, 1);
        if (interval.length() >= k_threshold_) durable.push_back(interval);
    }
    return IntervalValue(std::move(durable));
}

TemporalSupport DirectP4Evaluator::temporalSupport(
    int q, int candidate, const SearchState& state, const ProbeHistoryRow* row) const {
    if (window_mode_)
        throw std::logic_error("DirectP4 window arm must consume exact WindowAnd, not interval support");
    const auto& r = recipe(q);
    if (r.slots.empty()) return {IntervalValue(Intervals{}), 0, false, false};
    if (q != plan_.root && !row)
        throw std::logic_error("DirectP4 nonroot history requires a frame-owned row");
    if (row && row->size != r.slots.size())
        throw std::logic_error("DirectP4 history row width differs from recipe");
    if (r.temporal_order.size() == 1) {
        const auto index = r.temporal_order.front();
        auto support = readDurableHistory(r.slots[index], index, q, candidate, state, row);
        const int longest = support->empty() ? 0 : k_threshold_;
        return {std::move(support), longest, true, false};
    }
    // P4 without P1--P3 retains P0's eager temporal leaves and canonical
    // intersection order. All lookups happen AFTER injectivity, even though
    // the structural batch and its handle rows were prepared beforehand.
    std::vector<IntervalValue> eager;
    eager.reserve(r.temporal_order.size());
    for (const auto index : r.temporal_order)
        eager.push_back(readDurableHistory(r.slots[index], index, q, candidate, state, row));
    Intervals result = *eager.front();
    for (std::size_t i = 1; i < eager.size(); ++i) {
        if (result.empty()) {
            OPS_ADD(ops_, time_intersections_skipped, eager.size() - i);
            OPS_ADD(ops_, kappa_skipped, eager.size() - i);
            break;
        }
        result = OperatorKernels::intersectIntervals(result, *eager[i], k_threshold_, false, ops_);
    }
    const int longest = result.empty() ? 0 : k_threshold_;
    return {IntervalValue(std::move(result)), longest, true, false};
}
