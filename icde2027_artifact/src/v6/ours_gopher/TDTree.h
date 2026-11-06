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

#if !defined(DSM_E3E4_CORE_ONLY)
    void print_res() const;
    MatchSummary save_res(
        const std::string& filename,
        MatchOutputMode mode = MatchOutputMode::Full) const;
#endif
    std::size_t getMemoryUsage() const;
    std::size_t candidateRelationCount() const;
    const std::vector<int>& rootCandidates() const;
    const std::vector<int>* candidatesForParent(
        int query_vertex,
        int parent_data_vertex) const;
    // V6 read-only metadata access: block order/content remain immutable.
    const std::vector<TDTreeBlock>& candidateBlocks(int query_vertex) const {
        return nodes.at(static_cast<std::size_t>(query_vertex)).blocks;
    }
    std::size_t relationEntryCount(int query_vertex) const;
    std::size_t blockCount(int query_vertex) const;
    std::size_t queryCandidateCount(int query_vertex) const;
    // Opt-in order simulation: a sorted, duplicate-free copy of the existing
    // TD relation's domain. This does not rebuild, reroot, or mutate the tree.
    // A dynamic frontier vertex can be considered before its fixed TD parent.
    std::vector<int> queryCandidateDomain(int query_vertex) const;
    // Refresh reporting-only selectivity metadata after a planner handoff.
    // The executable root/order/parent must remain byte-identical.
    void setDecompositionMetadata(
        const QueryDecomposition& decomposition);
#if !defined(DSM_E3E4_CORE_ONLY)
    void writeSelectivitySummary(std::ostream& output) const;
    void writeCandidateSummary(std::ostream& output) const;
#endif

private:
    const Graph& G;
    const Graph& Q;
    // Own the decomposition so planner-built trees remain valid when handed
    // off after the planner's local decomposition object is destroyed.
    QueryDecomposition QD;
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
#if !defined(DSM_E3E4_CORE_ONLY)
    CheckedMatchCounter enumerateMatches(
        std::ostream& output,
        MatchOutputMode mode) const;
#endif
};

#endif // TDTREE_H
