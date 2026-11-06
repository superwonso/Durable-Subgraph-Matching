#include "cover_aware_tdtree_builder.h"

#include "TDTree.h"
#include "query_decomposition.h"
#include "validated_graph_snapshot.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

bool addCounter(
    std::uint64_t& value,
    std::uint64_t increment) {
    if (increment >
        std::numeric_limits<std::uint64_t>::max() - value) {
        return false;
    }
    value += increment;
    return true;
}

bool capacityBytes(
    std::size_t count,
    std::size_t width,
    std::uint64_t& result) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (count > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max()) ||
            width > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    const std::uint64_t count_u64 =
        static_cast<std::uint64_t>(count);
    const std::uint64_t width_u64 =
        static_cast<std::uint64_t>(width);
    if (count_u64 != 0 &&
        width_u64 > std::numeric_limits<std::uint64_t>::max() /
                        count_u64) {
        return false;
    }
    result = count_u64 * width_u64;
    return true;
}

bool contains(
    const std::vector<int>& sorted_values,
    int value) {
    return std::binary_search(
        sorted_values.begin(), sorted_values.end(), value);
}

std::size_t nonTreeArcCount(
    const Graph& query,
    const FixedPlan& plan) {
    std::size_t count = 0;
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const bool selected_tree_pair =
                plan.parent[static_cast<std::size_t>(source)] ==
                    edge.to ||
                plan.parent[static_cast<std::size_t>(edge.to)] ==
                    source;
            if (!selected_tree_pair) ++count;
        }
    }
    return count;
}

QueryDecomposition decompositionFromPlan(
    const Graph& query,
    const FixedPlan& plan) {
    QueryDecomposition decomposition;
    const std::size_t query_vertices =
        static_cast<std::size_t>(query.num_vertices);
    decomposition.spanning_tree_adj.resize(query_vertices);
    decomposition.non_tree_edges.reserve(
        nonTreeArcCount(query, plan));
    decomposition.vertex_labels = query.vertex_labels;
    decomposition.parent = plan.parent;
    decomposition.dfs_order = plan.order;
    decomposition.temporal_selectivity.assign(query_vertices, 0.0);
    decomposition.selectivity_mode = SelectivityMode::Paper;
    decomposition.root = plan.root;
    decomposition.connected = true;

    for (int parent = 0; parent < query.num_vertices; ++parent) {
        const std::size_t child_count = static_cast<std::size_t>(
            std::count(plan.parent.begin(), plan.parent.end(), parent));
        decomposition.spanning_tree_adj[
            static_cast<std::size_t>(parent)].reserve(child_count);
    }
    for (int child = 0; child < query.num_vertices; ++child) {
        const int parent = plan.parent[static_cast<std::size_t>(child)];
        if (parent >= 0) {
            decomposition.spanning_tree_adj[
                static_cast<std::size_t>(parent)].push_back(child);
        }
    }
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const bool selected_tree_pair =
                plan.parent[static_cast<std::size_t>(source)] ==
                    edge.to ||
                plan.parent[static_cast<std::size_t>(edge.to)] ==
                    source;
            if (!selected_tree_pair) {
                decomposition.non_tree_edges.emplace_back(
                    source, edge.to);
            }
        }
    }
    return decomposition;
}

bool sameQuoteIdentity(
    const CandidateBuildQuote& quote,
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan) {
    return candidateBuildQuoteContractIsValid(quote) &&
        cover.isBoundTo(data, query) &&
        quote.graph_fingerprint ==
            temporalGraphContentFingerprint(data) &&
        quote.query_fingerprint ==
            queryGraphContentFingerprint(query) &&
        quote.graph_fingerprint == cover.graphFingerprint() &&
        quote.query_fingerprint == cover.queryFingerprint() &&
        quote.cover_fingerprint == cover.coverFingerprint() &&
        quote.plan_fingerprint == plan.fingerprint &&
        plan.query_fingerprint == quote.query_fingerprint;
}

bool sameValidatedQuoteIdentity(
    const CandidateBuildQuote& quote,
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& plan) {
    return snapshot != nullptr &&
        candidateBuildQuoteContractIsValid(quote) &&
        quote.validatedSnapshotBound() &&
        cover.hasValidatedSnapshotAuthority() &&
        quote.isBoundToValidatedSnapshot(snapshot) &&
        cover.isBoundToValidatedSnapshot(snapshot) &&
        quote.validation_snapshot_token == snapshot->snapshotToken() &&
        cover.snapshotToken() == snapshot->snapshotToken() &&
        quote.graph_fingerprint == snapshot->dataFingerprint() &&
        quote.query_fingerprint == snapshot->queryFingerprint() &&
        quote.graph_fingerprint == cover.graphFingerprint() &&
        quote.query_fingerprint == cover.queryFingerprint() &&
        quote.cover_fingerprint == cover.coverFingerprint() &&
        quote.plan_fingerprint == plan.fingerprint &&
        plan.query_fingerprint == quote.query_fingerprint &&
        quote.graph_validation_rescans == 0 &&
        quote.data_fingerprint_rescans == 0 &&
        quote.query_fingerprint_rescans == 0;
}

CoverAwareBuildResult failed(
    CoverAwareBuildStatus status,
    CandidateBuildCounters counters = {}) {
    CoverAwareBuildResult result;
    result.status = status;
    result.counters = counters;
    result.tree.reset();
    return result;
}

bool withinQuote(
    const CandidateBuildCounters& counters,
    const CandidateBuildQuote& quote) {
    return counters.adjacency_probes <= quote.adjacency_probes &&
        counters.reverse_edge_lookups <=
            quote.reverse_edge_lookups &&
        counters.arc_checks <= quote.maximum_arc_checks &&
        counters.root_self_loop_checks <=
            quote.maximum_root_self_loop_checks &&
        counters.relation_entries <=
            quote.maximum_relation_entries &&
        counters.candidate_value_occurrences <=
            quote.maximum_candidate_value_occurrences &&
        counters.nonempty_blocks <= quote.maximum_blocks &&
        counters.semijoin_entry_visits <=
            quote.maximum_semijoin_entry_visits &&
        counters.index_entry_visits <=
            quote.maximum_index_entries &&
        counters.peak_payload_capacity_bytes <=
            quote.maximum_payload_bytes &&
        counters.final_payload_capacity_bytes <=
            quote.maximum_payload_bytes;
}

const TemporalEdge* edgeFromReference(
    const Graph& data,
    const Edge& edge) {
    if (edge.temporal_edge_id < 0 ||
        static_cast<std::size_t>(edge.temporal_edge_id) >=
            data.temporal_edges.size()) {
        return nullptr;
    }
    return &data.temporal_edges[
        static_cast<std::size_t>(edge.temporal_edge_id)];
}

} // namespace

const char* coverAwareBuildStatusName(
    CoverAwareBuildStatus status) {
    switch (status) {
        case CoverAwareBuildStatus::Complete:
            return "complete";
        case CoverAwareBuildStatus::MissingReservation:
            return "missing-reservation";
        case CoverAwareBuildStatus::InactiveReservation:
            return "inactive-reservation";
        case CoverAwareBuildStatus::QuoteContractMismatch:
            return "quote-contract-mismatch";
        case CoverAwareBuildStatus::SourceIdentityMismatch:
            return "source-identity-mismatch";
        case CoverAwareBuildStatus::FixedPlanMismatch:
            return "fixed-plan-mismatch";
        case CoverAwareBuildStatus::CapacityContractViolation:
            return "capacity-contract-violation";
        case CoverAwareBuildStatus::QuoteViolation:
            return "quote-violation";
        case CoverAwareBuildStatus::AllocationFailure:
            return "allocation-failure";
        case CoverAwareBuildStatus::InjectedFailure:
            return "injected-failure";
        case CoverAwareBuildStatus::InternalFailure:
            return "internal-failure";
    }
    return "unknown";
}

bool CoverAwareBuildResult::complete() const noexcept {
    return status == CoverAwareBuildStatus::Complete && tree != nullptr;
}

class CoverAwareTDTreeBuilder {
public:
    CoverAwareTDTreeBuilder(
        std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
        const Graph& data,
        const Graph& query,
        const CandidateCover& cover,
        const FixedPlan& plan,
        CandidateBuildReservation& reservation,
        CoverAwareBuildFailurePoint failure_point)
        : snapshot_(std::move(snapshot)),
          data_(data),
          query_(query),
          cover_(cover),
          plan_(plan),
          reservation_(reservation),
          quote_(reservation.quote()),
          failure_point_(failure_point) {
        counters_.reserved_payload_bytes =
            quote_.maximum_payload_bytes;
    }

    CoverAwareBuildResult run() {
        QueryDecomposition decomposition =
            decompositionFromPlan(query_, plan_);
        const bool topology_is_canonical = snapshot_
            ? queryDecompositionTopologyIsCanonical(
                  *snapshot_, decomposition)
            : queryDecompositionTopologyIsCanonical(
                  query_, decomposition);
        if (!topology_is_canonical) {
            return failed(
                CoverAwareBuildStatus::FixedPlanMismatch,
                counters_);
        }

        std::unique_ptr<TDTree> tree;
        if (snapshot_) {
            tree.reset(new TDTree(
                snapshot_,
                std::move(decomposition),
                cover_.minimumDuration(),
                true));
        } else {
            tree.reset(new TDTree(
                data_, query_, std::move(decomposition),
                cover_.minimumDuration(),
                TDTree::CoverAwareConstructionTag{}));
        }

        if (!initialize(*tree) || !measurePayload(*tree)) {
            return capacityFailure();
        }
        if (!fillRoot(*tree)) return quoteFailure();
        if (!measurePayload(*tree)) return capacityFailure();
        if (failure_point_ ==
            CoverAwareBuildFailurePoint::AllocationAfterRoot) {
            throw std::bad_alloc();
        }
        if (failure_point_ == CoverAwareBuildFailurePoint::AfterRoot) {
            return failed(
                CoverAwareBuildStatus::InjectedFailure,
                counters_);
        }

        if (!buildRelations(*tree)) return quoteFailure();
        if (!measurePayload(*tree)) return capacityFailure();
        if (failure_point_ ==
            CoverAwareBuildFailurePoint::AfterRelations) {
            return failed(
                CoverAwareBuildStatus::InjectedFailure,
                counters_);
        }

        if (!trimBottomUp(*tree)) return quoteFailure();
        if (failure_point_ ==
            CoverAwareBuildFailurePoint::AfterBottomUp) {
            return failed(
                CoverAwareBuildStatus::InjectedFailure,
                counters_);
        }
        if (!trimTopDown(*tree)) return quoteFailure();
        if (failure_point_ ==
            CoverAwareBuildFailurePoint::AfterTopDown) {
            return failed(
                CoverAwareBuildStatus::InjectedFailure,
                counters_);
        }

        removeEmptyBlocks(*tree);
        if (!finalize(*tree) || !measurePayload(*tree)) {
            return capacityFailure();
        }
        counters_.final_payload_capacity_bytes =
            tree->flatPayloadCapacityBytes();
        if (!withinQuote(counters_, quote_)) {
            ++counters_.quote_violations;
            return failed(
                CoverAwareBuildStatus::QuoteViolation,
                counters_);
        }

        reservation_.commit();
        CoverAwareBuildResult result;
        result.status = CoverAwareBuildStatus::Complete;
        result.counters = counters_;
        result.tree = std::move(tree);
        return result;
    }

private:
    bool initialize(TDTree& tree) {
        const std::size_t query_vertices =
            static_cast<std::size_t>(query_.num_vertices);
        tree.nodes.reserve(query_vertices);
        if (tree.nodes.capacity() != query_vertices) return false;
        tree.nodes.resize(query_vertices);

        tree.query_out_neighbor_label_requirements.reserve(
            query_vertices);
        if (tree.query_out_neighbor_label_requirements.capacity() !=
            query_vertices) {
            return false;
        }
        tree.query_out_neighbor_label_requirements.resize(
            query_vertices);
        tree.query_in_neighbor_label_requirements.reserve(
            query_vertices);
        if (tree.query_in_neighbor_label_requirements.capacity() !=
            query_vertices) {
            return false;
        }
        tree.query_in_neighbor_label_requirements.resize(
            query_vertices);

        for (int query_vertex = 0;
             query_vertex < query_.num_vertices;
             ++query_vertex) {
            TDTreeNode& node = tree.nodes[
                static_cast<std::size_t>(query_vertex)];
            node.query_vertex_id = query_vertex;
            node.isRoot = query_vertex == plan_.root;
            node.isLeaf = tree.QD.spanning_tree_adj[
                static_cast<std::size_t>(query_vertex)].empty();
        }
        return tree.QD.spanning_tree_adj.capacity() == query_vertices &&
            tree.QD.vertex_labels.capacity() == query_vertices &&
            tree.QD.parent.capacity() == query_vertices &&
            tree.QD.dfs_order.capacity() == query_vertices &&
            tree.QD.temporal_selectivity.capacity() == query_vertices &&
            tree.QD.non_tree_edges.capacity() ==
                quote_.maximum_non_tree_edges &&
            treeChildrenHaveExactCapacity(tree);
    }

    bool fillRoot(TDTree& tree) {
        TDTreeNode& root =
            tree.nodes[static_cast<std::size_t>(plan_.root)];
        const auto& root_cover =
            cover_.domain(static_cast<std::size_t>(plan_.root));
        root.root_candidates.reserve(root_cover.size());
        if (root.root_candidates.capacity() != root_cover.size()) {
            return false;
        }

        const bool requires_self_loop = GraphUtils::hasEdge(
            query_.adj, plan_.root, plan_.root);
        for (int candidate : root_cover) {
            bool keep = true;
            if (requires_self_loop) {
                if (!addCounter(counters_.root_self_loop_checks, 1) ||
                    !addCounter(counters_.arc_checks, 1)) {
                    return false;
                }
                const TemporalEdge* self =
                    data_.findTemporalEdge(candidate, candidate);
                keep = self != nullptr &&
                    hasMinimumConsecutiveDuration(
                        *self, cover_.minimumDuration());
            }
            if (keep) root.root_candidates.push_back(candidate);
        }
        counters_.candidate_value_occurrences =
            root.root_candidates.size();
        return counters_.root_self_loop_checks <=
                quote_.maximum_root_self_loop_checks &&
            counters_.arc_checks <= quote_.maximum_arc_checks &&
            counters_.candidate_value_occurrences <=
                quote_.maximum_candidate_value_occurrences;
    }

    bool buildRelations(TDTree& tree) {
        for (std::size_t depth = 1;
             depth < plan_.order.size();
             ++depth) {
            const int child = plan_.order[depth];
            const int parent =
                plan_.parent[static_cast<std::size_t>(child)];
            const RequiredArcMask mask =
                requiredArcMask(query_, parent, child);
            const auto& parent_cover =
                cover_.domain(static_cast<std::size_t>(parent));
            const auto& child_cover =
                cover_.domain(static_cast<std::size_t>(child));
            TDTreeNode& node =
                tree.nodes[static_cast<std::size_t>(child)];
            node.blocks.reserve(parent_cover.size());
            if (node.blocks.capacity() != parent_cover.size()) {
                return false;
            }

            for (int parent_data : parent_cover) {
                const auto& outgoing =
                    data_.adj[static_cast<std::size_t>(parent_data)];
                const auto& incoming =
                    data_.in_adj[static_cast<std::size_t>(parent_data)];
                const std::vector<Edge>* scan = nullptr;
                bool scanned_outgoing = false;
                if (mask == RequiredArcMask::ParentToChild) {
                    scan = &outgoing;
                    scanned_outgoing = true;
                } else if (mask == RequiredArcMask::ChildToParent) {
                    scan = &incoming;
                } else if (outgoing.size() <= incoming.size()) {
                    scan = &outgoing;
                    scanned_outgoing = true;
                } else {
                    scan = &incoming;
                }

                TDTreeBlock block;
                block.v_par = parent_data;
                block.V_cand.reserve(scan->size());
                if (block.V_cand.capacity() != scan->size()) {
                    return false;
                }
                std::uint64_t temporary_relation_bytes = 0;
                if (!capacityBytes(
                        block.V_cand.capacity(),
                        sizeof(int),
                        temporary_relation_bytes) ||
                    !measurePayload(
                        tree, temporary_relation_bytes)) {
                    return false;
                }
                for (const Edge& edge_ref : *scan) {
                    if (!addCounter(counters_.adjacency_probes, 1) ||
                        !addCounter(counters_.arc_checks, 1)) {
                        return false;
                    }
                    const int candidate = edge_ref.to;
                    if (!contains(child_cover, candidate)) continue;

                    const TemporalEdge* scanned_edge =
                        edgeFromReference(data_, edge_ref);
                    bool durable = scanned_edge != nullptr &&
                        hasMinimumConsecutiveDuration(
                            *scanned_edge,
                            cover_.minimumDuration());
                    if (mask == RequiredArcMask::Reciprocal) {
                        if (!addCounter(
                                counters_.reverse_edge_lookups, 1) ||
                            !addCounter(counters_.arc_checks, 1)) {
                            return false;
                        }
                        const TemporalEdge* other = scanned_outgoing
                            ? data_.findTemporalEdge(candidate, parent_data)
                            : data_.findTemporalEdge(parent_data, candidate);
                        durable = durable && other != nullptr &&
                            hasMinimumConsecutiveDuration(
                                *other,
                                cover_.minimumDuration());
                    }
                    if (!durable) continue;
                    block.V_cand.push_back(candidate);
                    if (!addCounter(counters_.relation_entries, 1) ||
                        !addCounter(
                            counters_.candidate_value_occurrences, 1)) {
                        return false;
                    }
                }
                if (!block.V_cand.empty()) {
                    node.blocks.push_back(std::move(block));
                    if (!addCounter(counters_.nonempty_blocks, 1)) {
                        return false;
                    }
                }
            }
            if (!addCounter(counters_.selected_tree_arcs, 1)) {
                return false;
            }
        }
        return counters_.adjacency_probes <= quote_.adjacency_probes &&
            counters_.reverse_edge_lookups <=
                quote_.reverse_edge_lookups &&
            counters_.arc_checks <= quote_.maximum_arc_checks &&
            counters_.relation_entries <=
                quote_.maximum_relation_entries &&
            counters_.candidate_value_occurrences <=
                quote_.maximum_candidate_value_occurrences &&
            counters_.nonempty_blocks <= quote_.maximum_blocks &&
            counters_.selected_tree_arcs + 1 ==
                static_cast<std::uint64_t>(query_.num_vertices) &&
            counters_.non_tree_prefilter_checks == 0;
    }

    bool trimBottomUp(TDTree& tree) {
        const std::size_t data_vertices =
            static_cast<std::size_t>(data_.num_vertices);
        std::vector<std::uint64_t> support_count;
        std::vector<std::uint64_t> candidate_marks;
        support_count.reserve(data_vertices);
        candidate_marks.reserve(data_vertices);
        if (support_count.capacity() != data_vertices ||
            candidate_marks.capacity() != data_vertices) {
            return false;
        }
        support_count.resize(data_vertices, 0);
        candidate_marks.resize(data_vertices, 0);
        std::uint64_t marker_bytes = 0;
        if (!capacityBytes(
                data_vertices,
                2 * sizeof(std::uint64_t),
                marker_bytes) ||
            !measurePayload(tree, marker_bytes)) {
            return false;
        }
        std::uint64_t generation = 0;

        for (auto it = plan_.order.rbegin();
             it != plan_.order.rend();
             ++it) {
            const int query_vertex = *it;
            const auto& children = tree.QD.spanning_tree_adj[
                static_cast<std::size_t>(query_vertex)];
            if (children.empty()) continue;

            for (int child : children) {
                for (const TDTreeBlock& block :
                     tree.nodes[static_cast<std::size_t>(child)].blocks) {
                    if (!addCounter(
                            counters_.semijoin_entry_visits, 1)) {
                        return false;
                    }
                    if (!block.V_cand.empty()) {
                        ++support_count[
                            static_cast<std::size_t>(block.v_par)];
                    }
                }
            }
            const std::uint64_t required = children.size();
            auto supported = [&](int candidate) {
                return support_count[
                    static_cast<std::size_t>(candidate)] == required;
            };
            TDTreeNode& node = tree.nodes[
                static_cast<std::size_t>(query_vertex)];
            if (node.isRoot) {
                for (std::size_t index = 0;
                     index < node.root_candidates.size();
                     ++index) {
                    if (!addCounter(
                            counters_.semijoin_entry_visits, 1)) {
                        return false;
                    }
                }
                node.root_candidates.erase(
                    std::remove_if(
                        node.root_candidates.begin(),
                        node.root_candidates.end(),
                        [&](int candidate) {
                            return !supported(candidate);
                        }),
                    node.root_candidates.end());
            } else {
                for (TDTreeBlock& block : node.blocks) {
                    for (std::size_t index = 0;
                         index < block.V_cand.size();
                         ++index) {
                        if (!addCounter(
                                counters_.semijoin_entry_visits, 1)) {
                            return false;
                        }
                    }
                    block.V_cand.erase(
                        std::remove_if(
                            block.V_cand.begin(),
                            block.V_cand.end(),
                            [&](int candidate) {
                                return !supported(candidate);
                            }),
                        block.V_cand.end());
                }
            }

            if (++generation == 0) return false;
            for (int child : children) {
                for (const TDTreeBlock& block :
                     tree.nodes[static_cast<std::size_t>(child)].blocks) {
                    if (candidate_marks[
                            static_cast<std::size_t>(block.v_par)] !=
                        generation) {
                        candidate_marks[
                            static_cast<std::size_t>(block.v_par)] =
                            generation;
                        support_count[
                            static_cast<std::size_t>(block.v_par)] = 0;
                    }
                }
            }
        }
        return counters_.semijoin_entry_visits <=
            quote_.maximum_semijoin_entry_visits;
    }

    bool trimTopDown(TDTree& tree) {
        const std::size_t data_vertices =
            static_cast<std::size_t>(data_.num_vertices);
        std::vector<std::uint64_t> marks;
        marks.reserve(data_vertices);
        if (marks.capacity() != data_vertices) return false;
        marks.resize(data_vertices, 0);
        std::uint64_t marker_bytes = 0;
        if (!capacityBytes(
                data_vertices,
                sizeof(std::uint64_t),
                marker_bytes) ||
            !measurePayload(tree, marker_bytes)) {
            return false;
        }
        std::uint64_t generation = 0;
        for (int query_vertex : plan_.order) {
            if (++generation == 0) return false;
            const TDTreeNode& node = tree.nodes[
                static_cast<std::size_t>(query_vertex)];
            if (node.isRoot) {
                for (int candidate : node.root_candidates) {
                    if (!addCounter(
                            counters_.semijoin_entry_visits, 1)) {
                        return false;
                    }
                    marks[static_cast<std::size_t>(candidate)] =
                        generation;
                }
            } else {
                for (const TDTreeBlock& block : node.blocks) {
                    for (int candidate : block.V_cand) {
                        if (!addCounter(
                                counters_.semijoin_entry_visits, 1)) {
                            return false;
                        }
                        marks[static_cast<std::size_t>(candidate)] =
                            generation;
                    }
                }
            }
            for (int child : tree.QD.spanning_tree_adj[
                     static_cast<std::size_t>(query_vertex)]) {
                TDTreeNode& child_node = tree.nodes[
                    static_cast<std::size_t>(child)];
                for (std::size_t index = 0;
                     index < child_node.blocks.size();
                     ++index) {
                    if (!addCounter(
                            counters_.semijoin_entry_visits, 1)) {
                        return false;
                    }
                }
                child_node.blocks.erase(
                    std::remove_if(
                        child_node.blocks.begin(),
                        child_node.blocks.end(),
                        [&](const TDTreeBlock& block) {
                            return marks[static_cast<std::size_t>(
                                block.v_par)] != generation;
                        }),
                    child_node.blocks.end());
            }
        }
        return counters_.semijoin_entry_visits <=
            quote_.maximum_semijoin_entry_visits;
    }

    void removeEmptyBlocks(TDTree& tree) {
        for (TDTreeNode& node : tree.nodes) {
            node.blocks.erase(
                std::remove_if(
                    node.blocks.begin(), node.blocks.end(),
                    [](const TDTreeBlock& block) {
                        return block.V_cand.empty();
                    }),
                node.blocks.end());
        }
    }

    bool finalize(TDTree& tree) {
        std::uint64_t final_relation_entries = 0;
        std::uint64_t final_candidate_value_occurrences =
            tree.rootCandidates().size();
        std::uint64_t final_nonempty_blocks = 0;
        for (const TDTreeNode& node : tree.nodes) {
            if (!node.isRoot) {
                for (const TDTreeBlock& block : node.blocks) {
                    if (block.V_cand.empty()) return false;
                    if (!addCounter(final_nonempty_blocks, 1) ||
                        !addCounter(
                            final_relation_entries,
                            block.V_cand.size()) ||
                        !addCounter(
                            final_candidate_value_occurrences,
                            block.V_cand.size()) ||
                        !addCounter(
                            counters_.index_entry_visits, 1)) {
                        return false;
                    }
                }
            }
        }
        return final_relation_entries <= counters_.relation_entries &&
            final_candidate_value_occurrences <=
                counters_.candidate_value_occurrences &&
            final_nonempty_blocks <= counters_.nonempty_blocks &&
            tree.hasOnlySortedFlatBlocks() &&
            tree.executionPlanFingerprint() == plan_.fingerprint;
    }

    bool treeChildrenHaveExactCapacity(const TDTree& tree) const {
        for (int parent = 0; parent < query_.num_vertices; ++parent) {
            const auto& children = tree.QD.spanning_tree_adj[
                static_cast<std::size_t>(parent)];
            if (children.capacity() != children.size()) return false;
        }
        return true;
    }

    bool measurePayload(
        const TDTree& tree,
        std::uint64_t temporary_bytes = 0) {
        const std::uint64_t tree_bytes =
            tree.flatPayloadCapacityBytes();
        if (temporary_bytes >
            std::numeric_limits<std::uint64_t>::max() - tree_bytes) {
            return false;
        }
        const std::uint64_t bytes = tree_bytes + temporary_bytes;
        counters_.peak_payload_capacity_bytes = std::max(
            counters_.peak_payload_capacity_bytes, bytes);
        return bytes <= quote_.maximum_payload_bytes;
    }

    CoverAwareBuildResult quoteFailure() {
        ++counters_.quote_violations;
        return failed(
            CoverAwareBuildStatus::QuoteViolation,
            counters_);
    }

    CoverAwareBuildResult capacityFailure() {
        ++counters_.quote_violations;
        return failed(
            CoverAwareBuildStatus::CapacityContractViolation,
            counters_);
    }

    std::shared_ptr<const ValidatedGraphSnapshot> snapshot_;
    const Graph& data_;
    const Graph& query_;
    const CandidateCover& cover_;
    const FixedPlan& plan_;
    CandidateBuildReservation& reservation_;
    const CandidateBuildQuote& quote_;
    CoverAwareBuildFailurePoint failure_point_;
    CandidateBuildCounters counters_;
};

namespace {

CoverAwareBuildResult buildImpl(
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation,
    CoverAwareBuildFailurePoint failure_point) {
    if (!reservation) {
        return failed(CoverAwareBuildStatus::MissingReservation);
    }
    if (!reservation->active()) {
        return failed(CoverAwareBuildStatus::InactiveReservation);
    }
    if (!candidateBuildQuoteContractIsValid(reservation->quote())) {
        return failed(CoverAwareBuildStatus::QuoteContractMismatch);
    }
    try {
        if (failure_point ==
            CoverAwareBuildFailurePoint::AllocationBeforeValidation) {
            throw std::bad_alloc();
        }
        const FixedPlanValidation validation = snapshot
            ? validateFixedPlan(*snapshot, plan)
            : validateFixedPlan(query, plan);
        if (!validation.valid ||
            plan.fingerprint == cover.incumbentPlanFingerprint()) {
            return failed(CoverAwareBuildStatus::FixedPlanMismatch);
        }
        const bool identity_matches = snapshot
            ? sameValidatedQuoteIdentity(
                  reservation->quote(), snapshot, cover, plan)
            : sameQuoteIdentity(
                  reservation->quote(), data, query, cover, plan);
        if (!identity_matches) {
            return failed(CoverAwareBuildStatus::SourceIdentityMismatch);
        }
        CoverAwareTDTreeBuilder builder(
            std::move(snapshot),
            data,
            query,
            cover,
            plan,
            *reservation,
            failure_point);
        return builder.run();
    } catch (const std::bad_alloc&) {
        return failed(CoverAwareBuildStatus::AllocationFailure);
    } catch (...) {
        return failed(CoverAwareBuildStatus::InternalFailure);
    }
}

} // namespace

CoverAwareBuildResult buildCoverAwareTDTree(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation) {
    return buildImpl(
        nullptr,
        data,
        query,
        cover,
        plan,
        std::move(reservation),
        CoverAwareBuildFailurePoint::None);
}

CoverAwareBuildResult buildCoverAwareTDTree(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation) {
    if (!snapshot) {
        return failed(CoverAwareBuildStatus::SourceIdentityMismatch);
    }
    return buildImpl(
        snapshot,
        snapshot->data(),
        snapshot->query(),
        cover,
        plan,
        std::move(reservation),
        CoverAwareBuildFailurePoint::None);
}

#ifdef DSM_TCC_V3_TESTING
CoverAwareBuildResult buildCoverAwareTDTreeForTesting(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    std::unique_ptr<CandidateBuildReservation> reservation,
    CoverAwareBuildFailurePoint failure_point) {
    return buildImpl(
        nullptr,
        data,
        query,
        cover,
        plan,
        std::move(reservation),
        failure_point);
}
#endif
