#include "fixed_plan_feasibility.h"

#include "TDTree.h"
#include "query_decomposition.h"
#include "v3_existing_capacity.h"
#include "validated_graph_snapshot.h"

#include <algorithm>
#include <atomic>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {

#ifdef DSM_TCC_V3_TESTING
std::atomic<std::uint64_t> g_query_fingerprint_calls{0};
std::atomic<bool> g_candidate_reservation_fail_next{false};
#endif

constexpr std::uint64_t kFnvOffset =
    UINT64_C(14695981039346656037);
constexpr std::uint64_t kFnvPrime =
    UINT64_C(1099511628211);
constexpr std::uint64_t kCandidatePayloadModelId =
    UINT64_C(0x464c41542d563301);
constexpr const char* kCandidatePayloadModel =
    "flat-sorted-exact-reserve-v1";

void appendU64(std::uint64_t& hash, std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & UINT64_C(0xff);
        hash *= kFnvPrime;
    }
}

void appendInt(std::uint64_t& hash, int value) {
    appendU64(
        hash,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(value)));
}

bool checkedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool checkedAccumulate(
    std::uint64_t& destination,
    std::uint64_t value) {
    std::uint64_t result = 0;
    if (!checkedAdd(destination, value, result)) return false;
    destination = result;
    return true;
}

bool checkedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) {
    if (lhs != 0 &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool sizeToU64(std::size_t value, std::uint64_t& result) {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    result = static_cast<std::uint64_t>(value);
    return true;
}

bool isStrictlySortedUnique(const std::vector<int>& values) {
    return std::adjacent_find(
        values.begin(),
        values.end(),
        std::greater_equal<int>()) == values.end();
}

bool isStrictlySortedUnique(const std::vector<Edge>& edges) {
    for (std::size_t i = 1; i < edges.size(); ++i) {
        if (edges[i - 1].to >= edges[i].to) return false;
    }
    return true;
}

bool temporalHistoryIsCanonicalAndCounted(
    const TemporalEdge& edge) {
    if (edge.active_snapshot_count < 0) return false;
    std::int64_t exact_count = 0;
    int previous_end = 0;
    bool have_previous = false;
    for (const TimeInterval& interval : edge.activeIntervals()) {
        if (interval.end < interval.start) return false;
        if (have_previous &&
            (interval.start <= previous_end ||
             (previous_end != std::numeric_limits<int>::max() &&
              interval.start == previous_end + 1))) {
            return false;
        }
        const std::int64_t span =
            static_cast<std::int64_t>(interval.end) -
            static_cast<std::int64_t>(interval.start) + 1;
        if (span > std::numeric_limits<std::int64_t>::max() -
                       exact_count) {
            return false;
        }
        exact_count += span;
        previous_end = interval.end;
        have_previous = true;
    }
    return exact_count == edge.active_snapshot_count;
}

bool queryTopologyIsValid(const Graph& query) {
    if (query.num_vertices <= 0) return false;
    const std::size_t vertex_count =
        static_cast<std::size_t>(query.num_vertices);
    if (query.vertex_labels.size() != vertex_count ||
        query.adj.size() != vertex_count ||
        query.in_adj.size() != vertex_count) {
        return false;
    }
    if (std::any_of(
            query.vertex_labels.begin(),
            query.vertex_labels.end(),
            [](Label label) { return label >= kLabelCount; })) {
        return false;
    }

    for (int source = 0; source < query.num_vertices; ++source) {
        const auto& outgoing =
            query.adj[static_cast<std::size_t>(source)];
        const auto& incoming =
            query.in_adj[static_cast<std::size_t>(source)];
        if (!isStrictlySortedUnique(outgoing) ||
            !isStrictlySortedUnique(incoming)) {
            return false;
        }
        for (const Edge& edge : outgoing) {
            if (edge.to < 0 || edge.to >= query.num_vertices ||
                !GraphUtils::hasEdge(query.in_adj, edge.to, source)) {
                return false;
            }
        }
        for (const Edge& edge : incoming) {
            if (edge.to < 0 || edge.to >= query.num_vertices ||
                !GraphUtils::hasEdge(query.adj, edge.to, source)) {
                return false;
            }
        }
    }
    return true;
}

bool dataGraphIsValidForQuote(const Graph& data) {
    if (data.num_vertices < 0) return false;
    const std::size_t vertex_count =
        static_cast<std::size_t>(data.num_vertices);
    if (data.external_ids.size() != vertex_count ||
        data.vertex_labels.size() != vertex_count ||
        data.adj.size() != vertex_count ||
        data.in_adj.size() != vertex_count) {
        return false;
    }
    if (std::any_of(
            data.vertex_labels.begin(),
            data.vertex_labels.end(),
            [](Label label) { return label >= kLabelCount; })) {
        return false;
    }

    std::vector<int> external_ids = data.external_ids;
    std::sort(external_ids.begin(), external_ids.end());
    if (std::adjacent_find(
            external_ids.begin(), external_ids.end()) !=
        external_ids.end()) {
        return false;
    }

    std::vector<std::uint8_t> outgoing_references(
        data.temporal_edges.size(), 0);
    std::vector<std::uint8_t> incoming_references(
        data.temporal_edges.size(), 0);
    for (int vertex = 0; vertex < data.num_vertices; ++vertex) {
        const auto& outgoing =
            data.adj[static_cast<std::size_t>(vertex)];
        const auto& incoming =
            data.in_adj[static_cast<std::size_t>(vertex)];
        if (!isStrictlySortedUnique(outgoing) ||
            !isStrictlySortedUnique(incoming)) {
            return false;
        }
        for (const Edge& edge : outgoing) {
            if (edge.to < 0 || edge.to >= data.num_vertices ||
                edge.temporal_edge_id < 0 ||
                static_cast<std::size_t>(edge.temporal_edge_id) >=
                    data.temporal_edges.size()) {
                return false;
            }
            const TemporalEdge& temporal_edge =
                data.temporal_edges[
                    static_cast<std::size_t>(edge.temporal_edge_id)];
            if (temporal_edge.u != vertex ||
                temporal_edge.v != edge.to ||
                !temporal_edge.hasCachedMaximumConsecutiveRun() ||
                temporal_edge.maximumConsecutiveRun() !=
                    ::maximumConsecutiveRun(
                        temporal_edge.activeIntervals()) ||
                !temporalHistoryIsCanonicalAndCounted(
                    temporal_edge)) {
                return false;
            }
            auto& outgoing_count = outgoing_references[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            if (outgoing_count ==
                std::numeric_limits<std::uint8_t>::max()) {
                return false;
            }
            ++outgoing_count;
            const auto& reciprocal_index =
                data.in_adj[static_cast<std::size_t>(edge.to)];
            const auto found = std::lower_bound(
                reciprocal_index.begin(),
                reciprocal_index.end(),
                vertex,
                [](const Edge& candidate, int target) {
                    return candidate.to < target;
                });
            if (found == reciprocal_index.end() ||
                found->to != vertex ||
                found->temporal_edge_id != edge.temporal_edge_id) {
                return false;
            }
        }
        for (const Edge& edge : incoming) {
            if (edge.to < 0 || edge.to >= data.num_vertices ||
                edge.temporal_edge_id < 0 ||
                static_cast<std::size_t>(edge.temporal_edge_id) >=
                    data.temporal_edges.size()) {
                return false;
            }
            const TemporalEdge& temporal_edge =
                data.temporal_edges[
                    static_cast<std::size_t>(edge.temporal_edge_id)];
            if (temporal_edge.u != edge.to ||
                temporal_edge.v != vertex) {
                return false;
            }
            const auto& reciprocal_index =
                data.adj[static_cast<std::size_t>(edge.to)];
            const auto found = std::lower_bound(
                reciprocal_index.begin(),
                reciprocal_index.end(),
                vertex,
                [](const Edge& candidate, int target) {
                    return candidate.to < target;
                });
            if (found == reciprocal_index.end() ||
                found->to != vertex ||
                found->temporal_edge_id != edge.temporal_edge_id) {
                return false;
            }
            auto& incoming_count = incoming_references[
                static_cast<std::size_t>(edge.temporal_edge_id)];
            if (incoming_count ==
                std::numeric_limits<std::uint8_t>::max()) {
                return false;
            }
            ++incoming_count;
        }
    }
    for (std::size_t edge_id = 0;
         edge_id < data.temporal_edges.size();
         ++edge_id) {
        if (outgoing_references[edge_id] != 1 ||
            incoming_references[edge_id] != 1) {
            return false;
        }
    }
    return true;
}

bool accountingIsValid(
    const CandidateBuildAccounting& accounting) {
    const CandidateBuildAccounting minimum =
        defaultCandidateBuildAccounting();
    return accounting.tree_object_payload_bytes >=
            minimum.tree_object_payload_bytes &&
        accounting.node_payload_bytes >=
            minimum.node_payload_bytes &&
        accounting.relation_entry_bytes >=
            minimum.relation_entry_bytes &&
        accounting.block_payload_bytes >=
            minimum.block_payload_bytes &&
        accounting.plan_vertex_payload_bytes >=
            minimum.plan_vertex_payload_bytes &&
        accounting.tree_child_entry_bytes >=
            minimum.tree_child_entry_bytes &&
        accounting.non_tree_edge_payload_bytes >=
            minimum.non_tree_edge_payload_bytes &&
        accounting.data_vertex_marker_bytes >=
            minimum.data_vertex_marker_bytes &&
        accounting.semijoin_visits_per_candidate_value >=
            minimum.semijoin_visits_per_candidate_value &&
        accounting.semijoin_visits_per_block >=
            minimum.semijoin_visits_per_block &&
        accounting.index_entries_per_block >=
            minimum.index_entries_per_block;
}

std::uint64_t candidateBuildAccountingFingerprint(
    const CandidateBuildAccounting& accounting) {
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x414343542d563301));
    appendU64(hash, kCandidatePayloadModelId);
    appendU64(hash, accounting.tree_object_payload_bytes);
    appendU64(hash, accounting.node_payload_bytes);
    appendU64(hash, accounting.relation_entry_bytes);
    appendU64(hash, accounting.block_payload_bytes);
    appendU64(hash, accounting.plan_vertex_payload_bytes);
    appendU64(hash, accounting.tree_child_entry_bytes);
    appendU64(hash, accounting.non_tree_edge_payload_bytes);
    appendU64(hash, accounting.data_vertex_marker_bytes);
    appendU64(hash, accounting.semijoin_visits_per_candidate_value);
    appendU64(hash, accounting.semijoin_visits_per_block);
    appendU64(hash, accounting.index_entries_per_block);
    return hash;
}

std::uint64_t candidateBuildContractFingerprint(
    const CandidateBuildQuote& quote,
    std::uint64_t accounting_fingerprint) {
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x4255494c442d5633));
    appendU64(hash, kCandidatePayloadModelId);
    appendU64(hash, accounting_fingerprint);
    appendU64(hash, static_cast<std::uint64_t>(quote.complete));
    appendU64(hash, static_cast<std::uint64_t>(quote.overflowed));
    appendU64(hash, static_cast<std::uint64_t>(quote.status));
    appendU64(hash, static_cast<std::uint64_t>(quote.plan_validation.valid));
    appendU64(
        hash,
        static_cast<std::uint64_t>(quote.plan_validation.status));
    appendInt(hash, quote.plan_validation.query_vertex);
    appendU64(hash, quote.graph_fingerprint);
    appendU64(hash, quote.query_fingerprint);
    appendU64(hash, quote.cover_fingerprint);
    appendU64(hash, quote.plan_fingerprint);
    appendU64(hash, quote.validation_snapshot_token);
    appendU64(hash, quote.graph_validation_rescans);
    appendU64(hash, quote.data_fingerprint_rescans);
    appendU64(hash, quote.query_fingerprint_rescans);
    appendU64(hash, quote.adjacency_probes);
    appendU64(hash, quote.reverse_edge_lookups);
    appendU64(hash, quote.maximum_arc_checks);
    appendU64(hash, quote.proposed_root_cover_cardinality);
    appendU64(hash, quote.maximum_relation_entries);
    appendU64(hash, quote.maximum_candidate_value_occurrences);
    appendU64(hash, quote.maximum_blocks);
    appendU64(hash, quote.maximum_tree_child_entries);
    appendU64(hash, quote.maximum_non_tree_edges);
    appendU64(hash, quote.maximum_root_self_loop_checks);
    appendU64(hash, quote.maximum_semijoin_entry_visits);
    appendU64(hash, quote.maximum_index_entries);
    appendU64(hash, quote.maximum_payload_bytes);
    return hash;
}

std::uint64_t candidateCoverFingerprint(
    const std::vector<std::vector<int>>& values,
    std::uint64_t graph_fingerprint,
    std::uint64_t query_fingerprint,
    int minimum_duration,
    std::uint64_t incumbent_plan_fingerprint) {
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x434f5645522d5633));
    appendU64(hash, graph_fingerprint);
    appendU64(hash, query_fingerprint);
    appendInt(hash, minimum_duration);
    appendU64(hash, incumbent_plan_fingerprint);
    appendU64(hash, static_cast<std::uint64_t>(values.size()));
    for (std::size_t query_vertex = 0;
         query_vertex < values.size();
         ++query_vertex) {
        appendU64(hash, static_cast<std::uint64_t>(query_vertex));
        appendU64(
            hash,
            static_cast<std::uint64_t>(values[query_vertex].size()));
        for (int data_vertex : values[query_vertex]) {
            appendInt(hash, data_vertex);
        }
    }
    return hash;
}

void markQuoteOverflow(CandidateBuildQuote& quote) {
    quote.complete = false;
    quote.overflowed = true;
    quote.status = CandidateBuildQuoteStatus::ArithmeticOverflow;
}

FixedPlanValidation validatePlanTopologyNoAllocation(
    const Graph& query,
    int root,
    const std::vector<int>& order,
    const std::vector<int>& parent) noexcept {
    FixedPlanValidation result;
    const std::size_t vertex_count =
        query.num_vertices > 0
        ? static_cast<std::size_t>(query.num_vertices)
        : 0;
    if (vertex_count == 0) {
        result.status =
            FixedPlanValidationStatus::InvalidQueryTopology;
        return result;
    }
    if (root < 0 || root >= query.num_vertices) {
        result.status = FixedPlanValidationStatus::RootOutOfRange;
        return result;
    }
    if (order.size() != vertex_count) {
        result.status = FixedPlanValidationStatus::OrderSizeMismatch;
        return result;
    }
    if (parent.size() != vertex_count) {
        result.status = FixedPlanValidationStatus::ParentSizeMismatch;
        return result;
    }
    if (order.empty() || order.front() != root) {
        result.status = FixedPlanValidationStatus::RootNotFirst;
        return result;
    }

    for (std::size_t depth = 0; depth < order.size(); ++depth) {
        const int query_vertex = order[depth];
        if (query_vertex < 0 || query_vertex >= query.num_vertices) {
            result.status =
                FixedPlanValidationStatus::OrderVertexOutOfRange;
            result.query_vertex = query_vertex;
            return result;
        }
        for (std::size_t earlier = 0; earlier < depth; ++earlier) {
            if (order[earlier] == query_vertex) {
                result.status =
                    FixedPlanValidationStatus::DuplicateOrderVertex;
                result.query_vertex = query_vertex;
                return result;
            }
        }
    }

    if (parent[static_cast<std::size_t>(root)] != -1) {
        result.status =
            FixedPlanValidationStatus::RootParentNotSentinel;
        result.query_vertex = root;
        return result;
    }
    for (std::size_t depth = 1; depth < order.size(); ++depth) {
        const int child = order[depth];
        const int parent_vertex =
            parent[static_cast<std::size_t>(child)];
        if (parent_vertex < 0 || parent_vertex >= query.num_vertices) {
            result.status =
                FixedPlanValidationStatus::NonRootParentOutOfRange;
            result.query_vertex = child;
            return result;
        }
        bool parent_precedes = false;
        for (std::size_t earlier = 0; earlier < depth; ++earlier) {
            if (order[earlier] == parent_vertex) {
                parent_precedes = true;
                break;
            }
        }
        if (!parent_precedes) {
            result.status =
                FixedPlanValidationStatus::ParentDoesNotPrecedeChild;
            result.query_vertex = child;
            return result;
        }
        if (requiredArcMask(query, parent_vertex, child) ==
            RequiredArcMask::None) {
            result.status =
                FixedPlanValidationStatus::ParentChildNotAdjacent;
            result.query_vertex = child;
            return result;
        }
    }
    result.valid = true;
    result.status = FixedPlanValidationStatus::Valid;
    return result;
}

bool decompositionTopologyIsCanonicalNoAllocation(
    const Graph& query,
    const QueryDecomposition& decomposition) noexcept {
    const std::size_t vertex_count =
        query.num_vertices > 0
        ? static_cast<std::size_t>(query.num_vertices)
        : 0;
    if (!decomposition.connected || vertex_count == 0 ||
        decomposition.vertex_labels.size() != vertex_count ||
        query.vertex_labels.size() != vertex_count) {
        return false;
    }
    for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
        if (decomposition.vertex_labels[vertex] !=
            query.vertex_labels[vertex]) {
            return false;
        }
    }
    if (!validatePlanTopologyNoAllocation(
            query,
            decomposition.root,
            decomposition.dfs_order,
            decomposition.parent).valid ||
        decomposition.spanning_tree_adj.size() != vertex_count) {
        return false;
    }

    for (int parent_vertex = 0;
         parent_vertex < query.num_vertices;
         ++parent_vertex) {
        std::size_t expected_children = 0;
        for (int child = 0; child < query.num_vertices; ++child) {
            if (decomposition.parent[static_cast<std::size_t>(child)] ==
                parent_vertex) {
                ++expected_children;
            }
        }
        const auto& actual_children =
            decomposition.spanning_tree_adj[
                static_cast<std::size_t>(parent_vertex)];
        if (actual_children.size() != expected_children) return false;
        for (std::size_t index = 0;
             index < actual_children.size();
             ++index) {
            const int child = actual_children[index];
            if (child < 0 || child >= query.num_vertices ||
                decomposition.parent[static_cast<std::size_t>(child)] !=
                    parent_vertex) {
                return false;
            }
            for (std::size_t earlier = 0; earlier < index; ++earlier) {
                if (actual_children[earlier] == child) return false;
            }
        }
    }

    std::size_t expected_non_tree_arcs = 0;
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const bool selected_tree_pair =
                decomposition.parent[static_cast<std::size_t>(source)] ==
                    edge.to ||
                decomposition.parent[static_cast<std::size_t>(edge.to)] ==
                    source;
            if (!selected_tree_pair) {
                if (expected_non_tree_arcs ==
                    std::numeric_limits<std::size_t>::max()) {
                    return false;
                }
                ++expected_non_tree_arcs;
            }
        }
    }
    if (decomposition.non_tree_edges.size() != expected_non_tree_arcs) {
        return false;
    }
    for (std::size_t index = 0;
         index < decomposition.non_tree_edges.size();
         ++index) {
        const auto& arc = decomposition.non_tree_edges[index];
        if (arc.first < 0 || arc.first >= query.num_vertices ||
            arc.second < 0 || arc.second >= query.num_vertices ||
            !GraphUtils::hasEdge(query.adj, arc.first, arc.second)) {
            return false;
        }
        const bool selected_tree_pair =
            decomposition.parent[static_cast<std::size_t>(arc.first)] ==
                arc.second ||
            decomposition.parent[static_cast<std::size_t>(arc.second)] ==
                arc.first;
        if (selected_tree_pair) return false;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (decomposition.non_tree_edges[earlier] == arc) return false;
        }
    }
    return true;
}

} // namespace

#ifdef DSM_TCC_V3_TESTING
bool checkedCandidateCoverCapacityAddProductForTesting(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result) noexcept {
    std::uint64_t product = 0;
    std::uint64_t sum = 0;
    if (!checkedMultiply(count, width, product) ||
        !checkedAdd(initial, product, sum)) {
        return false;
    }
    result = sum;
    return true;
}
#endif

bool CandidateCoverCapacityReceipt::invariantHolds() const noexcept {
    if (!complete || overflowed ||
        object_bytes != sizeof(CandidateCover) ||
        domain_count > domain_capacity ||
        value_count > value_capacity) {
        return false;
    }
    std::uint64_t expected = 0;
    if (!checkedMultiply(
            domain_capacity,
            static_cast<std::uint64_t>(sizeof(std::vector<int>)),
            expected) ||
        expected != outer_domains_bytes ||
        !checkedMultiply(
            value_capacity,
            static_cast<std::uint64_t>(sizeof(int)),
            expected) ||
        expected != nested_values_bytes ||
        !checkedAdd(
            outer_domains_bytes,
            nested_values_bytes,
            expected) ||
        expected != dynamic_bytes ||
        !checkedAdd(object_bytes, dynamic_bytes, expected) ||
        expected != total_bytes || total_bytes == 0) {
        return false;
    }
    return true;
}

CandidateCover::CandidateCover(
    std::vector<std::vector<int>> values,
    const Graph* source_graph,
    const Graph* source_query,
    std::uint64_t graph_fingerprint,
    std::uint64_t query_fingerprint,
    int minimum_duration,
    std::uint64_t incumbent_plan_fingerprint,
    std::uint64_t source_relation_entries)
    : values_(std::move(values)),
      source_graph_(source_graph),
      source_query_(source_query),
      graph_fingerprint_(graph_fingerprint),
      query_fingerprint_(query_fingerprint),
      minimum_duration_(minimum_duration),
      incumbent_plan_fingerprint_(incumbent_plan_fingerprint),
      cover_fingerprint_(candidateCoverFingerprint(
          values_,
          graph_fingerprint_,
          query_fingerprint_,
          minimum_duration_,
          incumbent_plan_fingerprint_)),
      source_relation_entries_(source_relation_entries) {}

CandidateCover::CandidateCover(
    std::vector<std::vector<int>> values,
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
    int minimum_duration,
    std::uint64_t incumbent_plan_fingerprint,
    std::uint64_t source_relation_entries)
    : values_(std::move(values)),
      snapshot_owner_(std::move(snapshot)),
      source_graph_(snapshot_owner_ == nullptr
          ? nullptr
          : &snapshot_owner_->data()),
      source_query_(snapshot_owner_ == nullptr
          ? nullptr
          : &snapshot_owner_->query()),
      graph_fingerprint_(snapshot_owner_ == nullptr
          ? 0
          : snapshot_owner_->dataFingerprint()),
      query_fingerprint_(snapshot_owner_ == nullptr
          ? 0
          : snapshot_owner_->queryFingerprint()),
      minimum_duration_(minimum_duration),
      incumbent_plan_fingerprint_(incumbent_plan_fingerprint),
      cover_fingerprint_(candidateCoverFingerprint(
          values_,
          graph_fingerprint_,
          query_fingerprint_,
          minimum_duration_,
          incumbent_plan_fingerprint_)),
      source_relation_entries_(source_relation_entries) {}

std::size_t CandidateCover::queryVertexCount() const noexcept {
    return values_.size();
}

const std::vector<std::vector<int>>&
CandidateCover::values() const noexcept {
    return values_;
}

const std::vector<int>& CandidateCover::domain(
    std::size_t query_vertex) const {
    if (query_vertex >= values_.size()) {
        throw std::out_of_range(
            "candidate-cover query vertex is out of range");
    }
    return values_[query_vertex];
}

std::uint64_t CandidateCover::graphFingerprint() const noexcept {
    return graph_fingerprint_;
}

std::uint64_t CandidateCover::queryFingerprint() const noexcept {
    return query_fingerprint_;
}

int CandidateCover::minimumDuration() const noexcept {
    return minimum_duration_;
}

std::uint64_t CandidateCover::incumbentPlanFingerprint() const noexcept {
    return incumbent_plan_fingerprint_;
}

std::uint64_t CandidateCover::coverFingerprint() const noexcept {
    return cover_fingerprint_;
}

std::uint64_t CandidateCover::sourceRelationEntries() const noexcept {
    return source_relation_entries_;
}

CandidateCoverCapacityReceipt CandidateCover::capacityReceipt()
    const noexcept {
    CandidateCoverCapacityReceipt receipt;
    receipt.object_bytes = sizeof(CandidateCover);
    auto overflow = [&]() noexcept {
        receipt.complete = false;
        receipt.overflowed = true;
        return receipt;
    };

    if (!sizeToU64(values_.size(), receipt.domain_count) ||
        !sizeToU64(values_.capacity(), receipt.domain_capacity) ||
        !checkedMultiply(
            receipt.domain_capacity,
            static_cast<std::uint64_t>(sizeof(std::vector<int>)),
            receipt.outer_domains_bytes)) {
        return overflow();
    }
    for (const std::vector<int>& domain_values : values_) {
        std::uint64_t size = 0;
        std::uint64_t capacity = 0;
        if (!sizeToU64(domain_values.size(), size) ||
            !sizeToU64(domain_values.capacity(), capacity) ||
            !checkedAccumulate(receipt.value_count, size) ||
            !checkedAccumulate(receipt.value_capacity, capacity)) {
            return overflow();
        }
    }
    if (!checkedMultiply(
            receipt.value_capacity,
            static_cast<std::uint64_t>(sizeof(int)),
            receipt.nested_values_bytes) ||
        !checkedAdd(
            receipt.outer_domains_bytes,
            receipt.nested_values_bytes,
            receipt.dynamic_bytes) ||
        !checkedAdd(
            receipt.object_bytes,
            receipt.dynamic_bytes,
            receipt.total_bytes)) {
        return overflow();
    }
    receipt.complete = true;
    if (!receipt.invariantHolds()) receipt.complete = false;
    return receipt;
}

bool CandidateCover::hasValidatedSnapshotAuthority() const noexcept {
    return snapshot_owner_ != nullptr;
}

std::uint64_t CandidateCover::snapshotToken() const noexcept {
    return snapshot_owner_ == nullptr
        ? 0
        : snapshot_owner_->snapshotToken();
}

bool CandidateCover::isBoundToValidatedSnapshot(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
    const noexcept {
    return sharesValidatedGraphSnapshot(
        snapshot_owner_, snapshot);
}

bool CandidateCover::isBoundTo(
    const Graph& data,
    const Graph& query) const noexcept {
    return source_graph_ == &data && source_query_ == &query;
}

std::uint64_t queryGraphContentFingerprint(const Graph& query) {
#ifdef DSM_TCC_V3_TESTING
    g_query_fingerprint_calls.fetch_add(1);
#endif
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x51554552592d5633));
    appendInt(hash, query.num_vertices);

    const int vertex_count = std::max(0, query.num_vertices);
    appendU64(hash, static_cast<std::uint64_t>(vertex_count));
    for (int vertex = 0; vertex < vertex_count; ++vertex) {
        appendInt(hash, vertex);
        const std::size_t index = static_cast<std::size_t>(vertex);
        appendU64(
            hash,
            index < query.vertex_labels.size()
                ? static_cast<std::uint64_t>(query.vertex_labels[index])
                : static_cast<std::uint64_t>(kInvalidLabel));
    }

    std::vector<std::pair<int, int>> arcs;
    for (int source = 0; source < vertex_count; ++source) {
        const std::size_t source_index =
            static_cast<std::size_t>(source);
        if (source_index >= query.adj.size()) continue;
        for (const Edge& edge : query.adj[source_index]) {
            arcs.emplace_back(source, edge.to);
        }
    }
    std::sort(arcs.begin(), arcs.end());
    arcs.erase(std::unique(arcs.begin(), arcs.end()), arcs.end());
    appendU64(hash, static_cast<std::uint64_t>(arcs.size()));
    for (const auto& arc : arcs) {
        appendInt(hash, arc.first);
        appendInt(hash, arc.second);
    }
    return hash;
}

#ifdef DSM_TCC_V3_TESTING
void resetQueryGraphContentFingerprintCallCount() noexcept {
    g_query_fingerprint_calls.store(0);
}

std::uint64_t queryGraphContentFingerprintCallCount() noexcept {
    return g_query_fingerprint_calls.load();
}
#endif

std::uint64_t fixedPlanFingerprint(
    std::uint64_t query_fingerprint,
    int root,
    const std::vector<int>& order,
    const std::vector<int>& parent) noexcept {
    std::uint64_t hash = kFnvOffset;
    appendU64(hash, UINT64_C(0x504c414e2d5633));
    appendU64(hash, query_fingerprint);
    appendInt(hash, root);
    appendU64(hash, static_cast<std::uint64_t>(order.size()));
    for (int vertex : order) appendInt(hash, vertex);
    appendU64(hash, static_cast<std::uint64_t>(parent.size()));
    for (int parent_vertex : parent) appendInt(hash, parent_vertex);
    return hash;
}

FixedPlan makeFixedPlan(
    const Graph& query,
    int root,
    std::vector<int> order,
    std::vector<int> parent) {
    FixedPlan result;
    result.root = root;
    result.order = std::move(order);
    result.parent = std::move(parent);
    result.query_fingerprint =
        queryGraphContentFingerprint(query);
    result.fingerprint = fixedPlanFingerprint(
        result.query_fingerprint,
        result.root,
        result.order,
        result.parent);
    return result;
}

FixedPlan makeFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    int root,
    std::vector<int> order,
    std::vector<int> parent) {
    FixedPlan result;
    result.root = root;
    result.order = std::move(order);
    result.parent = std::move(parent);
    result.query_fingerprint = snapshot.queryFingerprint();
    result.fingerprint = fixedPlanFingerprint(
        result.query_fingerprint,
        result.root,
        result.order,
        result.parent);
    return result;
}

bool queryDecompositionTopologyIsCanonical(
    const Graph& query,
    const QueryDecomposition& decomposition) noexcept {
    return queryTopologyIsValid(query) &&
        decompositionTopologyIsCanonicalNoAllocation(
            query, decomposition);
}

bool queryDecompositionTopologyIsCanonical(
    const ValidatedGraphSnapshot& snapshot,
    const QueryDecomposition& decomposition) noexcept {
    return snapshot.receipt().complete() &&
        decompositionTopologyIsCanonicalNoAllocation(
            snapshot.query(), decomposition);
}

FixedPlan makeFixedPlan(
    const Graph& query,
    const QueryDecomposition& decomposition) {
    return makeFixedPlan(
        query,
        decomposition.root,
        decomposition.dfs_order,
        decomposition.parent);
}

FixedPlan makeFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    const QueryDecomposition& decomposition) {
    return makeFixedPlan(
        snapshot,
        decomposition.root,
        decomposition.dfs_order,
        decomposition.parent);
}

const char* fixedPlanValidationStatusName(
    FixedPlanValidationStatus status) {
    switch (status) {
        case FixedPlanValidationStatus::Valid:
            return "valid";
        case FixedPlanValidationStatus::InvalidQueryTopology:
            return "invalid-query-topology";
        case FixedPlanValidationStatus::RootOutOfRange:
            return "root-out-of-range";
        case FixedPlanValidationStatus::OrderSizeMismatch:
            return "order-size-mismatch";
        case FixedPlanValidationStatus::ParentSizeMismatch:
            return "parent-size-mismatch";
        case FixedPlanValidationStatus::RootNotFirst:
            return "root-not-first";
        case FixedPlanValidationStatus::OrderVertexOutOfRange:
            return "order-vertex-out-of-range";
        case FixedPlanValidationStatus::DuplicateOrderVertex:
            return "duplicate-order-vertex";
        case FixedPlanValidationStatus::RootParentNotSentinel:
            return "root-parent-not-sentinel";
        case FixedPlanValidationStatus::NonRootParentOutOfRange:
            return "non-root-parent-out-of-range";
        case FixedPlanValidationStatus::ParentDoesNotPrecedeChild:
            return "parent-does-not-precede-child";
        case FixedPlanValidationStatus::ParentChildNotAdjacent:
            return "parent-child-not-adjacent";
        case FixedPlanValidationStatus::QueryFingerprintMismatch:
            return "query-fingerprint-mismatch";
        case FixedPlanValidationStatus::PlanFingerprintMismatch:
            return "plan-fingerprint-mismatch";
    }
    return "unknown";
}

FixedPlanValidation validateFixedPlan(
    const Graph& query,
    const FixedPlan& plan) {
    if (!queryTopologyIsValid(query)) {
        FixedPlanValidation result;
        result.status =
            FixedPlanValidationStatus::InvalidQueryTopology;
        return result;
    }
    FixedPlanValidation result = validatePlanTopologyNoAllocation(
        query, plan.root, plan.order, plan.parent);
    if (!result.valid) return result;

    const std::uint64_t expected_query_fingerprint =
        queryGraphContentFingerprint(query);
    if (plan.query_fingerprint != expected_query_fingerprint) {
        result.valid = false;
        result.status =
            FixedPlanValidationStatus::QueryFingerprintMismatch;
        return result;
    }
    const std::uint64_t expected_plan_fingerprint =
        fixedPlanFingerprint(
            expected_query_fingerprint,
            plan.root,
            plan.order,
            plan.parent);
    if (plan.fingerprint != expected_plan_fingerprint) {
        result.valid = false;
        result.status =
            FixedPlanValidationStatus::PlanFingerprintMismatch;
        return result;
    }

    result.valid = true;
    result.status = FixedPlanValidationStatus::Valid;
    return result;
}

FixedPlanValidation validateFixedPlan(
    const ValidatedGraphSnapshot& snapshot,
    const FixedPlan& plan) noexcept {
    if (!snapshot.receipt().complete()) {
        FixedPlanValidation result;
        result.status =
            FixedPlanValidationStatus::InvalidQueryTopology;
        return result;
    }
    const Graph& query = snapshot.query();
    FixedPlanValidation result = validatePlanTopologyNoAllocation(
        query, plan.root, plan.order, plan.parent);
    if (!result.valid) return result;

    const std::uint64_t expected_query_fingerprint =
        snapshot.queryFingerprint();
    if (plan.query_fingerprint != expected_query_fingerprint) {
        result.valid = false;
        result.status =
            FixedPlanValidationStatus::QueryFingerprintMismatch;
        return result;
    }
    const std::uint64_t expected_plan_fingerprint =
        fixedPlanFingerprint(
            expected_query_fingerprint,
            plan.root,
            plan.order,
            plan.parent);
    if (plan.fingerprint != expected_plan_fingerprint) {
        result.valid = false;
        result.status =
            FixedPlanValidationStatus::PlanFingerprintMismatch;
        return result;
    }
    result.valid = true;
    result.status = FixedPlanValidationStatus::Valid;
    return result;
}

RequiredArcMask requiredArcMask(
    const Graph& query,
    int query_parent,
    int query_child) {
    const bool parent_to_child = GraphUtils::hasEdge(
        query.adj, query_parent, query_child);
    const bool child_to_parent = GraphUtils::hasEdge(
        query.adj, query_child, query_parent);
    const std::uint8_t mask =
        static_cast<std::uint8_t>(parent_to_child ? 1U : 0U) |
        static_cast<std::uint8_t>(child_to_parent ? 2U : 0U);
    return static_cast<RequiredArcMask>(mask);
}

CandidateBuildAccounting defaultCandidateBuildAccounting() {
    CandidateBuildAccounting accounting;
    accounting.tree_object_payload_bytes = sizeof(TDTree);
    accounting.node_payload_bytes =
        sizeof(TDTreeNode) +
        2 * sizeof(std::array<int, kLabelCount>);
    accounting.relation_entry_bytes = sizeof(int);
    accounting.block_payload_bytes = sizeof(TDTreeBlock);
    accounting.plan_vertex_payload_bytes =
        sizeof(std::vector<int>) +
        2 * sizeof(int) +
        sizeof(Label) +
        sizeof(double);
    accounting.tree_child_entry_bytes = sizeof(int);
    accounting.non_tree_edge_payload_bytes =
        sizeof(std::pair<int, int>);
    accounting.data_vertex_marker_bytes =
        2 * sizeof(std::uint64_t);
    accounting.semijoin_visits_per_candidate_value = 2;
    accounting.semijoin_visits_per_block = 3;
    accounting.index_entries_per_block = 1;
    return accounting;
}

bool CandidateBuildQuote::materializerContractBound() const noexcept {
    return materializer_contract_bound_;
}

bool CandidateBuildQuote::validatedSnapshotBound() const noexcept {
    return snapshot_owner_ != nullptr &&
        validation_snapshot_token != 0 &&
        snapshot_owner_->snapshotToken() ==
            validation_snapshot_token;
}

bool CandidateBuildQuote::isBoundToValidatedSnapshot(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
    const noexcept {
    return sharesValidatedGraphSnapshot(
        snapshot_owner_, snapshot);
}

const char* CandidateBuildQuote::payloadModel() const noexcept {
    return kCandidatePayloadModel;
}

std::uint64_t CandidateBuildQuote::accountingFingerprint() const noexcept {
    return accounting_fingerprint_;
}

std::uint64_t CandidateBuildQuote::contractFingerprint() const noexcept {
    return contract_fingerprint_;
}

const char* candidateBuildQuoteStatusName(
    CandidateBuildQuoteStatus status) {
    switch (status) {
        case CandidateBuildQuoteStatus::Complete:
            return "complete";
        case CandidateBuildQuoteStatus::InvalidDataGraph:
            return "invalid-data-graph";
        case CandidateBuildQuoteStatus::InvalidQueryGraph:
            return "invalid-query-graph";
        case CandidateBuildQuoteStatus::InvalidAccounting:
            return "invalid-accounting";
        case CandidateBuildQuoteStatus::InvalidMinimumDuration:
            return "invalid-minimum-duration";
        case CandidateBuildQuoteStatus::CoverQueryCardinalityMismatch:
            return "cover-query-cardinality-mismatch";
        case CandidateBuildQuoteStatus::DataGraphIdentityMismatch:
            return "data-graph-identity-mismatch";
        case CandidateBuildQuoteStatus::QueryGraphIdentityMismatch:
            return "query-graph-identity-mismatch";
        case CandidateBuildQuoteStatus::CoverDomainNotSortedUnique:
            return "cover-domain-not-sorted-unique";
        case CandidateBuildQuoteStatus::CoverCandidateOutOfRange:
            return "cover-candidate-out-of-range";
        case CandidateBuildQuoteStatus::InvalidFixedPlan:
            return "invalid-fixed-plan";
        case CandidateBuildQuoteStatus::IncumbentPlanRejected:
            return "incumbent-plan-rejected";
        case CandidateBuildQuoteStatus::ArithmeticOverflow:
            return "arithmetic-overflow";
    }
    return "unknown";
}

CandidateBuildQuote CandidateBuildQuote::quoteImpl(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    CandidateBuildAccounting accounting,
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot) {
    CandidateBuildQuote quote;
    quote.graph_fingerprint = cover.graphFingerprint();
    quote.query_fingerprint = cover.queryFingerprint();
    quote.cover_fingerprint = cover.coverFingerprint();
    quote.plan_fingerprint = plan.fingerprint;
    quote.validation_snapshot_token = snapshot == nullptr
        ? 0
        : snapshot->snapshotToken();

    if (snapshot != nullptr) {
        if (!snapshot->receipt().complete() ||
            !sharesValidatedGraphSnapshot(
                snapshot, cover.snapshot_owner_)) {
            quote.status =
                CandidateBuildQuoteStatus::DataGraphIdentityMismatch;
            return quote;
        }
    } else {
        if (cover.source_graph_ != &data) {
            quote.status =
                CandidateBuildQuoteStatus::DataGraphIdentityMismatch;
            return quote;
        }
        if (cover.source_query_ != &query) {
            quote.status =
                CandidateBuildQuoteStatus::QueryGraphIdentityMismatch;
            return quote;
        }
        if (!dataGraphIsValidForQuote(data)) {
            quote.status = CandidateBuildQuoteStatus::InvalidDataGraph;
            return quote;
        }
        if (!queryTopologyIsValid(query)) {
            quote.status = CandidateBuildQuoteStatus::InvalidQueryGraph;
            return quote;
        }
    }
    if (!accountingIsValid(accounting)) {
        quote.status = CandidateBuildQuoteStatus::InvalidAccounting;
        return quote;
    }
    if (cover.minimumDuration() < 1) {
        quote.status =
            CandidateBuildQuoteStatus::InvalidMinimumDuration;
        return quote;
    }
    if (cover.queryVertexCount() !=
        static_cast<std::size_t>(query.num_vertices)) {
        quote.status =
            CandidateBuildQuoteStatus::CoverQueryCardinalityMismatch;
        return quote;
    }
    if (snapshot == nullptr) {
        if (cover.graphFingerprint() !=
            temporalGraphContentFingerprint(data)) {
            quote.status =
                CandidateBuildQuoteStatus::DataGraphIdentityMismatch;
            return quote;
        }
        if (cover.queryFingerprint() !=
            queryGraphContentFingerprint(query)) {
            quote.status =
                CandidateBuildQuoteStatus::QueryGraphIdentityMismatch;
            return quote;
        }
    } else if (cover.graphFingerprint() !=
                   snapshot->dataFingerprint() ||
               cover.queryFingerprint() !=
                   snapshot->queryFingerprint()) {
        quote.status =
            CandidateBuildQuoteStatus::QueryGraphIdentityMismatch;
        return quote;
    }

    if (snapshot == nullptr) {
        for (const auto& domain : cover.values()) {
            if (!isStrictlySortedUnique(domain)) {
                quote.status =
                    CandidateBuildQuoteStatus::CoverDomainNotSortedUnique;
                return quote;
            }
            for (int data_vertex : domain) {
                if (data_vertex < 0 ||
                    data_vertex >= data.num_vertices) {
                    quote.status =
                        CandidateBuildQuoteStatus::CoverCandidateOutOfRange;
                    return quote;
                }
            }
        }
    }

    quote.plan_validation = snapshot == nullptr
        ? validateFixedPlan(query, plan)
        : validateFixedPlan(*snapshot, plan);
    if (!quote.plan_validation.valid) {
        quote.status = CandidateBuildQuoteStatus::InvalidFixedPlan;
        return quote;
    }
    if (plan.fingerprint == cover.incumbentPlanFingerprint()) {
        quote.status =
            CandidateBuildQuoteStatus::IncumbentPlanRejected;
        return quote;
    }

    if (!sizeToU64(
            cover.domain(static_cast<std::size_t>(plan.root)).size(),
            quote.proposed_root_cover_cardinality)) {
        markQuoteOverflow(quote);
        return quote;
    }
    quote.maximum_root_self_loop_checks =
        GraphUtils::hasEdge(query.adj, plan.root, plan.root)
        ? quote.proposed_root_cover_cardinality
        : 0;

    quote.maximum_tree_child_entries =
        static_cast<std::uint64_t>(query.num_vertices - 1);
    for (int source = 0; source < query.num_vertices; ++source) {
        for (const Edge& edge :
             query.adj[static_cast<std::size_t>(source)]) {
            const bool selected_tree_pair =
                (plan.parent[static_cast<std::size_t>(source)] ==
                    edge.to) ||
                (plan.parent[static_cast<std::size_t>(edge.to)] ==
                    source);
            if (!selected_tree_pair &&
                !checkedAccumulate(
                    quote.maximum_non_tree_edges, 1)) {
                markQuoteOverflow(quote);
                return quote;
            }
        }
    }

    for (std::size_t depth = 1; depth < plan.order.size(); ++depth) {
        const int child = plan.order[depth];
        const int parent =
            plan.parent[static_cast<std::size_t>(child)];
        const RequiredArcMask mask =
            requiredArcMask(query, parent, child);
        const auto& parent_domain =
            cover.domain(static_cast<std::size_t>(parent));
        std::uint64_t parent_domain_size = 0;
        if (!sizeToU64(parent_domain.size(), parent_domain_size) ||
            !checkedAccumulate(
                quote.maximum_blocks,
                parent_domain_size)) {
            markQuoteOverflow(quote);
            return quote;
        }

        for (int parent_data_vertex : parent_domain) {
            const std::size_t data_index =
                static_cast<std::size_t>(parent_data_vertex);
            const std::size_t outgoing_degree =
                data.adj[data_index].size();
            const std::size_t incoming_degree =
                data.in_adj[data_index].size();
            std::size_t scan_degree = 0;
            if (mask == RequiredArcMask::ParentToChild) {
                scan_degree = outgoing_degree;
            } else if (mask == RequiredArcMask::ChildToParent) {
                scan_degree = incoming_degree;
            } else {
                scan_degree =
                    std::min(outgoing_degree, incoming_degree);
            }
            std::uint64_t scan_degree_u64 = 0;
            if (!sizeToU64(scan_degree, scan_degree_u64) ||
                !checkedAccumulate(
                    quote.adjacency_probes,
                    scan_degree_u64)) {
                markQuoteOverflow(quote);
                return quote;
            }
            if (mask == RequiredArcMask::Reciprocal &&
                !checkedAccumulate(
                    quote.reverse_edge_lookups,
                    scan_degree_u64)) {
                markQuoteOverflow(quote);
                return quote;
            }
        }
    }

    quote.maximum_relation_entries = quote.adjacency_probes;
    std::uint64_t pair_arc_checks = 0;
    if (!checkedAdd(
            quote.adjacency_probes,
            quote.reverse_edge_lookups,
            pair_arc_checks) ||
        !checkedAdd(
            pair_arc_checks,
            quote.maximum_root_self_loop_checks,
            quote.maximum_arc_checks) ||
        !checkedAdd(
            quote.proposed_root_cover_cardinality,
            quote.maximum_relation_entries,
            quote.maximum_candidate_value_occurrences)) {
        markQuoteOverflow(quote);
        return quote;
    }

    std::uint64_t candidate_visits = 0;
    std::uint64_t block_visits = 0;
    if (!checkedMultiply(
            quote.maximum_candidate_value_occurrences,
            accounting.semijoin_visits_per_candidate_value,
            candidate_visits) ||
        !checkedMultiply(
            quote.maximum_blocks,
            accounting.semijoin_visits_per_block,
            block_visits) ||
        !checkedAdd(
            candidate_visits,
            block_visits,
            quote.maximum_semijoin_entry_visits) ||
        !checkedMultiply(
            quote.maximum_blocks,
            accounting.index_entries_per_block,
            quote.maximum_index_entries)) {
        markQuoteOverflow(quote);
        return quote;
    }

    std::uint64_t query_vertices = 0;
    std::uint64_t payload = 0;
    std::uint64_t term = 0;
    if (!sizeToU64(
            static_cast<std::size_t>(query.num_vertices),
            query_vertices) ||
        !checkedAccumulate(
            payload,
            accounting.tree_object_payload_bytes) ||
        !checkedMultiply(
            query_vertices,
            accounting.node_payload_bytes,
            term) ||
        !checkedAccumulate(payload, term) ||
        !checkedMultiply(
            query_vertices,
            accounting.plan_vertex_payload_bytes,
            term) ||
        !checkedAccumulate(payload, term) ||
        !checkedMultiply(
            quote.maximum_tree_child_entries,
            accounting.tree_child_entry_bytes,
            term) ||
        !checkedAccumulate(payload, term) ||
        !checkedMultiply(
            quote.maximum_non_tree_edges,
            accounting.non_tree_edge_payload_bytes,
            term) ||
        !checkedAccumulate(payload, term) ||
        !checkedMultiply(
            static_cast<std::uint64_t>(data.num_vertices),
            accounting.data_vertex_marker_bytes,
            term) ||
        !checkedAccumulate(payload, term) ||
        !checkedMultiply(
            quote.maximum_candidate_value_occurrences,
            accounting.relation_entry_bytes,
            term) ||
        !checkedAccumulate(payload, term) ||
        !checkedMultiply(
            quote.maximum_blocks,
            accounting.block_payload_bytes,
            term) ||
        !checkedAccumulate(payload, term)) {
        markQuoteOverflow(quote);
        return quote;
    }
    quote.maximum_payload_bytes = payload;
    quote.complete = true;
    quote.status = CandidateBuildQuoteStatus::Complete;
    quote.accounting_ = accounting;
    quote.snapshot_owner_ = std::move(snapshot);
    quote.accounting_fingerprint_ =
        candidateBuildAccountingFingerprint(accounting);
    quote.materializer_contract_bound_ = true;
    quote.contract_fingerprint_ = candidateBuildContractFingerprint(
        quote, quote.accounting_fingerprint_);
    return quote;
}

CandidateBuildQuote quoteCoverAwareBuild(
    const Graph& data,
    const Graph& query,
    const CandidateCover& cover,
    const FixedPlan& plan,
    CandidateBuildAccounting accounting) {
    return CandidateBuildQuote::quoteImpl(
        data, query, cover, plan, accounting, nullptr);
}

CandidateBuildQuote quoteCoverAwareBuild(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const CandidateCover& cover,
    const FixedPlan& plan,
    CandidateBuildAccounting accounting) {
    if (!snapshot) {
        CandidateBuildQuote quote;
        quote.status = CandidateBuildQuoteStatus::InvalidDataGraph;
        return quote;
    }
    return CandidateBuildQuote::quoteImpl(
        snapshot->data(),
        snapshot->query(),
        cover,
        plan,
        accounting,
        snapshot);
}

const char* candidateBuildBudgetFitReasonName(
    CandidateBuildBudgetFitReason reason) {
    switch (reason) {
        case CandidateBuildBudgetFitReason::FitsQuote:
            return "fits-quote";
        case CandidateBuildBudgetFitReason::QuoteIncomplete:
            return "quote-incomplete";
        case CandidateBuildBudgetFitReason::QuoteOverflow:
            return "quote-overflow";
        case CandidateBuildBudgetFitReason::NoCandidateTreeToken:
            return "no-candidate-tree-token";
        case CandidateBuildBudgetFitReason::AdjacencyProbeBudget:
            return "adjacency-probe-budget";
        case CandidateBuildBudgetFitReason::ReverseLookupBudget:
            return "reverse-lookup-budget";
        case CandidateBuildBudgetFitReason::ArcCheckBudget:
            return "arc-check-budget";
        case CandidateBuildBudgetFitReason::RelationEntryBudget:
            return "relation-entry-budget";
        case CandidateBuildBudgetFitReason::CandidateValueBudget:
            return "candidate-value-budget";
        case CandidateBuildBudgetFitReason::BlockBudget:
            return "block-budget";
        case CandidateBuildBudgetFitReason::SemijoinVisitBudget:
            return "semijoin-visit-budget";
        case CandidateBuildBudgetFitReason::IndexEntryBudget:
            return "index-entry-budget";
        case CandidateBuildBudgetFitReason::PayloadMemoryBudget:
            return "payload-memory-budget";
        case CandidateBuildBudgetFitReason::GlobalMemoryAdmission:
            return "global-memory-admission";
    }
    return "unknown";
}

bool candidateBuildQuoteContractIsValid(
    const CandidateBuildQuote& quote) {
    const bool snapshot_binding_valid =
        (quote.validation_snapshot_token == 0 &&
         quote.snapshot_owner_ == nullptr) ||
        (quote.snapshot_owner_ != nullptr &&
         quote.validation_snapshot_token ==
             quote.snapshot_owner_->snapshotToken());
    return snapshot_binding_valid &&
        quote.materializer_contract_bound_ &&
        quote.complete &&
        !quote.overflowed &&
        quote.status == CandidateBuildQuoteStatus::Complete &&
        quote.accounting_fingerprint_ != 0 &&
        quote.accounting_fingerprint_ ==
            candidateBuildAccountingFingerprint(quote.accounting_) &&
        quote.contract_fingerprint_ != 0 &&
        quote.contract_fingerprint_ ==
            candidateBuildContractFingerprint(
                quote, quote.accounting_fingerprint_);
}

CandidateBuildBudgetFit evaluateCandidateBuildBudget(
    const CandidateBuildQuote& quote,
    const CandidateBuildBudget& budget) {
    CandidateBuildBudgetFit result;
    if (quote.overflowed) {
        result.reason = CandidateBuildBudgetFitReason::QuoteOverflow;
        return result;
    }
    if (!candidateBuildQuoteContractIsValid(quote) ||
        !quote.complete ||
        quote.status != CandidateBuildQuoteStatus::Complete) {
        result.reason = CandidateBuildBudgetFitReason::QuoteIncomplete;
        return result;
    }
    if (budget.candidate_tree_tokens < 1) {
        result.reason =
            CandidateBuildBudgetFitReason::NoCandidateTreeToken;
        return result;
    }
    if (quote.adjacency_probes > budget.adjacency_probes) {
        result.reason =
            CandidateBuildBudgetFitReason::AdjacencyProbeBudget;
        return result;
    }
    if (quote.reverse_edge_lookups >
        budget.reverse_edge_lookups) {
        result.reason =
            CandidateBuildBudgetFitReason::ReverseLookupBudget;
        return result;
    }
    if (quote.maximum_arc_checks > budget.arc_checks) {
        result.reason =
            CandidateBuildBudgetFitReason::ArcCheckBudget;
        return result;
    }
    if (quote.maximum_relation_entries >
        budget.relation_entries) {
        result.reason =
            CandidateBuildBudgetFitReason::RelationEntryBudget;
        return result;
    }
    if (quote.maximum_candidate_value_occurrences >
        budget.candidate_value_occurrences) {
        result.reason =
            CandidateBuildBudgetFitReason::CandidateValueBudget;
        return result;
    }
    if (quote.maximum_blocks > budget.blocks) {
        result.reason = CandidateBuildBudgetFitReason::BlockBudget;
        return result;
    }
    if (quote.maximum_semijoin_entry_visits >
        budget.semijoin_entry_visits) {
        result.reason =
            CandidateBuildBudgetFitReason::SemijoinVisitBudget;
        return result;
    }
    if (quote.maximum_index_entries > budget.index_entries) {
        result.reason =
            CandidateBuildBudgetFitReason::IndexEntryBudget;
        return result;
    }
    // This local reservation owns only the candidate payload partition.  H is
    // checked later, together with all already-live native capacity E, by the
    // independent global predicate E + Q_B + H <= M.  Charging H here would
    // silently reinterpret P as an envelope and double-count headroom.
    result.required_payload_bytes = quote.maximum_payload_bytes;
    if (result.required_payload_bytes > budget.payload_memory_bytes) {
        result.reason =
            CandidateBuildBudgetFitReason::PayloadMemoryBudget;
        return result;
    }
    result.fits = true;
    result.reason = CandidateBuildBudgetFitReason::FitsQuote;
    return result;
}

struct CandidateBuildLedgerState {
    explicit CandidateBuildLedgerState(CandidateBuildBudget initial)
        : remaining(initial) {}

    std::mutex mutex;
    CandidateBuildBudget remaining;
};

CandidateBuildLedger::CandidateBuildLedger(
    CandidateBuildBudget budget)
    : state_(std::make_shared<CandidateBuildLedgerState>(budget)) {}

CandidateBuildBudget CandidateBuildLedger::remaining() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->remaining;
}

CandidateBuildReservation::CandidateBuildReservation(
    const CandidateBuildQuote& quote,
    std::shared_ptr<CandidateBuildLedgerState> state)
    : quote_(quote), state_(std::move(state)) {}

CandidateBuildReservation::~CandidateBuildReservation() {
    rollback();
}

const CandidateBuildQuote&
CandidateBuildReservation::quote() const noexcept {
    return quote_;
}

bool CandidateBuildReservation::active() const noexcept {
    return active_;
}

void CandidateBuildReservation::commit() {
    if (!active_ || !state_) {
        throw std::logic_error(
            "candidate-build reservation is not active");
    }
    active_ = false;
    state_.reset();
}

void CandidateBuildReservation::rollback() noexcept {
    if (!active_ || !state_) return;
    std::lock_guard<std::mutex> lock(state_->mutex);
    CandidateBuildBudget& budget = state_->remaining;
    const CandidateBuildQuote& quote = quote_;
    auto refund = [](std::uint64_t& destination,
                     std::uint64_t value) noexcept {
        if (value > std::numeric_limits<std::uint64_t>::max() -
                        destination) {
            std::terminate();
        }
        destination += value;
    };
    refund(budget.adjacency_probes, quote.adjacency_probes);
    refund(
        budget.reverse_edge_lookups,
        quote.reverse_edge_lookups);
    refund(budget.arc_checks, quote.maximum_arc_checks);
    refund(
        budget.relation_entries,
        quote.maximum_relation_entries);
    refund(
        budget.candidate_value_occurrences,
        quote.maximum_candidate_value_occurrences);
    refund(budget.blocks, quote.maximum_blocks);
    refund(
        budget.semijoin_entry_visits,
        quote.maximum_semijoin_entry_visits);
    refund(budget.index_entries, quote.maximum_index_entries);
    refund(
        budget.payload_memory_bytes,
        quote.maximum_payload_bytes);
    refund(budget.candidate_tree_tokens, 1);
    active_ = false;
    state_.reset();
}

CandidateBuildReservationAttempt tryReserveCandidateBuild(
    const CandidateBuildQuote& quote,
    CandidateBuildLedger& ledger) {
    CandidateBuildReservationAttempt result;
    std::lock_guard<std::mutex> lock(ledger.state_->mutex);
    result.fit = evaluateCandidateBuildBudget(
        quote, ledger.state_->remaining);
    if (!result.fit.fits) return result;

    // Allocate the owner before touching the ledger.  If new throws, the
    // complete ledger remains byte-identical.
#ifdef DSM_TCC_V3_TESTING
    if (g_candidate_reservation_fail_next.exchange(false)) {
        throw std::bad_alloc();
    }
#endif
    result.reservation.reset(
        new CandidateBuildReservation(quote, ledger.state_));

    CandidateBuildBudget& budget = ledger.state_->remaining;
    budget.adjacency_probes -= quote.adjacency_probes;
    budget.reverse_edge_lookups -= quote.reverse_edge_lookups;
    budget.arc_checks -= quote.maximum_arc_checks;
    budget.relation_entries -= quote.maximum_relation_entries;
    budget.candidate_value_occurrences -=
        quote.maximum_candidate_value_occurrences;
    budget.blocks -= quote.maximum_blocks;
    budget.semijoin_entry_visits -=
        quote.maximum_semijoin_entry_visits;
    budget.index_entries -= quote.maximum_index_entries;
    budget.payload_memory_bytes -= quote.maximum_payload_bytes;
    --budget.candidate_tree_tokens;
    result.reservation->active_ = true;
    return result;
}

CandidateBuildReservationAttempt tryReserveCandidateBuild(
    const CandidateBuildQuote& quote,
    CandidateBuildLedger& ledger,
    ExistingCapacityAdmissionSealV1& admission_seal) {
    CandidateBuildReservationAttempt result;
    std::lock_guard<std::mutex> lock(ledger.state_->mutex);
    result.fit = evaluateCandidateBuildBudget(
        quote, ledger.state_->remaining);
    if (!result.fit.fits) return result;
    if (!admission_seal.consumableFor(quote)) {
        result.fit.fits = false;
        result.fit.reason =
            CandidateBuildBudgetFitReason::GlobalMemoryAdmission;
        return result;
    }

#ifdef DSM_TCC_V3_TESTING
    if (g_candidate_reservation_fail_next.exchange(false)) {
        throw std::bad_alloc();
    }
#endif
    result.reservation.reset(
        new CandidateBuildReservation(quote, ledger.state_));

    // No throwing operation follows the one-shot transition. E and H remain
    // uncharged guards; only the exact candidate quote is debited.
    admission_seal.consume();
    CandidateBuildBudget& budget = ledger.state_->remaining;
    budget.adjacency_probes -= quote.adjacency_probes;
    budget.reverse_edge_lookups -= quote.reverse_edge_lookups;
    budget.arc_checks -= quote.maximum_arc_checks;
    budget.relation_entries -= quote.maximum_relation_entries;
    budget.candidate_value_occurrences -=
        quote.maximum_candidate_value_occurrences;
    budget.blocks -= quote.maximum_blocks;
    budget.semijoin_entry_visits -=
        quote.maximum_semijoin_entry_visits;
    budget.index_entries -= quote.maximum_index_entries;
    budget.payload_memory_bytes -= quote.maximum_payload_bytes;
    --budget.candidate_tree_tokens;
    result.reservation->active_ = true;
    return result;
}

#ifdef DSM_TCC_V3_TESTING
void setCandidateBuildReservationAllocationFailureForTesting(
    bool fail_next) noexcept {
    g_candidate_reservation_fail_next.store(fail_next);
}
#endif

CandidateCover TDTree::exportCandidateCover() const {
    if (k_threshold < 1) {
        throw std::logic_error(
            "candidate cover requires a positive minimum duration");
    }
    if (!snapshot_owner_ && !dataGraphIsValidForQuote(G)) {
        throw std::logic_error(
            "cannot export candidate cover from an invalid temporal graph");
    }
    if (snapshot_owner_
            ? !queryDecompositionTopologyIsCanonical(
                  *snapshot_owner_, QD)
            : !queryDecompositionTopologyIsCanonical(Q, QD)) {
        throw std::logic_error(
            "cannot export candidate cover from inconsistent "
            "decomposition topology");
    }
    const FixedPlan incumbent_plan = snapshot_owner_
        ? makeFixedPlan(*snapshot_owner_, QD)
        : makeFixedPlan(Q, QD);
    const FixedPlanValidation validation = snapshot_owner_
        ? validateFixedPlan(*snapshot_owner_, incumbent_plan)
        : validateFixedPlan(Q, incumbent_plan);
    if (!validation.valid) {
        throw std::logic_error(
            std::string("cannot export candidate cover from an illegal plan: ") +
            fixedPlanValidationStatusName(validation.status));
    }
    if (nodes.size() != static_cast<std::size_t>(Q.num_vertices)) {
        throw std::logic_error(
            "cannot export candidate cover from an incomplete TD-tree");
    }

    std::vector<std::vector<int>> domains(nodes.size());
    std::uint64_t source_relation_entries = 0;
    for (std::size_t query_vertex = 0;
         query_vertex < nodes.size();
         ++query_vertex) {
        const TDTreeNode& node = nodes[query_vertex];
        const bool expected_root =
            static_cast<int>(query_vertex) == QD.root;
        if (node.query_vertex_id !=
                static_cast<int>(query_vertex) ||
            node.isRoot != expected_root) {
            throw std::logic_error(
                "cannot export candidate cover from inconsistent nodes");
        }

        auto& domain_values = domains[query_vertex];
        if (node.isRoot) {
            std::uint64_t entry_count = 0;
            if (!sizeToU64(node.root_candidates.size(), entry_count) ||
                !checkedAccumulate(
                    source_relation_entries,
                    entry_count)) {
                throw std::overflow_error(
                    "candidate-cover source-entry count overflow");
            }
            domain_values = node.root_candidates;
        } else {
            int previous_parent = -1;
            bool have_previous_parent = false;
            for (const TDTreeBlock& block : node.blocks) {
                if ((have_previous_parent &&
                     block.v_par <= previous_parent) ||
                    node.findBlock(block.v_par) != &block ||
                    !isStrictlySortedUnique(block.V_cand)) {
                    throw std::logic_error(
                        "cannot export candidate cover before block "
                        "normalization");
                }
                previous_parent = block.v_par;
                have_previous_parent = true;
                std::uint64_t entry_count = 0;
                if (!sizeToU64(block.V_cand.size(), entry_count) ||
                    !checkedAccumulate(
                        source_relation_entries,
                        entry_count)) {
                    throw std::overflow_error(
                        "candidate-cover source-entry count overflow");
                }
                domain_values.insert(
                    domain_values.end(),
                    block.V_cand.begin(),
                    block.V_cand.end());
            }
        }
        std::sort(domain_values.begin(), domain_values.end());
        domain_values.erase(
            std::unique(domain_values.begin(), domain_values.end()),
            domain_values.end());
        for (int data_vertex : domain_values) {
            if (data_vertex < 0 || data_vertex >= G.num_vertices) {
                throw std::logic_error(
                    "candidate cover contains an out-of-range data vertex");
            }
        }
    }

    if (snapshot_owner_) {
        return CandidateCover(
            std::move(domains),
            snapshot_owner_,
            k_threshold,
            incumbent_plan.fingerprint,
            source_relation_entries);
    }
    return CandidateCover(
        std::move(domains), &G, &Q,
        temporalGraphContentFingerprint(G),
        queryGraphContentFingerprint(Q), k_threshold,
        incumbent_plan.fingerprint, source_relation_entries);
}
