#include "utils.h"
#include <fstream>
#include <iostream>
#include <string>
#include "utils.h"
// Define global variables
Graph g;
Graph q;
Graph G;
Graph Q;
int k = 3; // Threshold

using namespace std;

void save_graph_to_file(Graph G, const string& filename) {
    ofstream file(filename);
    if (file.is_open()) {
        for (const auto& node : G.nodes) {
            file << node.first << " " << node.second.label << " " << node.second.degree << endl;
        }
        for (const auto& edge : G.edges) {
            for (const TimeEdge& time_edge : edge.second) {
                file << edge.first << " " << time_edge.v2 << " ";
                for (int time_instance : time_edge.time_instances) {
                    file << time_instance << " ";
                }
                file << endl;
            }
        }
        file.close();
    }
}

void save_tree_to_file(Tree* T, const string& filename) {
    ofstream file(filename);
    if (file.is_open()) {
        // Helper function to save a tree node and its children
        function<void(TreeNode*, int)> save_node = [&](TreeNode* node, int depth) {
            if (node == nullptr) return;

            // Save the current node (label, degree, depth for hierarchy visualization)
            file << string(depth, '-') << "Node: " << node->label << ", Degree: " << node->degree << endl;

            // Save candidate vertices for the node
            file << string(depth, '-') << "Candidates: ";
            for (int v : node->V_cand) {
                file << v << " ";
            }
            file << endl;

            // Save the Bloom filter state (if applicable)
            file << string(depth, '-') << "Bloom Filter: ";
            // Assuming you have a way to represent the bloom filter (pseudo-code)
            // file << node->bloom_filter->to_string() << endl;
            file << "[Bloom Filter Data]" << endl; // Placeholder for actual Bloom filter data

            // Recursively save each child node
            for (TreeNode* child : node->children) {
                save_node(child, depth + 1);
            }
        };

        // Start saving from the root of the tree
        save_node(T->root, 0);

        file.close();
    } else {
        cerr << "Failed to open file: " << filename << endl;
    }
}

void Graph::add_edge(int v1, int v2, int timeinstance) {
    // Ensure nodes exist
    if (nodes.find(v1) == nodes.end()) {
        nodes[v1] = TreeNode();
        nodes[v1].id = v1;
    }
    if (nodes.find(v2) == nodes.end()) {
        nodes[v2] = TreeNode();
        nodes[v2].id = v2;
    }

    // Check if the edge already exists using std::find_if
    auto& edge_list = edges[v1];
    auto it = std::find_if(edge_list.begin(), edge_list.end(), [v2](const TimeEdge& edge) {
        return edge.v2 == v2;
    });

    if (it != edge_list.end()) {
        // Edge exists, add the time instance
        it->time_instances.insert(timeinstance);
    } else {
        // Edge doesn't exist, create a new TimeEdge
        TimeEdge new_edge = {v1, v2, {timeinstance}};
        edges[v1].push_back(new_edge);
        edges[v2].push_back(new_edge);  // For undirected graph
    }

    // Add neighbor labels
    nodes[v1].neighbor_labels.insert(v2);
    nodes[v2].neighbor_labels.insert(v1);
}
