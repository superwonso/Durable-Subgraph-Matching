#include "TDTree.h"

#include "validated_graph_snapshot.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace {

struct MappingDigestAccumulator {
    std::uint64_t xor_value = 0;
    std::uint64_t sum_value = 0;

    void add(std::uint64_t value) noexcept {
        xor_value ^= value;
        sum_value += value;
    }
};

std::uint64_t mixMappingDigest(std::uint64_t value) noexcept {
    value += UINT64_C(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30U)) *
        UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27U)) *
        UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31U);
}

std::uint64_t mappingDigestHash(
    const Graph& graph,
    const std::vector<int>& mapping) noexcept {
    std::uint64_t hash = UINT64_C(0x6a09e667f3bcc909);
    for (std::size_t query_vertex = 0;
         query_vertex < mapping.size();
         ++query_vertex) {
        const std::uint64_t external =
            static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(
                    graph.externalId(mapping[query_vertex])));
        hash = mixMappingDigest(
            hash ^ mixMappingDigest(
                external +
                static_cast<std::uint64_t>(query_vertex) *
                    UINT64_C(0x9e3779b97f4a7c15)));
    }
    return hash;
}

void appendFingerprintWord(
    std::uint64_t& hash,
    std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & UINT64_C(0xff);
        hash *= UINT64_C(1099511628211);
    }
}

bool matchesCorrectedPaperIncumbent(
    const Graph& data,
    const Graph& query,
    const QueryDecomposition& decomposition) noexcept {
    constexpr std::string_view kCorrectedFormulaId =
        "label_population_over_product_query_degree_mean_vertex_activity";
    if ((decomposition.selectivity_mode != SelectivityMode::Paper &&
         decomposition.selectivity_mode != SelectivityMode::Intended) ||
        std::string_view(selectivityFormulaId(
            decomposition.selectivity_mode)) != kCorrectedFormulaId ||
        data.num_vertices < 0 ||
        data.vertex_labels.size() !=
            static_cast<std::size_t>(data.num_vertices) ||
        data.vertex_active_durations.size() !=
            static_cast<std::size_t>(data.num_vertices)) {
        return false;
    }

    try {
        std::array<std::size_t, kLabelCount> label_counts{};
        std::array<long long, kLabelCount> duration_sums{};
        for (std::size_t vertex = 0;
             vertex < data.vertex_labels.size();
             ++vertex) {
            const Label label = data.vertex_labels[vertex];
            const int duration = data.vertex_active_durations[vertex];
            if (label >= kLabelCount || duration < 0 ||
                duration_sums[label] >
                    std::numeric_limits<long long>::max() - duration) {
                return false;
            }
            if (label_counts[label] ==
                std::numeric_limits<std::size_t>::max()) {
                return false;
            }
            ++label_counts[label];
            duration_sums[label] += duration;
        }
        std::array<double, kLabelCount> mean_activity{};
        for (std::size_t label = 0; label < kLabelCount; ++label) {
            if (label_counts[label] != 0) {
                mean_activity[label] =
                    static_cast<double>(duration_sums[label]) /
                    static_cast<double>(label_counts[label]);
            }
        }
        const QueryDecomposition expected = decomposeQuery(
            query,
            label_counts,
            mean_activity,
            SelectivityMode::Paper);
        return expected.connected == decomposition.connected &&
            expected.root == decomposition.root &&
            expected.dfs_order == decomposition.dfs_order &&
            expected.parent == decomposition.parent &&
            expected.spanning_tree_adj ==
                decomposition.spanning_tree_adj &&
            expected.non_tree_edges == decomposition.non_tree_edges &&
            expected.vertex_labels == decomposition.vertex_labels &&
            expected.temporal_selectivity ==
                decomposition.temporal_selectivity;
    } catch (...) {
        return false;
    }
}

bool checkedCapacityAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return false;
    }
    result = lhs + rhs;
    return true;
}

bool checkedCapacityMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) noexcept {
    if (lhs != 0 &&
        rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return false;
    }
    result = lhs * rhs;
    return true;
}

bool sizeToCapacityU64(
    std::size_t value,
    std::uint64_t& result) noexcept {
    if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
        if (value > static_cast<std::size_t>(
                std::numeric_limits<std::uint64_t>::max())) {
            return false;
        }
    }
    result = static_cast<std::uint64_t>(value);
    return true;
}

bool checkedCapacityAccumulate(
    std::uint64_t& destination,
    std::uint64_t value) noexcept {
    std::uint64_t result = 0;
    if (!checkedCapacityAdd(destination, value, result)) return false;
    destination = result;
    return true;
}

bool checkedCapacityAccumulateSize(
    std::uint64_t& destination,
    std::size_t value) noexcept {
    std::uint64_t converted = 0;
    return sizeToCapacityU64(value, converted) &&
        checkedCapacityAccumulate(destination, converted);
}

bool checkedCapacityAccumulateProduct(
    std::uint64_t& destination,
    std::size_t count,
    std::size_t width) noexcept {
    std::uint64_t count_u64 = 0;
    std::uint64_t width_u64 = 0;
    std::uint64_t bytes = 0;
    return sizeToCapacityU64(count, count_u64) &&
        sizeToCapacityU64(width, width_u64) &&
        checkedCapacityMultiply(count_u64, width_u64, bytes) &&
        checkedCapacityAccumulate(destination, bytes);
}

} // namespace

#ifdef DSM_TCC_V3_TESTING
bool checkedTDTreeCapacityAddProductForTesting(
    std::uint64_t initial,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result) noexcept {
    std::uint64_t product = 0;
    std::uint64_t sum = 0;
    if (!checkedCapacityMultiply(count, width, product) ||
        !checkedCapacityAdd(initial, product, sum)) {
        return false;
    }
    result = sum;
    return true;
}
#endif

const char* matchOutputModeName(MatchOutputMode mode) {
    return mode == MatchOutputMode::CountOnly ? "count-only" : "full";
}

bool CheckedMatchCounter::increment() {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        overflowed = true;
        return false;
    }
    ++value;
    return true;
}

const TDTreeBlock* TDTreeNode::findBlock(int parent_vertex) const {
    const auto found = std::lower_bound(
        blocks.begin(), blocks.end(), parent_vertex,
        [](const TDTreeBlock& block, int candidate_parent) {
            return block.v_par < candidate_parent;
        });
    return found == blocks.end() || found->v_par != parent_vertex
        ? nullptr
        : &*found;
}

void TDTreeNode::rebuildBlockIndex() {
    std::sort(blocks.begin(), blocks.end(), [](const TDTreeBlock& lhs, const TDTreeBlock& rhs) {
        return lhs.v_par < rhs.v_par;
    });

    std::vector<TDTreeBlock> normalized;
    normalized.reserve(blocks.size());
    for (auto& block : blocks) {
        if (!normalized.empty() && normalized.back().v_par == block.v_par) {
            auto& candidates = normalized.back().V_cand;
            candidates.insert(
                candidates.end(), block.V_cand.begin(), block.V_cand.end());
        } else {
            normalized.push_back(std::move(block));
        }
    }
    for (auto& block : normalized) {
        std::sort(block.V_cand.begin(), block.V_cand.end());
        block.V_cand.erase(
            std::unique(block.V_cand.begin(), block.V_cand.end()),
            block.V_cand.end());
    }
    blocks.swap(normalized);
}

TDTree::TDTree(
    const Graph& temporal_graph,
    const Graph& query_graph,
    const QueryDecomposition& decomposition,
    int minimum_duration)
    : snapshot_owner_(),
      G(temporal_graph),
      Q(query_graph),
      QD(decomposition),
      k_threshold(minimum_duration) {
    if (k_threshold < 1) {
        throw std::invalid_argument(
            "TD-tree minimum duration must be positive");
    }
    if (!queryDecompositionTopologyIsCanonical(Q, QD)) {
        throw std::invalid_argument(
            "TD-tree requires a canonical complete query decomposition");
    }
    build();
}

TDTree::TDTree(
    const Graph& temporal_graph,
    const Graph& query_graph,
    QueryDecomposition decomposition,
    int minimum_duration,
    CoverAwareConstructionTag)
    : snapshot_owner_(),
      G(temporal_graph),
      Q(query_graph),
      QD(std::move(decomposition)),
      k_threshold(minimum_duration),
      sealed_flat_candidate_(true) {}

TDTree::TDTree(
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
    QueryDecomposition decomposition,
    int minimum_duration,
    bool sealed_flat_candidate)
    : snapshot_owner_(std::move(snapshot)),
      G(snapshot_owner_->data()),
      Q(snapshot_owner_->query()),
      QD(std::move(decomposition)),
      k_threshold(minimum_duration),
      sealed_flat_candidate_(sealed_flat_candidate) {
    if (k_threshold < 1) {
        throw std::invalid_argument(
            "TD-tree minimum duration must be positive");
    }
    if (!snapshot_owner_->receipt().complete() ||
        !queryDecompositionTopologyIsCanonical(
            *snapshot_owner_, QD)) {
        throw std::invalid_argument(
            "validated TD-tree requires one complete snapshot and "
            "canonical decomposition");
    }
    if (!sealed_flat_candidate_ &&
        matchesCorrectedPaperIncumbent(G, Q, QD)) {
        corrected_paper_incumbent_origin_ = true;
        corrected_paper_incumbent_plan_fingerprint_ =
            fixedPlanFingerprint(
                snapshot_owner_->queryFingerprint(),
                QD.root,
                QD.dfs_order,
                QD.parent);
    }
    if (!sealed_flat_candidate_) build();
}

std::unique_ptr<TDTree> TDTree::buildValidatedIncumbent(
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot,
    const QueryDecomposition& decomposition,
    int minimum_duration) {
    if (!snapshot) {
        throw std::invalid_argument(
            "validated incumbent requires a snapshot");
    }
    return std::unique_ptr<TDTree>(new TDTree(
        std::move(snapshot),
        decomposition,
        minimum_duration,
        false));
}

bool TDTree::hasValidatedSnapshotAuthority() const noexcept {
    return snapshot_owner_ != nullptr;
}

std::uint64_t TDTree::validationSnapshotToken() const noexcept {
    return snapshot_owner_ ? snapshot_owner_->snapshotToken() : 0;
}

bool TDTree::isBoundToValidatedSnapshot(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot)
    const noexcept {
    return sharesValidatedGraphSnapshot(
        snapshot_owner_, snapshot);
}

bool TDTree::hasCorrectedPaperIncumbentOrigin(
    const FixedPlan& incumbent_plan) const noexcept {
    return corrected_paper_incumbent_origin_ &&
        snapshot_owner_ != nullptr &&
        incumbent_plan.query_fingerprint ==
            snapshot_owner_->queryFingerprint() &&
        incumbent_plan.fingerprint ==
            corrected_paper_incumbent_plan_fingerprint_;
}

void TDTree::build() {
    initializeNodes();
    initializeQueryRequirements();
    fillRoot();
    buildCandidateRelations();

    // A bottom-up semijoin removes unsupported parents. The following
    // top-down semijoin removes orphaned descendant blocks in linear time.
    trimBottomUp();
    trimTopDown();
    rebuildBlockIndexes();
}

void TDTree::initializeNodes() {
    nodes.resize(static_cast<std::size_t>(Q.num_vertices));
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        node.query_vertex_id = query_vertex;
        node.isRoot = query_vertex == QD.root;
        node.isLeaf = QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)].empty();
    }
}

void TDTree::initializeQueryRequirements() {
    query_out_neighbor_label_requirements.assign(static_cast<std::size_t>(Q.num_vertices), {});
    query_in_neighbor_label_requirements.assign(static_cast<std::size_t>(Q.num_vertices), {});
    for (int query_vertex = 0; query_vertex < Q.num_vertices; ++query_vertex) {
        auto& out_requirements =
            query_out_neighbor_label_requirements[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : Q.adj[static_cast<std::size_t>(query_vertex)]) {
            const Label neighbor_label = Q.vertex_labels[static_cast<std::size_t>(edge.to)];
            if (neighbor_label < kLabelCount) ++out_requirements[neighbor_label];
        }
        auto& in_requirements =
            query_in_neighbor_label_requirements[static_cast<std::size_t>(query_vertex)];
        for (const auto& edge : Q.in_adj[static_cast<std::size_t>(query_vertex)]) {
            const Label neighbor_label = Q.vertex_labels[static_cast<std::size_t>(edge.to)];
            if (neighbor_label < kLabelCount) ++in_requirements[neighbor_label];
        }
    }
}

bool TDTree::isDataVertexCandidate(int data_vertex, int query_vertex) const {
    if (data_vertex < 0 || query_vertex < 0 ||
        data_vertex >= G.num_vertices || query_vertex >= Q.num_vertices) {
        return false;
    }

    const std::size_t data_index = static_cast<std::size_t>(data_vertex);
    const std::size_t query_index = static_cast<std::size_t>(query_vertex);
    if (G.vertex_labels[data_index] != Q.vertex_labels[query_index]) return false;
    if (G.adj[data_index].size() < Q.adj[query_index].size()) return false;
    if (G.in_adj[data_index].size() < Q.in_adj[query_index].size()) return false;
    if (data_index >= G.vertex_active_durations.size() ||
        G.vertex_active_durations[data_index] < k_threshold) {
        return false;
    }

    const auto& out_available = G.neighbor_label_counts[data_index];
    const auto& out_required = query_out_neighbor_label_requirements[query_index];
    const auto& in_available = G.incoming_neighbor_label_counts[data_index];
    const auto& in_required = query_in_neighbor_label_requirements[query_index];
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (out_available[label] < out_required[label] ||
            in_available[label] < in_required[label]) {
            return false;
        }
    }
    return true;
}

void TDTree::fillRoot() {
    if (QD.root < 0 || QD.root >= Q.num_vertices) return;
    auto& root = nodes[static_cast<std::size_t>(QD.root)];
    root.root_candidates.reserve(static_cast<std::size_t>(G.num_vertices) / kLabelCount + 1);
    for (int data_vertex = 0; data_vertex < G.num_vertices; ++data_vertex) {
        if (isDataVertexCandidate(data_vertex, QD.root)) {
            root.root_candidates.push_back(data_vertex);
        }
    }
}

bool TDTree::passesAvailableNonTreeConstraints(
    int data_vertex,
    int query_vertex,
    const std::vector<int>& order_position,
    const std::vector<std::vector<std::uint8_t>>& candidate_flags) const {
    for (const auto& non_tree_edge : QD.non_tree_edges) {
        int other_query_vertex = -1;
        const std::vector<Edge>* candidate_neighbors = nullptr;
        if (non_tree_edge.first == query_vertex) {
            other_query_vertex = non_tree_edge.second;
            candidate_neighbors = &G.adj[static_cast<std::size_t>(data_vertex)];
        } else if (non_tree_edge.second == query_vertex) {
            other_query_vertex = non_tree_edge.first;
            candidate_neighbors = &G.in_adj[static_cast<std::size_t>(data_vertex)];
        } else {
            continue;
        }

        if (order_position[static_cast<std::size_t>(other_query_vertex)] >=
            order_position[static_cast<std::size_t>(query_vertex)]) {
            continue;
        }

        bool found_compatible_neighbor = false;
        for (const auto& edge_ref : *candidate_neighbors) {
            if (candidate_flags[static_cast<std::size_t>(other_query_vertex)]
                               [static_cast<std::size_t>(edge_ref.to)] == 0) {
                continue;
            }
            const auto& temporal_edge = G.temporal_edges[static_cast<std::size_t>(edge_ref.temporal_edge_id)];
            if (hasMinimumConsecutiveDuration(temporal_edge, k_threshold)) {
                found_compatible_neighbor = true;
                break;
            }
        }
        if (!found_compatible_neighbor) return false;
    }
    return true;
}

void TDTree::buildCandidateRelations() {
    if (QD.root < 0 || !QD.connected) return;

    std::vector<int> order_position(static_cast<std::size_t>(Q.num_vertices), -1);
    for (std::size_t i = 0; i < QD.dfs_order.size(); ++i) {
        order_position[static_cast<std::size_t>(QD.dfs_order[i])] = static_cast<int>(i);
    }

    std::vector<std::vector<std::uint8_t>> candidate_flags(
        static_cast<std::size_t>(Q.num_vertices),
        std::vector<std::uint8_t>(static_cast<std::size_t>(G.num_vertices), 0));
    std::vector<std::vector<int>> candidate_lists(static_cast<std::size_t>(Q.num_vertices));

    auto& root_candidates = nodes[static_cast<std::size_t>(QD.root)].root_candidates;
    candidate_lists[static_cast<std::size_t>(QD.root)] = root_candidates;
    for (int candidate : root_candidates) {
        candidate_flags[static_cast<std::size_t>(QD.root)][static_cast<std::size_t>(candidate)] = 1;
    }

    for (std::size_t order_index = 1; order_index < QD.dfs_order.size(); ++order_index) {
        const int query_vertex = QD.dfs_order[order_index];
        const int parent_query_vertex = QD.parent[static_cast<std::size_t>(query_vertex)];
        if (parent_query_vertex < 0) continue;
        const bool parent_to_child =
            GraphUtils::hasEdge(Q.adj, parent_query_vertex, query_vertex);
        const bool child_to_parent =
            GraphUtils::hasEdge(Q.adj, query_vertex, parent_query_vertex);
        if (!parent_to_child && !child_to_parent) continue;

        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        const auto& parent_candidates = candidate_lists[static_cast<std::size_t>(parent_query_vertex)];
        node.blocks.reserve(parent_candidates.size());

        for (int parent_data_vertex : parent_candidates) {
            TDTreeBlock block;
            block.v_par = parent_data_vertex;
            const auto& expansion_edges = parent_to_child
                ? G.adj[static_cast<std::size_t>(parent_data_vertex)]
                : G.in_adj[static_cast<std::size_t>(parent_data_vertex)];
            for (const auto& edge_ref : expansion_edges) {
                const int candidate = edge_ref.to;
                if (!isDataVertexCandidate(candidate, query_vertex)) continue;

                const TemporalEdge* forward_edge = parent_to_child
                    ? G.findTemporalEdge(parent_data_vertex, candidate)
                    : nullptr;
                const TemporalEdge* reverse_edge = child_to_parent
                    ? G.findTemporalEdge(candidate, parent_data_vertex)
                    : nullptr;
                if ((parent_to_child &&
                     (forward_edge == nullptr || !hasMinimumConsecutiveDuration(
                         *forward_edge, k_threshold))) ||
                    (child_to_parent &&
                     (reverse_edge == nullptr || !hasMinimumConsecutiveDuration(
                         *reverse_edge, k_threshold)))) {
                    continue;
                }
                if (!passesAvailableNonTreeConstraints(
                        candidate, query_vertex, order_position, candidate_flags)) {
                    continue;
                }

                block.V_cand.push_back(candidate);
                auto& flag = candidate_flags[static_cast<std::size_t>(query_vertex)]
                                            [static_cast<std::size_t>(candidate)];
                if (flag == 0) {
                    flag = 1;
                    candidate_lists[static_cast<std::size_t>(query_vertex)].push_back(candidate);
                }
            }

            if (!block.V_cand.empty()) node.blocks.push_back(std::move(block));
        }
        std::sort(
            candidate_lists[static_cast<std::size_t>(query_vertex)].begin(),
            candidate_lists[static_cast<std::size_t>(query_vertex)].end());
    }
}

void TDTree::trimBottomUp() {
    for (auto order_it = QD.dfs_order.rbegin(); order_it != QD.dfs_order.rend(); ++order_it) {
        const int query_vertex = *order_it;
        const auto& children = QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)];
        if (children.empty()) continue;

        std::unordered_map<int, int> support_count;
        std::size_t support_entries = 0;
        for (int child : children) support_entries += nodes[static_cast<std::size_t>(child)].blocks.size();
        support_count.reserve(support_entries);
        for (int child : children) {
            const auto& child_blocks = nodes[static_cast<std::size_t>(child)].blocks;
            std::unordered_map<int, bool> seen_parent;
            seen_parent.reserve(child_blocks.size());
            for (const auto& block : child_blocks) {
                if (!block.V_cand.empty()) seen_parent.emplace(block.v_par, true);
            }
            for (const auto& entry : seen_parent) ++support_count[entry.first];
        }

        const int required_support = static_cast<int>(children.size());
        auto supported = [&](int candidate) {
            const auto found = support_count.find(candidate);
            return found != support_count.end() && found->second == required_support;
        };

        auto& node = nodes[static_cast<std::size_t>(query_vertex)];
        if (node.isRoot) {
            node.root_candidates.erase(
                std::remove_if(
                    node.root_candidates.begin(), node.root_candidates.end(),
                    [&](int candidate) { return !supported(candidate); }),
                node.root_candidates.end());
        } else {
            for (auto& block : node.blocks) {
                block.V_cand.erase(
                    std::remove_if(
                        block.V_cand.begin(), block.V_cand.end(),
                        [&](int candidate) { return !supported(candidate); }),
                    block.V_cand.end());
            }
            node.blocks.erase(
                std::remove_if(
                    node.blocks.begin(), node.blocks.end(),
                    [](const TDTreeBlock& block) { return block.V_cand.empty(); }),
                node.blocks.end());
        }
    }
}

void TDTree::trimTopDown() {
    std::vector<int> candidate_marks(static_cast<std::size_t>(G.num_vertices), 0);
    int mark = 0;
    for (int query_vertex : QD.dfs_order) {
        if (mark == std::numeric_limits<int>::max()) {
            std::fill(candidate_marks.begin(), candidate_marks.end(), 0);
            mark = 0;
        }
        ++mark;

        const auto parent_candidates = uniqueCandidates(nodes[static_cast<std::size_t>(query_vertex)]);
        for (int candidate : parent_candidates) {
            candidate_marks[static_cast<std::size_t>(candidate)] = mark;
        }

        for (int child : QD.spanning_tree_adj[static_cast<std::size_t>(query_vertex)]) {
            auto& child_blocks = nodes[static_cast<std::size_t>(child)].blocks;
            child_blocks.erase(
                std::remove_if(
                    child_blocks.begin(), child_blocks.end(),
                    [&](const TDTreeBlock& block) {
                        return candidate_marks[static_cast<std::size_t>(block.v_par)] != mark;
                    }),
                child_blocks.end());
        }
    }
}

void TDTree::rebuildBlockIndexes() {
    for (auto& node : nodes) node.rebuildBlockIndex();
}

std::vector<int> TDTree::uniqueCandidates(const TDTreeNode& node) const {
    if (node.isRoot) return node.root_candidates;

    std::vector<int> candidates;
    for (const auto& block : node.blocks) {
        candidates.insert(candidates.end(), block.V_cand.begin(), block.V_cand.end());
    }
    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    return candidates;
}

std::size_t TDTree::uniqueCandidateCount(const TDTreeNode& node) const {
    return uniqueCandidates(node).size();
}

std::size_t TDTree::candidateRelationCount() const {
    std::size_t count = 0;
    for (const auto& node : nodes) {
        if (node.isRoot) {
            count += node.root_candidates.size();
        } else {
            for (const auto& block : node.blocks) count += block.V_cand.size();
        }
    }
    return count;
}

const std::vector<int>& TDTree::rootCandidates() const {
    static const std::vector<int> empty;
    if (QD.root < 0 ||
        QD.root >= static_cast<int>(nodes.size())) {
        return empty;
    }
    return nodes[static_cast<std::size_t>(QD.root)]
        .root_candidates;
}

const std::vector<int>* TDTree::candidatesForParent(
    int query_vertex,
    int parent_data_vertex) const {
    if (query_vertex < 0 ||
        query_vertex >= static_cast<int>(nodes.size())) {
        return nullptr;
    }
    const TDTreeBlock* block =
        nodes[static_cast<std::size_t>(query_vertex)]
            .findBlock(parent_data_vertex);
    return block == nullptr ? nullptr : &block->V_cand;
}

std::size_t TDTree::relationEntryCount(
    int query_vertex) const {
    if (query_vertex < 0 ||
        query_vertex >= static_cast<int>(nodes.size())) {
        return 0;
    }
    const auto& node =
        nodes[static_cast<std::size_t>(query_vertex)];
    if (node.isRoot) return node.root_candidates.size();
    std::size_t count = 0;
    for (const auto& block : node.blocks) {
        count += block.V_cand.size();
    }
    return count;
}

std::size_t TDTree::blockCount(int query_vertex) const {
    if (query_vertex < 0 ||
        query_vertex >= static_cast<int>(nodes.size())) {
        return 0;
    }
    const auto& node =
        nodes[static_cast<std::size_t>(query_vertex)];
    return node.isRoot
        ? (node.root_candidates.empty() ? 0 : 1)
        : node.blocks.size();
}

std::size_t TDTree::queryCandidateCount(
    int query_vertex) const {
    if (query_vertex < 0 ||
        query_vertex >= static_cast<int>(nodes.size())) {
        return 0;
    }
    return uniqueCandidateCount(
        nodes[static_cast<std::size_t>(query_vertex)]);
}

void TDTree::print_res() const {
    std::cout << "TD-tree candidate summary:\n";
    for (const auto& node : nodes) {
        std::size_t relation_count = 0;
        if (node.isRoot) {
            relation_count = node.root_candidates.size();
        } else {
            for (const auto& block : node.blocks) relation_count += block.V_cand.size();
        }
        std::cout << "  q" << node.query_vertex_id
                  << " label=" << labelToString(Q.vertex_labels[static_cast<std::size_t>(node.query_vertex_id)])
                  << " blocks=" << node.blocks.size()
                  << " unique_candidates=" << uniqueCandidateCount(node)
                  << " relation_entries=" << relation_count << '\n';
    }
}

CheckedMatchCounter TDTree::enumerateMatches(
    std::ostream& output,
    MatchOutputMode mode,
    std::size_t requested_threads,
    bool collect_result_digest,
    MatchSummary& summary) const {
    summary.threads_requested =
        std::max<std::size_t>(1, requested_threads);
    summary.threads_used = 1;
    summary.workers_with_tasks = 0;
    summary.root_tasks_claimed = 0;
    summary.worker_root_tasks_min = 0;
    summary.worker_root_tasks_max = 0;

    CheckedMatchCounter empty_counter;
    if (QD.root < 0 || !QD.connected || QD.dfs_order.empty()) {
        return empty_counter;
    }

    struct SearchContext {
        CheckedMatchCounter counter;
        MappingDigestAccumulator digest;
        std::vector<int> mapping;
        std::vector<std::uint8_t> used_data_vertices;
        std::uint64_t dfs_states = 0;
        std::uint64_t generated_candidates = 0;
        std::uint64_t accepted_candidates = 0;
        std::uint64_t completed_matches = 0;
        std::uint64_t pruned_injectivity = 0;
        std::uint64_t pruned_temporal_support = 0;
        std::vector<std::uint64_t>
            generated_candidates_by_depth;
        std::vector<std::uint64_t>
            accepted_candidates_by_depth;
        std::vector<std::uint64_t>
            pruned_injectivity_by_depth;
        std::vector<std::uint64_t>
            pruned_temporal_support_by_depth;
    };

    auto make_context = [&]() {
        SearchContext context;
        context.mapping.assign(
            static_cast<std::size_t>(Q.num_vertices), -1);
        context.used_data_vertices.assign(
            static_cast<std::size_t>(G.num_vertices), 0);
        context.generated_candidates_by_depth.assign(
            QD.dfs_order.size(), 0);
        context.accepted_candidates_by_depth.assign(
            QD.dfs_order.size(), 0);
        context.pruned_injectivity_by_depth.assign(
            QD.dfs_order.size(), 0);
        context.pruned_temporal_support_by_depth.assign(
            QD.dfs_order.size(), 0);
        return context;
    };

    const auto& root_candidates =
        nodes[static_cast<std::size_t>(QD.root)].root_candidates;
    summary.root_candidate_count = root_candidates.size();
    summary.generated_candidates_by_depth.assign(
        QD.dfs_order.size(), 0);
    summary.accepted_candidates_by_depth.assign(
        QD.dfs_order.size(), 0);
    summary.pruned_injectivity_by_depth.assign(
        QD.dfs_order.size(), 0);
    summary.pruned_temporal_support_by_depth.assign(
        QD.dfs_order.size(), 0);

    auto merge_statistics = [&](const SearchContext& context) {
        if (collect_result_digest) {
            summary.mapping_digest_xor ^=
                context.digest.xor_value;
            summary.mapping_digest_sum +=
                context.digest.sum_value;
        }
        summary.dfs_states += context.dfs_states;
        summary.generated_candidates +=
            context.generated_candidates;
        summary.accepted_candidates +=
            context.accepted_candidates;
        summary.completed_matches +=
            context.completed_matches;
        summary.pruned_injectivity +=
            context.pruned_injectivity;
        summary.pruned_temporal_support +=
            context.pruned_temporal_support;
        for (std::size_t depth = 0;
             depth < QD.dfs_order.size();
             ++depth) {
            summary.generated_candidates_by_depth[depth] +=
                context.generated_candidates_by_depth[depth];
            summary.accepted_candidates_by_depth[depth] +=
                context.accepted_candidates_by_depth[depth];
            summary.pruned_injectivity_by_depth[depth] +=
                context.pruned_injectivity_by_depth[depth];
            summary.pruned_temporal_support_by_depth[depth] +=
                context.pruned_temporal_support_by_depth[depth];
        }
    };

    auto enumerate_root = [&](std::size_t root_index,
                              SearchContext& context,
                              std::ostream* match_rows) {
        auto dfs = [&](auto&& self,
                       std::size_t depth,
                       const std::vector<TimeInterval>& current_intervals,
                       bool has_intervals) -> void {
            if (context.counter.overflowed) return;
            ++context.dfs_states;
            if (depth >= QD.dfs_order.size()) {
                if (!context.counter.increment()) return;
                ++context.completed_matches;
                if (collect_result_digest) {
                    context.digest.add(
                        mappingDigestHash(G, context.mapping));
                }
                if (match_rows != nullptr) {
                    *match_rows << "Match "
                                << (context.counter.value - 1)
                                << ": ";
                    for (int query_vertex = 0;
                         query_vertex < Q.num_vertices;
                         ++query_vertex) {
                        if (query_vertex > 0) *match_rows << ", ";
                        *match_rows << 'q' << query_vertex << '('
                                    << labelToString(
                                           Q.vertex_labels[
                                               static_cast<std::size_t>(
                                                   query_vertex)])
                                    << ")->"
                                    << G.externalId(
                                           context.mapping[
                                               static_cast<std::size_t>(
                                                   query_vertex)]);
                    }
                    *match_rows << " | active="
                                << formatIntervals(current_intervals)
                                << '\n';
                }
                return;
            }

            const int query_vertex = QD.dfs_order[depth];
            const int parent_query_vertex =
                QD.parent[static_cast<std::size_t>(query_vertex)];
            if (parent_query_vertex < 0) return;
            const int parent_data_vertex =
                context.mapping[
                    static_cast<std::size_t>(parent_query_vertex)];
            const TDTreeBlock* block =
                nodes[static_cast<std::size_t>(query_vertex)]
                    .findBlock(parent_data_vertex);
            if (block == nullptr) return;

            context.generated_candidates +=
                static_cast<std::uint64_t>(block->V_cand.size());
            context.generated_candidates_by_depth[depth] +=
                static_cast<std::uint64_t>(block->V_cand.size());
            for (int candidate : block->V_cand) {
                if (context.used_data_vertices[
                        static_cast<std::size_t>(candidate)] != 0) {
                    ++context.pruned_injectivity;
                    ++context.pruned_injectivity_by_depth[depth];
                    continue;
                }

                std::vector<TimeInterval> next_intervals =
                    current_intervals;
                bool next_has_intervals = has_intervals;
                bool valid = true;

                auto apply_temporal_edge =
                    [&](int source_data_vertex,
                        int target_data_vertex) {
                        const TemporalEdge* temporal_edge =
                            G.findTemporalEdge(
                                source_data_vertex,
                                target_data_vertex);
                        if (temporal_edge == nullptr) return false;

                        if (!next_has_intervals) {
                            next_intervals.clear();
                            for (const auto& interval :
                                 temporal_edge->activeIntervals()) {
                                if (interval.length() >= k_threshold) {
                                    next_intervals.push_back(interval);
                                }
                            }
                            next_has_intervals = true;
                        } else {
                            next_intervals =
                                intersectTimeIntervals(
                                    next_intervals,
                                    temporal_edge->activeIntervals(),
                                    k_threshold);
                        }
                        return !next_intervals.empty();
                    };

                // A self-loop appears in both adj and in_adj. Apply its
                // temporal history exactly once before checking arcs to
                // mapped vertices.
                if (GraphUtils::hasEdge(
                        Q.adj, query_vertex, query_vertex) &&
                    !apply_temporal_edge(candidate, candidate)) {
                    ++context.pruned_temporal_support;
                    ++context.pruned_temporal_support_by_depth[depth];
                    continue;
                }

                // Check every outgoing and incoming query arc whose other
                // endpoint is already mapped. Tree orientation never changes
                // arc direction.
                for (const auto& query_edge :
                     Q.adj[static_cast<std::size_t>(query_vertex)]) {
                    const int other_query_vertex = query_edge.to;
                    if (other_query_vertex == query_vertex) continue;
                    const int other_data_vertex =
                        context.mapping[
                            static_cast<std::size_t>(
                                other_query_vertex)];
                    if (other_data_vertex < 0) continue;
                    if (!apply_temporal_edge(
                            candidate, other_data_vertex)) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    ++context.pruned_temporal_support;
                    ++context.pruned_temporal_support_by_depth[depth];
                    continue;
                }
                for (const auto& query_edge :
                     Q.in_adj[static_cast<std::size_t>(query_vertex)]) {
                    const int other_query_vertex = query_edge.to;
                    if (other_query_vertex == query_vertex) continue;
                    const int other_data_vertex =
                        context.mapping[
                            static_cast<std::size_t>(
                                other_query_vertex)];
                    if (other_data_vertex < 0) continue;
                    if (!apply_temporal_edge(
                            other_data_vertex, candidate)) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    ++context.pruned_temporal_support;
                    ++context.pruned_temporal_support_by_depth[depth];
                    continue;
                }

                ++context.accepted_candidates;
                ++context.accepted_candidates_by_depth[depth];
                context.mapping[
                    static_cast<std::size_t>(query_vertex)] =
                    candidate;
                context.used_data_vertices[
                    static_cast<std::size_t>(candidate)] = 1;
                self(
                    self,
                    depth + 1,
                    next_intervals,
                    next_has_intervals);
                context.used_data_vertices[
                    static_cast<std::size_t>(candidate)] = 0;
                context.mapping[
                    static_cast<std::size_t>(query_vertex)] = -1;
                if (context.counter.overflowed) return;
            }
        };

        const int root_candidate = root_candidates[root_index];
        std::vector<TimeInterval> root_intervals;
        bool root_has_intervals = false;
        if (GraphUtils::hasEdge(Q.adj, QD.root, QD.root)) {
            const TemporalEdge* self_edge =
                G.findTemporalEdge(root_candidate, root_candidate);
            if (self_edge == nullptr) return;
            for (const auto& interval : self_edge->activeIntervals()) {
                if (interval.length() >= k_threshold) {
                    root_intervals.push_back(interval);
                }
            }
            if (root_intervals.empty()) return;
            root_has_intervals = true;
        }
        context.mapping[static_cast<std::size_t>(QD.root)] =
            root_candidate;
        context.used_data_vertices[
            static_cast<std::size_t>(root_candidate)] = 1;
        dfs(dfs, 1, root_intervals, root_has_intervals);
        context.used_data_vertices[
            static_cast<std::size_t>(root_candidate)] = 0;
        context.mapping[static_cast<std::size_t>(QD.root)] = -1;
    };

    if (mode == MatchOutputMode::Full ||
        summary.threads_requested == 1) {
        summary.threads_created = 1;
        SearchContext context = make_context();
        const std::size_t task_limit =
            context.counter.overflowed ? 0 : root_candidates.size();
        for (std::size_t root_index = 0;
             root_index < task_limit;
             ++root_index) {
            ++summary.root_tasks_claimed;
            enumerate_root(
                root_index,
                context,
                mode == MatchOutputMode::Full ? &output : nullptr);
            if (context.counter.overflowed) break;
        }
        if (summary.root_tasks_claimed > 0) {
            summary.workers_with_tasks = 1;
            summary.worker_root_tasks_min =
                summary.root_tasks_claimed;
            summary.worker_root_tasks_max =
                summary.root_tasks_claimed;
        }
        merge_statistics(context);
        return context.counter;
    }

    const std::size_t worker_count = summary.threads_requested;
    summary.threads_used = worker_count;
    std::vector<SearchContext> contexts;
    contexts.reserve(worker_count);
    for (std::size_t worker = 0;
         worker < worker_count;
         ++worker) {
        contexts.push_back(make_context());
    }
    std::vector<std::size_t> worker_root_tasks(worker_count, 0);
    std::atomic<std::size_t> next_root{
        std::min(worker_count, root_candidates.size())};
    std::atomic<bool> cancel{false};
    std::mutex exception_mutex;
    std::exception_ptr worker_exception;
    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    auto join_workers = [&]() {
        for (auto& worker : workers) {
            if (worker.joinable()) worker.join();
        }
    };

    try {
        for (std::size_t worker = 0;
             worker < worker_count;
             ++worker) {
            workers.emplace_back([&, worker]() {
                try {
                    if (worker < root_candidates.size() &&
                        !cancel.load(std::memory_order_relaxed)) {
                        ++worker_root_tasks[worker];
                        enumerate_root(
                            worker, contexts[worker], nullptr);
                        if (contexts[worker].counter.overflowed) {
                            cancel.store(
                                true, std::memory_order_relaxed);
                        }
                    }
                    while (!cancel.load(std::memory_order_relaxed)) {
                        const std::size_t root_index =
                            next_root.fetch_add(
                                1, std::memory_order_relaxed);
                        if (root_index >= root_candidates.size()) {
                            break;
                        }
                        ++worker_root_tasks[worker];
                        enumerate_root(
                            root_index, contexts[worker], nullptr);
                        if (contexts[worker].counter.overflowed) {
                            cancel.store(
                                true, std::memory_order_relaxed);
                        }
                    }
                } catch (...) {
                    {
                        std::lock_guard<std::mutex> lock(
                            exception_mutex);
                        if (!worker_exception) {
                            worker_exception =
                                std::current_exception();
                        }
                    }
                    cancel.store(true, std::memory_order_relaxed);
                }
            });
            ++summary.threads_created;
        }
    } catch (...) {
        cancel.store(true, std::memory_order_relaxed);
        join_workers();
        throw;
    }
    join_workers();
    if (worker_exception) std::rethrow_exception(worker_exception);

    summary.worker_root_tasks_min =
        std::numeric_limits<std::size_t>::max();
    for (std::size_t tasks : worker_root_tasks) {
        summary.root_tasks_claimed += tasks;
        if (tasks > 0) ++summary.workers_with_tasks;
        summary.worker_root_tasks_min =
            std::min(summary.worker_root_tasks_min, tasks);
        summary.worker_root_tasks_max =
            std::max(summary.worker_root_tasks_max, tasks);
    }

    CheckedMatchCounter total;
    for (const SearchContext& context : contexts) {
        merge_statistics(context);
        if (context.counter.overflowed ||
            context.counter.value >
                std::numeric_limits<std::uint64_t>::max() -
                    total.value) {
            total.value =
                std::numeric_limits<std::uint64_t>::max();
            total.overflowed = true;
            break;
        }
        total.value += context.counter.value;
    }
    return total;
}

MatchSummary TDTree::save_res(
    const std::string& filename,
    MatchOutputMode mode,
    std::size_t threads,
    bool collect_result_digest) const {
    MatchSummary summary;
    summary.result_digest_collected = collect_result_digest;
    summary.threads_requested =
        std::max<std::size_t>(1, threads);
    // Binary mode keeps tellp/seekp offsets stable on Windows (text mode
    // translates '\n' to CRLF and would corrupt the count placeholder).
    std::ofstream output(filename, std::ios::binary);
    if (!output.is_open()) return summary;

    output << "[Run]\nmode: " << matchOutputModeName(mode)
           << "\nselectivity_mode: "
           << selectivityModeName(QD.selectivity_mode)
           << "\nselectivity_formula_id: "
           << selectivityFormulaId(QD.selectivity_mode)
           << "\nquery_root: " << QD.root
           << "\nroot_selectivity_score: "
           << std::setprecision(17)
           << QD.temporal_selectivity[static_cast<std::size_t>(QD.root)]
           << '\n';
    if (QD.structural_candidate_counts.size() ==
            static_cast<std::size_t>(Q.num_vertices) &&
        QD.compatible_edge_counts.size() ==
            static_cast<std::size_t>(Q.num_vertices) &&
        QD.durable_compatible_edge_counts.size() ==
            static_cast<std::size_t>(Q.num_vertices) &&
        QD.durability_survival_rates.size() ==
            static_cast<std::size_t>(Q.num_vertices)) {
        const std::size_t root_index =
            static_cast<std::size_t>(QD.root);
        output
            << "root_structural_candidate_count: "
            << QD.structural_candidate_counts[root_index]
            << "\nroot_compatible_edge_count: "
            << QD.compatible_edge_counts[root_index]
            << "\nroot_durable_compatible_edge_count: "
            << QD.durable_compatible_edge_counts[root_index]
            << "\nroot_durability_survival_rate: "
            << std::setprecision(17)
            << QD.durability_survival_rates[root_index]
            << '\n';
    }
    output << "threads_requested: "
           << summary.threads_requested
           << "\nplan_order:";
    for (int query_vertex : QD.dfs_order) {
        output << ' ' << query_vertex;
    }
    output << "\nplan_parent:";
    // Canonical query-vertex order (q0, q1, ...), rather than DFS order,
    // makes cross-engine plan identity checks unambiguous.
    for (int parent : QD.parent) {
        output << ' ' << parent;
    }
    std::array<std::size_t, kLabelCount> label_counts{};
    std::array<long long, kLabelCount> duration_sums{};
    for (std::size_t data_vertex = 0;
         data_vertex < G.vertex_labels.size();
         ++data_vertex) {
        const Label label = G.vertex_labels[data_vertex];
        if (label >= kLabelCount) continue;
        ++label_counts[label];
        duration_sums[label] +=
            G.vertex_active_durations[data_vertex];
    }
    output << "\n\n[Selectivity Summary]\n";
    for (int query_vertex = 0;
         query_vertex < Q.num_vertices;
         ++query_vertex) {
        const Label label =
            Q.vertex_labels[
                static_cast<std::size_t>(query_vertex)];
        const std::size_t degree =
            Q.adj[static_cast<std::size_t>(query_vertex)].size() +
            Q.in_adj[static_cast<std::size_t>(query_vertex)].size();
        const double mean_vertex_active_snapshots =
            label < kLabelCount && label_counts[label] > 0
            ? static_cast<double>(duration_sums[label]) /
                static_cast<double>(label_counts[label])
            : 0.0;
        const auto order_it = std::find(
            QD.dfs_order.begin(), QD.dfs_order.end(),
            query_vertex);
        output << 'q' << query_vertex
               << " label=" << labelToString(label)
               << " degree=" << degree
               << " candidate_count="
               << (label < kLabelCount
                       ? label_counts[label]
                       : 0)
               << " mean_vertex_active_snapshots="
               << std::setprecision(17)
               << mean_vertex_active_snapshots
               << " score="
               << QD.temporal_selectivity[
                      static_cast<std::size_t>(
                          query_vertex)]
               ;
        if (QD.structural_candidate_counts.size() ==
                static_cast<std::size_t>(Q.num_vertices) &&
            QD.compatible_edge_counts.size() ==
                static_cast<std::size_t>(Q.num_vertices) &&
            QD.durable_compatible_edge_counts.size() ==
                static_cast<std::size_t>(Q.num_vertices) &&
            QD.durability_survival_rates.size() ==
                static_cast<std::size_t>(Q.num_vertices)) {
            const std::size_t query_index =
                static_cast<std::size_t>(query_vertex);
            output
                << " structural_candidate_count="
                << QD.structural_candidate_counts[query_index]
                << " compatible_edge_count="
                << QD.compatible_edge_counts[query_index]
                << " durable_compatible_edge_count="
                << QD.durable_compatible_edge_counts[query_index]
                << " durability_survival_rate="
                << std::setprecision(17)
                << QD.durability_survival_rates[query_index];
        }
        output << " is_root="
               << (query_vertex == QD.root
                       ? "true"
                       : "false")
               << " order_position="
               << (order_it == QD.dfs_order.end()
                       ? -1
                       : static_cast<long long>(
                             order_it -
                             QD.dfs_order.begin()))
               << '\n';
    }
    output << "\n[Candidate Summary]\n";
    for (const auto& node : nodes) {
        const auto candidates = uniqueCandidates(node);
        std::size_t relation_entries =
            node.root_candidates.size();
        for (const auto& block : node.blocks) {
            relation_entries += block.V_cand.size();
        }
        output << "q" << node.query_vertex_id
               << " label=" << labelToString(Q.vertex_labels[static_cast<std::size_t>(node.query_vertex_id)])
               << " blocks=" << node.blocks.size()
               << " candidates=" << candidates.size()
               << " relation_entries=" << relation_entries
               << " sample=";
        const std::size_t sample_size = std::min<std::size_t>(20, candidates.size());
        for (std::size_t i = 0; i < sample_size; ++i) {
            if (i > 0) output << ',';
            output << G.externalId(candidates[i]);
        }
        if (candidates.size() > sample_size) output << ",...";
        output << '\n';
    }

    output << "\n[Final Matches]\nCount: ";
    const std::streampos count_position = output.tellp();
    output << std::setw(20) << 0 << '\n';

    const auto start = std::chrono::steady_clock::now();
    const CheckedMatchCounter counter = enumerateMatches(
        output,
        mode,
        summary.threads_requested,
        collect_result_digest,
        summary);
    summary.match_count = counter.value;
    summary.count_overflow = counter.overflowed;
    summary.enumeration_milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    const std::streampos end_position = output.tellp();
    output.seekp(count_position);
    if (summary.count_overflow) {
        output << std::setw(20) << "OVERFLOW";
    } else {
        output << std::setw(20) << summary.match_count;
    }
    output.seekp(end_position);
    output << "\n[Statistics]\n"
           << "mode: " << matchOutputModeName(mode) << '\n'
           << "count_overflow: " << (summary.count_overflow ? "true" : "false") << '\n'
           << "result_digest_collected: "
           << (summary.result_digest_collected ? "true" : "false")
           << '\n'
           << "mapping_digest_xor_hex: "
           << std::hex << std::setfill('0') << std::setw(16)
           << summary.mapping_digest_xor
           << "\nmapping_digest_sum_hex: " << std::setw(16)
           << summary.mapping_digest_sum
           << std::dec << std::setfill(' ') << '\n'
           << "threads_requested: "
           << summary.threads_requested << '\n'
           << "threads_used: " << summary.threads_used << '\n'
           << "workers_with_tasks: "
           << summary.workers_with_tasks << '\n'
           << "root_tasks_claimed: "
           << summary.root_tasks_claimed << '\n'
           << "worker_root_tasks_min: "
           << summary.worker_root_tasks_min << '\n'
           << "worker_root_tasks_max: "
           << summary.worker_root_tasks_max << '\n'
           << "root_candidate_count: "
           << summary.root_candidate_count << '\n'
           << "candidate_relation_entries: " << candidateRelationCount() << '\n'
           << "dfs_states: " << summary.dfs_states << '\n'
           << "metric_schema_version: logical_extensions_v1\n"
           << "raw_extension_candidates: "
           << summary.generated_candidates << '\n'
           << "post_structural_candidates: "
           << summary.generated_candidates << '\n'
           << "generated_candidates: "
           << summary.generated_candidates << '\n'
           << "accepted_candidates: "
           << summary.accepted_candidates << '\n'
           << "accepted_extensions: "
           << summary.accepted_candidates << '\n'
           << "completed_matches: "
           << summary.completed_matches << '\n'
           << "pruned_injectivity: "
           << summary.pruned_injectivity << '\n'
           << "pruned_temporal_support: "
           << summary.pruned_temporal_support << '\n'
           << "enumeration_ms: " << summary.enumeration_milliseconds << '\n';
    output.flush();
    summary.output_written = output.good();
    return summary;
}

MatchSummary TDTree::countMatchesV3(std::size_t threads) const {
    MatchSummary summary;
    summary.result_digest_collected = true;
    summary.threads_requested = std::max<std::size_t>(1, threads);
    // CountOnly never writes to this sink.  Keeping the existing enumerator's
    // ostream reference avoids a second exact-search implementation while
    // preserving the file-free V3 boundary.
    std::ostringstream sink;
    const CheckedMatchCounter counter = enumerateMatches(
        sink,
        MatchOutputMode::CountOnly,
        summary.threads_requested,
        true,
        summary);
    summary.match_count = counter.value;
    summary.count_overflow = counter.overflowed;
    summary.output_written = false;
    summary.enumeration_milliseconds = 0;
    return summary;
}

bool TDTreeCapacityReceipt::invariantHolds() const noexcept {
    if (!complete || overflowed || object_bytes != sizeof(TDTree) ||
        node_count > node_capacity ||
        root_candidate_count > root_candidate_capacity ||
        block_count > block_capacity ||
        block_candidate_count > block_candidate_capacity ||
        query_out_requirements_count >
            query_out_requirements_capacity ||
        query_in_requirements_count >
            query_in_requirements_capacity ||
        decomposition_spanning_outer_count >
            decomposition_spanning_outer_capacity ||
        decomposition_spanning_nested_count >
            decomposition_spanning_nested_capacity ||
        decomposition_non_tree_edges_count >
            decomposition_non_tree_edges_capacity ||
        decomposition_vertex_labels_count >
            decomposition_vertex_labels_capacity ||
        decomposition_parent_count > decomposition_parent_capacity ||
        decomposition_dfs_order_count >
            decomposition_dfs_order_capacity ||
        decomposition_temporal_selectivity_count >
            decomposition_temporal_selectivity_capacity ||
        decomposition_structural_candidate_counts_count >
            decomposition_structural_candidate_counts_capacity ||
        decomposition_compatible_edge_counts_count >
            decomposition_compatible_edge_counts_capacity ||
        decomposition_durable_compatible_edge_counts_count >
            decomposition_durable_compatible_edge_counts_capacity ||
        decomposition_durability_survival_rates_count >
            decomposition_durability_survival_rates_capacity) {
        return false;
    }

    std::uint64_t expected = 0;
    const auto bytesMatch = [&](std::uint64_t capacity,
                                std::uint64_t width,
                                std::uint64_t observed) noexcept {
        return checkedCapacityMultiply(capacity, width, expected) &&
            expected == observed;
    };
    if (!bytesMatch(
            node_capacity, sizeof(TDTreeNode), nodes_bytes) ||
        !bytesMatch(
            root_candidate_capacity, sizeof(int), root_candidates_bytes) ||
        !bytesMatch(
            block_capacity, sizeof(TDTreeBlock), blocks_bytes) ||
        !bytesMatch(
            block_candidate_capacity, sizeof(int), block_candidates_bytes) ||
        !bytesMatch(
            query_out_requirements_capacity,
            sizeof(std::array<int, kLabelCount>),
            query_out_requirements_bytes) ||
        !bytesMatch(
            query_in_requirements_capacity,
            sizeof(std::array<int, kLabelCount>),
            query_in_requirements_bytes) ||
        !bytesMatch(
            decomposition_spanning_outer_capacity,
            sizeof(std::vector<int>),
            decomposition_spanning_outer_bytes) ||
        !bytesMatch(
            decomposition_spanning_nested_capacity,
            sizeof(int),
            decomposition_spanning_nested_bytes) ||
        !bytesMatch(
            decomposition_non_tree_edges_capacity,
            sizeof(std::pair<int, int>),
            decomposition_non_tree_edges_bytes) ||
        !bytesMatch(
            decomposition_vertex_labels_capacity,
            sizeof(Label),
            decomposition_vertex_labels_bytes) ||
        !bytesMatch(
            decomposition_parent_capacity,
            sizeof(int),
            decomposition_parent_bytes) ||
        !bytesMatch(
            decomposition_dfs_order_capacity,
            sizeof(int),
            decomposition_dfs_order_bytes) ||
        !bytesMatch(
            decomposition_temporal_selectivity_capacity,
            sizeof(double),
            decomposition_temporal_selectivity_bytes) ||
        !bytesMatch(
            decomposition_structural_candidate_counts_capacity,
            sizeof(std::size_t),
            decomposition_structural_candidate_counts_bytes) ||
        !bytesMatch(
            decomposition_compatible_edge_counts_capacity,
            sizeof(std::size_t),
            decomposition_compatible_edge_counts_bytes) ||
        !bytesMatch(
            decomposition_durable_compatible_edge_counts_capacity,
            sizeof(std::size_t),
            decomposition_durable_compatible_edge_counts_bytes) ||
        !bytesMatch(
            decomposition_durability_survival_rates_capacity,
            sizeof(double),
            decomposition_durability_survival_rates_bytes)) {
        return false;
    }

    std::uint64_t decomposition = 0;
    if (!checkedCapacityAccumulate(
            decomposition, decomposition_spanning_outer_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_spanning_nested_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_non_tree_edges_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_vertex_labels_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_parent_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_dfs_order_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_temporal_selectivity_bytes) ||
        !checkedCapacityAccumulate(
            decomposition,
            decomposition_structural_candidate_counts_bytes) ||
        !checkedCapacityAccumulate(
            decomposition, decomposition_compatible_edge_counts_bytes) ||
        !checkedCapacityAccumulate(
            decomposition,
            decomposition_durable_compatible_edge_counts_bytes) ||
        !checkedCapacityAccumulate(
            decomposition,
            decomposition_durability_survival_rates_bytes) ||
        decomposition != query_decomposition_bytes) {
        return false;
    }

    std::uint64_t dynamic = 0;
    if (!checkedCapacityAccumulate(dynamic, nodes_bytes) ||
        !checkedCapacityAccumulate(dynamic, root_candidates_bytes) ||
        !checkedCapacityAccumulate(dynamic, blocks_bytes) ||
        !checkedCapacityAccumulate(dynamic, block_candidates_bytes) ||
        !checkedCapacityAccumulate(
            dynamic, query_out_requirements_bytes) ||
        !checkedCapacityAccumulate(
            dynamic, query_in_requirements_bytes) ||
        !checkedCapacityAccumulate(dynamic, query_decomposition_bytes) ||
        dynamic != dynamic_bytes) {
        return false;
    }
    std::uint64_t total = 0;
    return checkedCapacityAdd(object_bytes, dynamic_bytes, total) &&
        total == total_bytes && total_bytes > 0;
}

TDTreeCapacityReceipt TDTree::capacityReceipt() const noexcept {
    TDTreeCapacityReceipt receipt;
    receipt.object_bytes = sizeof(TDTree);

    auto overflow = [&]() noexcept {
        receipt.complete = false;
        receipt.overflowed = true;
        return receipt;
    };
    const auto recordVector = [&](const auto& values,
                                  std::uint64_t& count,
                                  std::uint64_t& capacity,
                                  std::uint64_t& bytes) noexcept {
        using Value = typename std::decay_t<decltype(values)>::value_type;
        return sizeToCapacityU64(values.size(), count) &&
            sizeToCapacityU64(values.capacity(), capacity) &&
            checkedCapacityAccumulateProduct(
                bytes, values.capacity(), sizeof(Value));
    };
    if (!sizeToCapacityU64(nodes.size(), receipt.node_count) ||
        !sizeToCapacityU64(nodes.capacity(), receipt.node_capacity) ||
        !checkedCapacityAccumulateProduct(
            receipt.nodes_bytes,
            nodes.capacity(),
            sizeof(TDTreeNode)) ||
        !recordVector(
            query_out_neighbor_label_requirements,
            receipt.query_out_requirements_count,
            receipt.query_out_requirements_capacity,
            receipt.query_out_requirements_bytes) ||
        !recordVector(
            query_in_neighbor_label_requirements,
            receipt.query_in_requirements_count,
            receipt.query_in_requirements_capacity,
            receipt.query_in_requirements_bytes)) {
        return overflow();
    }

    for (const TDTreeNode& node : nodes) {
        if (!checkedCapacityAccumulateSize(
                receipt.root_candidate_count,
                node.root_candidates.size()) ||
            !checkedCapacityAccumulateSize(
                receipt.root_candidate_capacity,
                node.root_candidates.capacity()) ||
            !checkedCapacityAccumulateProduct(
                receipt.root_candidates_bytes,
                node.root_candidates.capacity(),
                sizeof(int)) ||
            !checkedCapacityAccumulateSize(
                receipt.block_count, node.blocks.size()) ||
            !checkedCapacityAccumulateSize(
                receipt.block_capacity, node.blocks.capacity()) ||
            !checkedCapacityAccumulateProduct(
                receipt.blocks_bytes,
                node.blocks.capacity(),
                sizeof(TDTreeBlock))) {
            return overflow();
        }
        for (const TDTreeBlock& block : node.blocks) {
            if (!checkedCapacityAccumulateSize(
                    receipt.block_candidate_count,
                    block.V_cand.size()) ||
                !checkedCapacityAccumulateSize(
                    receipt.block_candidate_capacity,
                    block.V_cand.capacity()) ||
                !checkedCapacityAccumulateProduct(
                    receipt.block_candidates_bytes,
                    block.V_cand.capacity(),
                    sizeof(int))) {
                return overflow();
            }
        }
    }

    if (!recordVector(
            QD.spanning_tree_adj,
            receipt.decomposition_spanning_outer_count,
            receipt.decomposition_spanning_outer_capacity,
            receipt.decomposition_spanning_outer_bytes)) {
        return overflow();
    }
    for (const std::vector<int>& children : QD.spanning_tree_adj) {
        if (!checkedCapacityAccumulateSize(
                receipt.decomposition_spanning_nested_count,
                children.size()) ||
            !checkedCapacityAccumulateSize(
                receipt.decomposition_spanning_nested_capacity,
                children.capacity()) ||
            !checkedCapacityAccumulateProduct(
                receipt.decomposition_spanning_nested_bytes,
                children.capacity(), sizeof(int))) {
            return overflow();
        }
    }
    if (!recordVector(
            QD.non_tree_edges,
            receipt.decomposition_non_tree_edges_count,
            receipt.decomposition_non_tree_edges_capacity,
            receipt.decomposition_non_tree_edges_bytes) ||
        !recordVector(
            QD.vertex_labels,
            receipt.decomposition_vertex_labels_count,
            receipt.decomposition_vertex_labels_capacity,
            receipt.decomposition_vertex_labels_bytes) ||
        !recordVector(
            QD.parent,
            receipt.decomposition_parent_count,
            receipt.decomposition_parent_capacity,
            receipt.decomposition_parent_bytes) ||
        !recordVector(
            QD.dfs_order,
            receipt.decomposition_dfs_order_count,
            receipt.decomposition_dfs_order_capacity,
            receipt.decomposition_dfs_order_bytes) ||
        !recordVector(
            QD.temporal_selectivity,
            receipt.decomposition_temporal_selectivity_count,
            receipt.decomposition_temporal_selectivity_capacity,
            receipt.decomposition_temporal_selectivity_bytes) ||
        !recordVector(
            QD.structural_candidate_counts,
            receipt.decomposition_structural_candidate_counts_count,
            receipt.decomposition_structural_candidate_counts_capacity,
            receipt.decomposition_structural_candidate_counts_bytes) ||
        !recordVector(
            QD.compatible_edge_counts,
            receipt.decomposition_compatible_edge_counts_count,
            receipt.decomposition_compatible_edge_counts_capacity,
            receipt.decomposition_compatible_edge_counts_bytes) ||
        !recordVector(
            QD.durable_compatible_edge_counts,
            receipt.decomposition_durable_compatible_edge_counts_count,
            receipt.decomposition_durable_compatible_edge_counts_capacity,
            receipt.decomposition_durable_compatible_edge_counts_bytes) ||
        !recordVector(
            QD.durability_survival_rates,
            receipt.decomposition_durability_survival_rates_count,
            receipt.decomposition_durability_survival_rates_capacity,
            receipt.decomposition_durability_survival_rates_bytes)) {
        return overflow();
    }

    if (!checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_spanning_outer_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_spanning_nested_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_non_tree_edges_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_vertex_labels_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_parent_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_dfs_order_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_temporal_selectivity_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_structural_candidate_counts_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_compatible_edge_counts_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_durable_compatible_edge_counts_bytes) ||
        !checkedCapacityAccumulate(
            receipt.query_decomposition_bytes,
            receipt.decomposition_durability_survival_rates_bytes)) {
        return overflow();
    }

    if (!checkedCapacityAccumulate(receipt.dynamic_bytes, receipt.nodes_bytes) ||
        !checkedCapacityAccumulate(
            receipt.dynamic_bytes, receipt.root_candidates_bytes) ||
        !checkedCapacityAccumulate(receipt.dynamic_bytes, receipt.blocks_bytes) ||
        !checkedCapacityAccumulate(
            receipt.dynamic_bytes, receipt.block_candidates_bytes) ||
        !checkedCapacityAccumulate(
            receipt.dynamic_bytes, receipt.query_out_requirements_bytes) ||
        !checkedCapacityAccumulate(
            receipt.dynamic_bytes, receipt.query_in_requirements_bytes) ||
        !checkedCapacityAccumulate(
            receipt.dynamic_bytes, receipt.query_decomposition_bytes) ||
        !checkedCapacityAdd(
            receipt.object_bytes,
            receipt.dynamic_bytes,
            receipt.total_bytes)) {
        return overflow();
    }

    receipt.complete = true;
    if (receipt.complete && !receipt.invariantHolds()) {
        receipt.complete = false;
    }
    return receipt;
}

std::size_t TDTree::getMemoryUsage() const {
    const TDTreeCapacityReceipt receipt = capacityReceipt();
    if (!receipt.complete) {
        if (receipt.overflowed) {
            throw std::overflow_error(
                "TD-tree native capacity receipt overflow");
        }
        throw std::logic_error(
            "TD-tree native capacity receipt is incomplete");
    }
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t)) {
        if (receipt.total_bytes >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())) {
            throw std::overflow_error(
                "TD-tree native capacity does not fit size_t");
        }
    }
    return static_cast<std::size_t>(receipt.total_bytes);
}

std::uint64_t TDTree::executionPlanFingerprint() const {
    const std::uint64_t query_fingerprint = snapshot_owner_
        ? snapshot_owner_->queryFingerprint()
        : queryGraphContentFingerprint(Q);
    return fixedPlanFingerprint(
        query_fingerprint,
        QD.root,
        QD.dfs_order,
        QD.parent);
}

std::uint64_t TDTree::candidateContentFingerprint() const noexcept {
    std::uint64_t hash = UINT64_C(14695981039346656037);
    appendFingerprintWord(hash, UINT64_C(0x44534d2d54445233));
    appendFingerprintWord(hash, executionPlanFingerprint());
    appendFingerprintWord(
        hash,
        static_cast<std::uint64_t>(
            static_cast<std::uint32_t>(k_threshold)));
    appendFingerprintWord(hash, static_cast<std::uint64_t>(nodes.size()));
    for (std::size_t query_vertex = 0;
         query_vertex < nodes.size();
         ++query_vertex) {
        const TDTreeNode& node = nodes[query_vertex];
        appendFingerprintWord(hash, static_cast<std::uint64_t>(query_vertex));
        appendFingerprintWord(
            hash,
            static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(node.query_vertex_id)));
        const int query_parent = query_vertex < QD.parent.size()
            ? QD.parent[query_vertex]
            : -1;
        appendFingerprintWord(
            hash,
            static_cast<std::uint64_t>(
                static_cast<std::uint32_t>(query_parent)));
        appendFingerprintWord(
            hash,
            static_cast<std::uint64_t>(node.root_candidates.size()));
        for (int candidate : node.root_candidates) {
            appendFingerprintWord(
                hash,
                static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(
                        G.externalId(candidate))));
        }
        appendFingerprintWord(
            hash,
            static_cast<std::uint64_t>(node.blocks.size()));
        for (const TDTreeBlock& block : node.blocks) {
            appendFingerprintWord(
                hash,
                static_cast<std::uint64_t>(
                    static_cast<std::uint32_t>(
                        G.externalId(block.v_par))));
            appendFingerprintWord(
                hash,
                static_cast<std::uint64_t>(block.V_cand.size()));
            for (int candidate : block.V_cand) {
                appendFingerprintWord(
                    hash,
                    static_cast<std::uint64_t>(
                        static_cast<std::uint32_t>(
                            G.externalId(candidate))));
            }
        }
    }
    return hash;
}

int TDTree::minimumDuration() const noexcept {
    return k_threshold;
}

bool TDTree::hasOnlySortedFlatBlocks() const {
    for (const TDTreeNode& node : nodes) {
        if (!std::is_sorted(
                node.root_candidates.begin(),
                node.root_candidates.end()) ||
            std::adjacent_find(
                node.root_candidates.begin(),
                node.root_candidates.end()) !=
                node.root_candidates.end()) {
            return false;
        }
        int previous_parent = 0;
        bool have_parent = false;
        for (const TDTreeBlock& block : node.blocks) {
            if ((have_parent && block.v_par <= previous_parent) ||
                !std::is_sorted(
                    block.V_cand.begin(), block.V_cand.end()) ||
                std::adjacent_find(
                    block.V_cand.begin(), block.V_cand.end()) !=
                    block.V_cand.end()) {
                return false;
            }
            previous_parent = block.v_par;
            have_parent = true;
        }
    }
    return true;
}

std::uint64_t TDTree::flatPayloadCapacityBytes() const {
    const TDTreeCapacityReceipt receipt = capacityReceipt();
    if (!receipt.complete) {
        if (receipt.overflowed) {
            throw std::overflow_error(
                "candidate TD-tree native capacity overflow");
        }
        throw std::logic_error(
            "flat candidate payload capacity model is incomplete");
    }
    return receipt.total_bytes;
}

void TDTree::setDecompositionMetadata(
    const QueryDecomposition& decomposition) {
    if (sealed_flat_candidate_) {
        throw std::logic_error(
            "a committed flat candidate TD-tree is payload-sealed; "
            "store reporting metadata outside the tree");
    }
    if (decomposition.root != QD.root ||
        decomposition.dfs_order != QD.dfs_order ||
        decomposition.parent != QD.parent) {
        throw std::invalid_argument(
            "TD-tree metadata update cannot change its executable plan");
    }
    if ((snapshot_owner_
             ? !queryDecompositionTopologyIsCanonical(
                   *snapshot_owner_, decomposition)
             : !queryDecompositionTopologyIsCanonical(
                   Q, decomposition)) ||
        decomposition.spanning_tree_adj != QD.spanning_tree_adj ||
        decomposition.non_tree_edges != QD.non_tree_edges ||
        decomposition.connected != QD.connected ||
        decomposition.vertex_labels != QD.vertex_labels) {
        throw std::invalid_argument(
            "TD-tree metadata update cannot change executable topology");
    }
    QD = decomposition;
    corrected_paper_incumbent_origin_ =
        snapshot_owner_ != nullptr && !sealed_flat_candidate_ &&
        matchesCorrectedPaperIncumbent(G, Q, QD);
    corrected_paper_incumbent_plan_fingerprint_ =
        corrected_paper_incumbent_origin_
        ? fixedPlanFingerprint(
              snapshot_owner_->queryFingerprint(),
              QD.root,
              QD.dfs_order,
              QD.parent)
        : 0;
}
