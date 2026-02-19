#ifndef TDTREE_H
#define TDTREE_H

#include <vector>
#include <unordered_set>
#include <memory>
#include <string>
#include "query_decomposition.h"
#include "Utils.h"

// Structure: TDTreeBlock
struct TDTreeBlock {
    int v_par; // Parent vertex in the data graph
    std::vector<int> V_cand; // Candidate vertices for the current query vertex
    std::unordered_set<int> TS; // Time instance set (only for leaf nodes)

    TDTreeBlock(int parent_vertex) : v_par(parent_vertex) {}
};

// Structure: TDTreeNode
struct TDTreeNode {
    int query_vertex_id; // ID of the query vertex
    std::vector<TDTreeBlock> blocks; // List of blocks
    std::unique_ptr<BloomFilter> bloom; // Bloom filter for internal nodes
    bool isLeaf; // Indicates if the node is a leaf

    TDTreeNode(int q_vid, size_t bloom_size = 1000, size_t num_hashes = 3)
        : query_vertex_id(q_vid), isLeaf(false) {
        bloom = std::make_unique<BloomFilter>(bloom_size, num_hashes);
    }
};

// Class: TDTree
class TDTree {
public:
    // Constructor
    TDTree(const Graph& temporal_graph, const Graph& query_graph, const QueryDecomposition& decomposition, int k);

    // Build the TD-Tree
    void build();

    // Print the TD-Tree (for debugging)
    void print() const;
    void print_res() const;
    void save_res(const std::string& filename) const;
    size_t getMemoryUsage() const;

private:
    const Graph& G; // Temporal graph
    const Graph& Q; // Query Graph
    const QueryDecomposition& QD; // Query decomposition result
    int k_threshold; // Minimum duration threshold

    std::vector<std::unique_ptr<TDTreeNode>> nodes; // List of TD-Tree nodes

    // Internal methods for tree construction
    void growTDTree();
    void trimTDTree();
    void initTree(const std::vector<std::vector<int>>& query_tree);    
    void fillRoot();
    void fillNode(TDTreeNode* current_node, int parent_vertex, const std::unordered_set<int>& TS_set);
    void removeBlock(TDTreeNode* node, int block_index);

    // Helper methods for trimming
    void trimTopDown();
    void trimBottomUp();

    // Non-tree edge verification
    bool nonTreeEdgeTest(int v_prime, TDTreeNode* current_node) const;
    bool checkMinimumDuration(int vertex) const;
    bool checkMinimumConsecutiveDuration(const std::unordered_set<int>& time_instances) const;
    std::unordered_set<int> computeVertexTimeInstances(int vertex) const;

    // Reference to label counts for selectivity
    // const std::unordered_map<std::string, int>& label_counts_;

};

#endif // TDTREE_H
