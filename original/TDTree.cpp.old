#include "TDTree.h"
#include <iostream>

// Constructor: Initializes the TDTree and starts building
TDTree::TDTree(const Graph& temporal_graph, const QueryDecomposition& decomposition, int k)
    : G(temporal_graph), QD(decomposition), k_threshold(k), label_counts_({}) {
    build();
}

// Fill the root node with candidate matching vertices
void TDTree::fillRoot() {
    TDTreeNode* root = nodes[0].get();

    // Iterate over all vertices in the temporal graph to find candidates for the root
    for (int v = 0; v < G.num_vertices; ++v) {
        // Ensure both sides are strings for comparison
        if (!G.vertex_labels.empty() && !QD.spanning_tree_adj[root->query_vertex_id].empty()) {
            int query_index = QD.spanning_tree_adj[root->query_vertex_id][0];
            if (G.vertex_labels[v] == QD.vertex_labels[query_index]) { // Assuming labels are stored similarly
                // Additional conditions can be added here (degree, distinct neighbor labels, duration)

                // For simplicity, assume all labels match and add as candidates
                TDTreeBlock block(-1); // -1 indicates no parent
                block.V_cand.push_back(v);
                root->blocks.emplace_back(block);
                root->bloom->add(v);
            }
        }
    }
}

// Grow the TD-Tree by recursively filling nodes
void TDTree::growTDTree() {
    initTree(label_counts_);
    fillRoot();

    // Iterate over nodes in DFS order to fill child nodes
    size_t current_idx = 0;
    while (current_idx < nodes.size()) {
        TDTreeNode* current_node = nodes[current_idx].get();
        for (auto& block : current_node->blocks) {
            for (auto& v : block.V_cand) {
                // Iterate over all children in the spanning tree
                for (auto& child_query_id : QD.spanning_tree_adj[current_node->query_vertex_id]) {
                    // Check if child node already exists
                    size_t child_idx = 0;
                    bool found = false;
                    for (; child_idx < nodes.size(); ++child_idx) {
                        if (nodes[child_idx]->query_vertex_id == child_query_id) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) {
                        // Create new child node
                        auto child_node = std::make_unique<TDTreeNode>(child_query_id);
                        child_node->isLeaf = QD.spanning_tree_adj[child_query_id].empty();
                        nodes.push_back(std::move(child_node));
                        child_idx = nodes.size() - 1;
                    }
                    TDTreeNode* child_node = nodes[child_idx].get();

                    // Find all neighbors of v in the temporal graph
                    for (const auto& edge : G.adj[v]) {
                        int v_prime = edge.to;

                        // Apply non-tree edge test and other conditions
                        if (!nonTreeEdgeTest(v_prime, child_node)) continue;

                        auto it_edge_time_instances = G.edge_time_instances.find({v, v_prime});
                        if (it_edge_time_instances != G.edge_time_instances.end()) {
                            std::unordered_set<int> TS_intersection; 
                            TS_intersection = intersectTimeSets(it_edge_time_instances->second, block.TS);

                            if (TS_intersection.size() < k_threshold) continue;

                            // Add v_prime to the child node's V_cand
                            TDTreeBlock child_block(v);
                            child_block.V_cand.push_back(v_prime);
                            if (child_node->isLeaf) {
                                child_block.TS = TS_intersection;
                            }
                            child_node->blocks.emplace_back(child_block);
                            child_node->bloom->add(v_prime);
                        }
                    }
                }
            }
        }
        current_idx++;
    }
}

// Non-tree edge test using Bloom filters
bool TDTree::nonTreeEdgeTest(int v_prime, TDTreeNode* current_node) const {
    // Placeholder implementation: always return true
    return true;
}

// Trim the TD-Tree by removing false candidate matchings
void TDTree::trimTDTree() {
    trimTopDown();
    trimBottomUp();
}

// Top-Down Pass for trimming (Preorder)
void TDTree::trimTopDown() {
    for (auto& node_ptr : nodes) {
        TDTreeNode* node = node_ptr.get();
        if (node->isLeaf) continue;

        for (int i = 0; i < node->blocks.size(); ) {
            TDTreeBlock& block = node->blocks[i];
            bool should_remove = false;

            if (!node->bloom->possiblyContains(block.v_par)) should_remove = true;

            if (!should_remove) ++i;
            else node->blocks.erase(node->blocks.begin() + i);
        }
    }
}

// Bottom-Up Pass for trimming (Postorder)
void TDTree::trimBottomUp() {
    for (int i = nodes.size() - 1; i >= 0; --i) {
        TDTreeNode* node = nodes[i].get();
        if (node->isLeaf) continue;

        for (int j = 0; j < node->blocks.size(); ) {
            TDTreeBlock& block = node->blocks[j];
            bool should_remove = false;

            if (!should_remove) ++j;
            else node->blocks.erase(node->blocks.begin() + j);
        }
    }
}

// Remove a block from a node
void TDTree::removeBlock(TDTreeNode* node, int block_index) {
    if (block_index >= 0 && block_index < node->blocks.size()) {
        node->blocks.erase(node->blocks.begin() + block_index);
    }
}

// Print the TD-Tree structure
void TDTree::print() const {
    for (const auto& node_ptr : nodes) {
        const TDTreeNode* node = node_ptr.get();
        std::cout << "Query Vertex ID: " << node->query_vertex_id << std::endl;
        for (const auto& block : node->blocks) {
            std::cout << "  Parent Vertex: " << block.v_par << " | Candidates: ";
            for (const auto& v : block.V_cand) {
                std::cout << v << " ";
            }
            if (node->isLeaf) {
                std::cout << "| TS: { ";
                for (const auto& ts : block.TS) {
                    std::cout << ts << " ";
                }
                std::cout << "}";
            }
            std::cout << std::endl;
        }
    }
}
