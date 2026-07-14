#ifndef GPU_CANDIDATE_EXPANDER_H
#define GPU_CANDIDATE_EXPANDER_H

#include <string>
#include <unordered_map>
#include <vector>

#include "Utils.h"
#include "query_decomposition.h"

class CandidateExpander {
public:
    CandidateExpander(const Graph& graph, const QueryDecomposition& decomposition, int k_threshold);

    bool isGpuEnabled() const;
    std::vector<int> filterRootCandidates(int root_qid) const;
    std::vector<int> filterChildCandidates(int parent_vertex, int child_qid) const;

private:
    const Graph& graph_;
    const QueryDecomposition& decomposition_;
    int k_threshold_;
    bool gpu_enabled_;

    std::unordered_map<std::string, int> label_to_id_;
    std::vector<int> vertex_label_ids_;
    std::vector<int> query_label_ids_;
    std::vector<int> vertex_degrees_;
    std::vector<int> vertex_duration_counts_;
    std::vector<int> vertex_distinct_neighbor_label_counts_;
    std::vector<int> adjacency_offsets_;
    std::vector<int> adjacency_neighbors_;

    void initializeLabelIds();
    void initializeVertexStats();
    void initializeAdjacencyIndex();

    std::vector<int> filterRootCandidatesCpu(int root_qid) const;
    std::vector<int> filterChildCandidatesCpu(int parent_vertex, int child_qid) const;

#ifdef BUILD_WITH_CUDA
    bool tryEnableGpu();
    std::vector<int> filterRootCandidatesGpu(int root_qid) const;
    std::vector<int> filterChildCandidatesGpu(int parent_vertex, int child_qid) const;
#endif
};

#endif // GPU_CANDIDATE_EXPANDER_H
