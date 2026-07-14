#include "GpuCandidateExpander.h"

#include <algorithm>
#include <unordered_set>

#ifdef BUILD_WITH_CUDA
namespace cuda_backend {
bool isRuntimeAvailable();
std::vector<int> filterRootCandidates(
    const std::vector<int>& vertex_label_ids,
    const std::vector<int>& vertex_degrees,
    const std::vector<int>& vertex_duration_counts,
    const std::vector<int>& vertex_distinct_neighbor_label_counts,
    int query_label_id,
    int required_degree,
    int k_threshold);
std::vector<int> filterChildCandidates(
    const std::vector<int>& adjacency_offsets,
    const std::vector<int>& adjacency_neighbors,
    const std::vector<int>& vertex_label_ids,
    const std::vector<int>& vertex_degrees,
    const std::vector<int>& vertex_duration_counts,
    const std::vector<int>& vertex_distinct_neighbor_label_counts,
    int parent_vertex,
    int query_label_id,
    int required_degree,
    int k_threshold);
} // namespace cuda_backend
#endif

CandidateExpander::CandidateExpander(const Graph& graph, const QueryDecomposition& decomposition, int k_threshold)
    : graph_(graph), decomposition_(decomposition), k_threshold_(k_threshold), gpu_enabled_(false) {
    initializeLabelIds();
    initializeVertexStats();
    initializeAdjacencyIndex();

#ifdef BUILD_WITH_CUDA
    gpu_enabled_ = tryEnableGpu();
#endif
}

bool CandidateExpander::isGpuEnabled() const {
    return gpu_enabled_;
}

std::vector<int> CandidateExpander::filterRootCandidates(int root_qid) const {
#ifdef BUILD_WITH_CUDA
    if (gpu_enabled_) {
        return filterRootCandidatesGpu(root_qid);
    }
#endif
    return filterRootCandidatesCpu(root_qid);
}

std::vector<int> CandidateExpander::filterChildCandidates(int parent_vertex, int child_qid) const {
#ifdef BUILD_WITH_CUDA
    if (gpu_enabled_) {
        return filterChildCandidatesGpu(parent_vertex, child_qid);
    }
#endif
    return filterChildCandidatesCpu(parent_vertex, child_qid);
}

void CandidateExpander::initializeLabelIds() {
    auto intern_label = [&](const std::string& label) -> int {
        auto it = label_to_id_.find(label);
        if (it != label_to_id_.end()) {
            return it->second;
        }
        const int id = static_cast<int>(label_to_id_.size());
        label_to_id_.emplace(label, id);
        return id;
    };

    vertex_label_ids_.resize(graph_.vertex_labels.size(), -1);
    for (size_t i = 0; i < graph_.vertex_labels.size(); ++i) {
        vertex_label_ids_[i] = intern_label(graph_.vertex_labels[i]);
    }

    query_label_ids_.resize(decomposition_.vertex_labels.size(), -1);
    for (size_t i = 0; i < decomposition_.vertex_labels.size(); ++i) {
        query_label_ids_[i] = intern_label(decomposition_.vertex_labels[i]);
    }
}

void CandidateExpander::initializeVertexStats() {
    const size_t num_vertices = graph_.adj.size();
    vertex_degrees_.assign(num_vertices, 0);
    vertex_duration_counts_.assign(num_vertices, 0);
    vertex_distinct_neighbor_label_counts_.assign(num_vertices, 0);

    for (size_t u = 0; u < num_vertices; ++u) {
        vertex_degrees_[u] = static_cast<int>(graph_.adj[u].size());

        std::unordered_set<int> time_instances;
        std::unordered_set<int> neighbor_label_ids;
        for (const auto& edge : graph_.adj[u]) {
            if (edge.to >= 0 && edge.to < static_cast<int>(vertex_label_ids_.size())) {
                neighbor_label_ids.insert(vertex_label_ids_[edge.to]);
            }

            auto time_it = graph_.edge_time_instances.find({static_cast<int>(u), edge.to});
            if (time_it != graph_.edge_time_instances.end()) {
                time_instances.insert(time_it->second.begin(), time_it->second.end());
            }

            time_it = graph_.edge_time_instances.find({edge.to, static_cast<int>(u)});
            if (time_it != graph_.edge_time_instances.end()) {
                time_instances.insert(time_it->second.begin(), time_it->second.end());
            }
        }

        vertex_duration_counts_[u] = static_cast<int>(time_instances.size());
        vertex_distinct_neighbor_label_counts_[u] = static_cast<int>(neighbor_label_ids.size());
    }
}

void CandidateExpander::initializeAdjacencyIndex() {
    adjacency_offsets_.assign(graph_.adj.size() + 1, 0);
    adjacency_neighbors_.clear();

    size_t offset = 0;
    for (size_t u = 0; u < graph_.adj.size(); ++u) {
        adjacency_offsets_[u] = static_cast<int>(offset);
        for (const auto& edge : graph_.adj[u]) {
            adjacency_neighbors_.push_back(edge.to);
            ++offset;
        }
    }
    adjacency_offsets_[graph_.adj.size()] = static_cast<int>(offset);
}

std::vector<int> CandidateExpander::filterRootCandidatesCpu(int root_qid) const {
    std::vector<int> candidates;
    if (root_qid < 0 || root_qid >= static_cast<int>(query_label_ids_.size())) {
        return candidates;
    }

    const int query_label_id = query_label_ids_[root_qid];
    const int required_degree = static_cast<int>(decomposition_.spanning_tree_adj[root_qid].size());

    for (int v = 0; v < static_cast<int>(vertex_label_ids_.size()); ++v) {
        if (vertex_label_ids_[v] != query_label_id) {
            continue;
        }
        if (vertex_degrees_[v] < required_degree) {
            continue;
        }
        if (vertex_distinct_neighbor_label_counts_[v] < required_degree) {
            continue;
        }
        if (vertex_duration_counts_[v] < k_threshold_) {
            continue;
        }
        candidates.push_back(v);
    }

    return candidates;
}

std::vector<int> CandidateExpander::filterChildCandidatesCpu(int parent_vertex, int child_qid) const {
    std::vector<int> candidates;
    if (parent_vertex < 0 || parent_vertex + 1 >= static_cast<int>(adjacency_offsets_.size())) {
        return candidates;
    }
    if (child_qid < 0 || child_qid >= static_cast<int>(query_label_ids_.size())) {
        return candidates;
    }

    const int query_label_id = query_label_ids_[child_qid];
    const int required_degree = static_cast<int>(decomposition_.spanning_tree_adj[child_qid].size());
    const int start = adjacency_offsets_[parent_vertex];
    const int end = adjacency_offsets_[parent_vertex + 1];

    for (int i = start; i < end; ++i) {
        const int cand = adjacency_neighbors_[i];
        if (cand < 0 || cand >= static_cast<int>(vertex_label_ids_.size())) {
            continue;
        }
        if (vertex_label_ids_[cand] != query_label_id) {
            continue;
        }
        if (vertex_degrees_[cand] < required_degree) {
            continue;
        }
        if (vertex_distinct_neighbor_label_counts_[cand] < required_degree) {
            continue;
        }
        if (vertex_duration_counts_[cand] < k_threshold_) {
            continue;
        }
        candidates.push_back(cand);
    }

    return candidates;
}

#ifdef BUILD_WITH_CUDA
bool CandidateExpander::tryEnableGpu() {
    return cuda_backend::isRuntimeAvailable();
}

std::vector<int> CandidateExpander::filterRootCandidatesGpu(int root_qid) const {
    if (root_qid < 0 || root_qid >= static_cast<int>(query_label_ids_.size())) {
        return {};
    }

    return cuda_backend::filterRootCandidates(
        vertex_label_ids_,
        vertex_degrees_,
        vertex_duration_counts_,
        vertex_distinct_neighbor_label_counts_,
        query_label_ids_[root_qid],
        static_cast<int>(decomposition_.spanning_tree_adj[root_qid].size()),
        k_threshold_);
}

std::vector<int> CandidateExpander::filterChildCandidatesGpu(int parent_vertex, int child_qid) const {
    if (child_qid < 0 || child_qid >= static_cast<int>(query_label_ids_.size())) {
        return {};
    }

    return cuda_backend::filterChildCandidates(
        adjacency_offsets_,
        adjacency_neighbors_,
        vertex_label_ids_,
        vertex_degrees_,
        vertex_duration_counts_,
        vertex_distinct_neighbor_label_counts_,
        parent_vertex,
        query_label_ids_[child_qid],
        static_cast<int>(decomposition_.spanning_tree_adj[child_qid].size()),
        k_threshold_);
}
#endif
