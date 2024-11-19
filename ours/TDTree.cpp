#include "TDTree.h"
#include <iostream>

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
    std::cout << "TD-Tree grown with " << nodes.size() << " nodes." << std::endl;
}

// Non-tree edge test using Bloom filters
bool TDTree::nonTreeEdgeTest(int v_prime, TDTreeNode* current_node) const {
    // Check if the node is internal and has a Bloom filter
    if (!current_node || !current_node->bloom) {
        // If there is no bloom filter or current node is null, we cannot perform the test
        return false;
    }

    // Use the Bloom filter to check if v_prime is a possible match
    bool bloom_result = current_node->bloom->possiblyContains(v_prime);

    if (bloom_result) {
        // If Bloom filter says it might contain the element, we consider it as a potential candidate
        return true;
    } else {
        // If Bloom filter says no, we can definitively say it does not match
        return false;
    }
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
    
    // Calculate duration as max time - min time + 1
    int min_time = *std::min_element(vertex_time_instances.begin(), vertex_time_instances.end());
    int max_time = *std::max_element(vertex_time_instances.begin(), vertex_time_instances.end());
    int duration = max_time - min_time + 1;
    
    return duration >= k_threshold;
}
