#ifndef TDTREE_H
#define TDTREE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

#include "Utils.h"
#include "query_decomposition.h"

struct TDTreeCell {
    int data_vertex = -1;
    // Algorithm 1 stores the root-to-leaf tree-path time-instance set at a
    // leaf cell. Internal cells leave this empty.
    std::vector<TimeInterval> leaf_time_instances;
    // One block index per query-tree child, in spanning_tree_adj order.
    // This is the paper's auxiliary pointer from a parent cell to a child
    // block. Indexes are local to the corresponding child TD-tree node.
    std::vector<std::size_t> child_block_indices;
};

struct TDTreeBlock {
    int v_par = -1;
    std::vector<TDTreeCell> candidates;
    // Section 7.2 compaction for leaf blocks: this is the union of the
    // candidates' original leaf sets, while each cell stores union \ original.
    std::vector<TimeInterval> leaf_time_union;
    bool leaf_time_compacted = false;
};

struct TDTreeNode {
    int query_vertex_id = -1;
    bool isRoot = false;
    bool isLeaf = false;
    std::vector<TDTreeCell> root_candidates;
    std::vector<TDTreeBlock> blocks;
};

struct MatchSummary {
    std::uint64_t match_count = 0;
    long long enumeration_milliseconds = 0;
    bool output_written = false;
    bool count_overflow = false;
    std::size_t threads_requested = 1;
    std::size_t threads_used = 1;
    std::size_t workers_with_tasks = 0;
    std::size_t root_tasks_claimed = 0;
    std::size_t worker_root_tasks_min = 0;
    std::size_t worker_root_tasks_max = 0;
    std::size_t root_candidate_count = 0;
    std::uint64_t dfs_states = 0;
    std::uint64_t generated_candidates = 0;
    std::uint64_t accepted_candidates = 0;
    std::uint64_t completed_matches = 0;
    std::uint64_t pruned_injectivity = 0;
    std::uint64_t pruned_temporal_support = 0;
};

enum class MatchOutputMode {
    Full,
    CountOnly
};

const char* matchOutputModeName(MatchOutputMode mode);

struct CheckedMatchCounter {
    std::uint64_t value = 0;
    bool overflowed = false;

    bool increment();
};

class TDTree {
public:
    TDTree(
        const Graph& temporal_graph,
        const Graph& query_graph,
        const QueryDecomposition& decomposition,
        int minimum_duration,
        DurationMode duration_mode = DurationMode::Cardinality);

    void print_res() const;
    MatchSummary save_res(
        const std::string& filename,
        MatchOutputMode mode = MatchOutputMode::Full,
        std::size_t threads = 1) const;
    std::size_t getMemoryUsage() const;
    std::size_t candidateRelationCount() const;

private:
    const Graph& G;
    const Graph& Q;
    const QueryDecomposition& QD;
    int k_threshold;
    DurationMode duration_mode;

    std::vector<TDTreeNode> nodes;
    std::vector<std::array<int, kLabelCount>> query_out_neighbor_label_requirements;
    std::vector<std::array<int, kLabelCount>> query_in_neighbor_label_requirements;

    void build();
    void initializeNodes();
    void initializeQueryRequirements();
    void growAndTrim();

    bool isDataVertexCandidate(int data_vertex, int query_vertex) const;
    bool buildChildren(
        int query_vertex,
        TDTreeCell& parent_cell,
        std::vector<int>& path_mapping,
        const std::vector<TimeInterval>& path_time_instances,
        bool has_path_time_instances);
    bool passesMappedNonTreeConstraints(
        int data_vertex,
        int query_vertex,
        const std::vector<int>& path_mapping) const;
    bool treeEdgeTimeInstances(
        int parent_query_vertex,
        int child_query_vertex,
        int parent_data_vertex,
        int child_data_vertex,
        std::vector<TimeInterval>& time_instances) const;
    std::vector<std::size_t> captureBlockSizes() const;
    void rollbackBlocks(const std::vector<std::size_t>& sizes);
    void compactLeafBlock(TDTreeBlock& block) const;
    std::vector<int> generateMatchingOrder() const;

    std::vector<int> uniqueCandidates(const TDTreeNode& node) const;
    std::size_t uniqueCandidateCount(const TDTreeNode& node) const;
    CheckedMatchCounter enumerateMatches(
        std::ostream& output,
        MatchOutputMode mode,
        std::size_t requested_threads,
        MatchSummary& summary) const;
};

#endif // TDTREE_H
