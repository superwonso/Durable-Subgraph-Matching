#include "TDTree.h"
#include <iostream>
#include <algorithm>
#include <fstream>

// Constructor: Initializes the TDTree and starts building
TDTree::TDTree(const Graph& temporal_graph, const Graph& query_graph, const QueryDecomposition& decomposition, int k)
    : G(temporal_graph), Q(query_graph), QD(decomposition), k_threshold(k) {
    build();
}

// Build the TD-Tree by growing and then trimming
void TDTree::build() {
    initTree(QD.spanning_tree_adj);
    growTDTree();
    trimTDTree();
    std::cout << "TD-Tree built successfully!" << std::endl;
}

// Initialize the TD-Tree using the query tree structure
void TDTree::initTree(const std::vector<std::vector<int>>& query_tree) {
    // Initialize the TD-tree nodes based on the query tree structure
    for (size_t i = 0; i < query_tree.size(); ++i) {
        auto node = std::make_unique<TDTreeNode>(i);
        node->isLeaf = query_tree[i].empty();
        nodes.push_back(std::move(node));
    }
    std::cout << "TD-Tree initialized with " << nodes.size() << " nodes." << std::endl;
}

// Fill the root node with candidate matching vertices
void TDTree::fillRoot() {
    TDTreeNode* root = nodes[0].get();

    // Iterate over all vertices in the temporal graph to find candidates for the root
    for (int v = 0; v < G.num_vertices; ++v) {
        // Check if vertex labels match and other required conditions
        if (!G.vertex_labels.empty() && !QD.vertex_labels.empty()) {
            int query_index = root->query_vertex_id;

            // Label matching condition
            if (G.vertex_labels[v] == QD.vertex_labels[query_index]) {

                // Degree condition: check if the degree of the vertex in G is sufficient
                if (G.adj[v].size() < QD.spanning_tree_adj[query_index].size()) {
                    continue;
                }

                // Neighbor label count condition: ensure sufficient distinct labels
                std::unordered_set<std::string> neighbor_labels;
                for (const auto& edge : G.adj[v]) {
                    neighbor_labels.insert(G.vertex_labels[edge.to]);                

                }
                if (neighbor_labels.size() < QD.spanning_tree_adj[query_index].size()) {
                    continue;
                }

                // Minimum duration condition: check if vertex has duration >= k
                if (!checkMinimumDuration(v)) {
                    continue;  // Skip this vertex if duration requirement is not met
                }

                // Add vertex as a candidate for the root node
                TDTreeBlock block(-1); // -1 indicates no parent
                block.V_cand.push_back(v);
                block.TS = computeVertexTimeInstances(v);
                root->blocks.emplace_back(block);
                root->bloom->add(v);
            }
        } else {
            std::cout << "Error: Vertex labels not found in the temporal graph or query graph." << std::endl;
        }
    }
    std::cout << "Root node filled with " << root->blocks.size() << " candidate blocks." << std::endl;
}

// Grow the TD-Tree by recursively filling nodes
void TDTree::growTDTree() {
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
                        if (it_edge_time_instances == G.edge_time_instances.end()) {
                        it_edge_time_instances = G.edge_time_instances.find({v_prime, v});
                        }
                        
                        if (it_edge_time_instances != G.edge_time_instances.end()) {
                            std::unordered_set<int> TS_intersection; 
                            TS_intersection = intersectTimeSets(it_edge_time_instances->second, block.TS);

                            if (!checkMinimumConsecutiveDuration(TS_intersection)) continue;

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
    std::cout << "TD-Tree grown with " << nodes.size() << " nodes." << std::endl;
}

// Non-tree edge test using Bloom filters
bool TDTree::nonTreeEdgeTest(int v_prime, TDTreeNode* current_node) const {
    if (!current_node || !current_node->bloom) return false;
    return !current_node->bloom->possiblyContains(v_prime);
}


// Trim the TD-Tree by removing false candidate matchings
void TDTree::trimTDTree() {
    trimTopDown();
    trimBottomUp();
    std::cout << "TD-Tree trimmed successfully!" << std::endl;
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

            // Remove any candidate vertex that cannot be extended to satisfy all
            // child query vertices.  The check is performed per candidate so that
            // blocks may store multiple candidates, mirroring typical graph
            // matching scenarios.
            block.V_cand.erase(std::remove_if(block.V_cand.begin(), block.V_cand.end(),
                [&](int v_cand) {
                    for (int child_qid : QD.spanning_tree_adj[node->query_vertex_id]) {
                        TDTreeNode* child_node = nullptr;
                        // Locate the TDTreeNode corresponding to the child query vertex
                        for (auto& n_ptr : nodes) {
                            if (n_ptr->query_vertex_id == child_qid) {
                                child_node = n_ptr.get();
                                break;
                            }
                        }

                        bool found_match = false;
                        if (child_node) {
                            // Fast rejection using the child's bloom filter when
                            // available before scanning the blocks.
                            if (child_node->bloom && !child_node->bloom->possiblyContains(v_cand)) {
                                return true; // candidate not present in child
                            }
                            for (const auto& child_block : child_node->blocks) {
                                if (child_block.v_par == v_cand) {
                                    found_match = true;
                                    break;
                                }
                            }
                        }

                        if (!found_match) {
                            return true; // remove this candidate
                        }
                    }
                    return false; // candidate is consistent with all children
                }),
                block.V_cand.end());

            if (block.V_cand.empty()) {
                node->blocks.erase(node->blocks.begin() + j);
            } else {
                ++j;
            }
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
    // for (const auto& node_ptr : nodes) {
    //     const TDTreeNode* node = node_ptr.get();
    //     std::cout << "Query Vertex ID: " << node->query_vertex_id << std::endl;
    //     for (const auto& block : node->blocks) {
    //         std::cout << "  Parent Vertex: " << block.v_par << " | Candidates: ";
    //         for (const auto& v : block.V_cand) {
    //             std::cout << v << " ";
    //         }
    //         if (node->isLeaf) {
    //             std::cout << "| TS: { ";
    //             for (const auto& ts : block.TS) {
    //                 std::cout << ts << " ";
    //             }
    //             std::cout << "}";
    //         }
    //         std::cout << std::endl;
    //     }
    // }
}

void TDTree::print_res() const {
    for (const auto& node_ptr : nodes) {
        const TDTreeNode* node = node_ptr.get();
        // Print only the final matching results for each query vertex
        if (node->isLeaf) {
            std::cout << "Query Vertex ID: " << node->query_vertex_id << " - Candidates: ";
            for (const auto& block : node->blocks) {
                for (const auto& v : block.V_cand) {
                    std::cout << v << " ";
                }
            }
            std::cout << std::endl;
        }
    }
}

void TDTree::save_res(const std::string& filename) const {
    std::ofstream out(filename);
    if (!out.is_open()) {
        return;
    }
    for (const auto& node_ptr : nodes) {
        const TDTreeNode* node = node_ptr.get();
        if (node->isLeaf) {
            out << "Query Vertex ID: " << node->query_vertex_id << " - Candidates: ";
            for (const auto& block : node->blocks) {
                for (const auto& v : block.V_cand) {
                    out << v << ' ';
                }
            }
            out << '\n';
        }
    }
}

bool TDTree::checkMinimumDuration(int vertex) const {
    std::unordered_set<int> vertex_time_instances;
    
    // Collect all time instances for the vertex from its edges
    for (const auto& edge : G.adj[vertex]) {
        // Check forward edge
        std::pair<int, int> edge_pair(vertex, edge.to);
        auto time_it = G.edge_time_instances.find(edge_pair);
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }
        
        // Check reverse edge
        edge_pair = std::make_pair(edge.to, vertex);
        time_it = G.edge_time_instances.find(edge_pair);
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }
    }
    
    // If no time instances found, vertex doesn't meet duration requirement
    if (vertex_time_instances.empty()) {
        return false;
    }
    
    // Calculate duration as the number of unique time instances
    int duration = vertex_time_instances.size();
    
    return duration >= k_threshold;
}

bool TDTree::checkMinimumConsecutiveDuration(const std::unordered_set<int>& time_instances) const {
    if (time_instances.size() < k_threshold) {
        return false;
    }

    // Convert unordered_set to a sorted vector
    std::vector<int> times(time_instances.begin(), time_instances.end());
    std::sort(times.begin(), times.end());

    // Check if there are at least k consecutive time instances
    int consecutive_count = 1;
    for (size_t i = 1; i < times.size(); ++i) {
        if (times[i] == times[i - 1] + 1) {
            consecutive_count++;
            if (consecutive_count >= k_threshold) {
                return true;
            }
        } else {
            consecutive_count = 1;
        }
    }

    return false;
}


std::unordered_set<int> TDTree::computeVertexTimeInstances(int vertex) const {
    std::unordered_set<int> vertex_time_instances;

    for (const auto& edge : G.adj[vertex]) {
        std::pair<int, int> edge_pair(vertex, edge.to);
        auto time_it = G.edge_time_instances.find(edge_pair);
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }

        edge_pair = std::make_pair(edge.to, vertex);
        time_it = G.edge_time_instances.find(edge_pair);
        if (time_it != G.edge_time_instances.end()) {
            vertex_time_instances.insert(time_it->second.begin(), time_it->second.end());
        }
    }

    return vertex_time_instances;
}
