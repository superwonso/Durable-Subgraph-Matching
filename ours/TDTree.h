#ifndef TDTREE_H
#define TDTREE_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <string>
#include <unordered_map>
#include <vector>

#include "Utils.h"
#include "query_decomposition.h"

struct TDTreeBlock {
    int v_par = -1;
    std::vector<int> V_cand;
};

struct TDTreeNode {
    int query_vertex_id = -1;
    bool isRoot = false;
    bool isLeaf = false;
    std::vector<int> root_candidates;
    std::vector<TDTreeBlock> blocks;
    std::unordered_map<int, std::size_t> block_index;

    const TDTreeBlock* findBlock(int parent_vertex) const;
    void rebuildBlockIndex();
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
        int minimum_duration);

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

    std::vector<TDTreeNode> nodes;
    std::vector<std::array<int, kLabelCount>> query_out_neighbor_label_requirements;
    std::vector<std::array<int, kLabelCount>> query_in_neighbor_label_requirements;

    void build();
    void initializeNodes();
    void initializeQueryRequirements();
    void fillRoot();
    void buildCandidateRelations();
    void trimBottomUp();
    void trimTopDown();
    void rebuildBlockIndexes();

    bool isDataVertexCandidate(int data_vertex, int query_vertex) const;
    bool passesAvailableNonTreeConstraints(
        int data_vertex,
        int query_vertex,
        const std::vector<int>& order_position,
        const std::vector<std::vector<std::uint8_t>>& candidate_flags) const;

    std::vector<int> uniqueCandidates(const TDTreeNode& node) const;
    std::size_t uniqueCandidateCount(const TDTreeNode& node) const;
    CheckedMatchCounter enumerateMatches(
        std::ostream& output,
        MatchOutputMode mode,
        std::size_t requested_threads,
        MatchSummary& summary) const;
};

#endif // TDTREE_H
