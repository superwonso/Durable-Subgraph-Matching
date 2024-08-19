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