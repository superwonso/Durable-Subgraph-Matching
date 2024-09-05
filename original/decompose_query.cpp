#include <iostream>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <limits>
#include "utils.h"
#include "decompose_query.h"

using namespace std;

// Helper function to calculate selectivity
// double calculate_selectivity(int vertex, const Graph& G, const Graph& Q) {
//     int label_count = 0;
//     for (const auto& node : G.nodes) {
//         if (node.second.label == Q.nodes.at(vertex).label) {
//             label_count++;
//         }
//     }
//     return static_cast<double>(label_count) / Q.nodes.at(vertex).degree;
// }

// double calculate_selectivity(int vertex, const Graph& G, const Graph& Q) {
//     int label_count = 0;
//     auto it = Q.nodes.find(vertex);
//     if (it != Q.nodes.end()) {
//         int vertex_label = it->second.label;
//         for (const auto& node : G.nodes) {
//             if (node.second.label == vertex_label) {
//                 label_count++;
//             }
//         }
//         return static_cast<double>(label_count) / it->second.degree;
//     } else {
//         cerr << "Vertex " << vertex << " not found in query graph Q." << endl;
//         return numeric_limits<double>::max(); // Returns the maximum value to prevent it from being selected
//     }
// }
double calculate_selectivity(int vertex, const Graph& G, const Graph& Q) {
    int label_count = 0;
    auto it = Q.nodes.find(vertex);
    if (it != Q.nodes.end()) {
        int vertex_label = it->second.label;
        for (const auto& node : G.nodes) {
            if (node.second.label == vertex_label) {
                label_count++;
            }
        }
        // If label_count is zero, it means this label is not present in G
        if (label_count == 0) {
            cerr << "Label " << vertex_label << " of vertex " << vertex << " not found in data graph G." << endl;
        }
        return static_cast<double>(label_count) / it->second.degree;
    } else {
        cerr << "Vertex " << vertex << " not found in query graph Q." << endl;
        return numeric_limits<double>::max(); // Returns the maximum value to prevent it from being selected
    }
}

// Helper function to perform DFS and build the spanning tree T
// void dfs_build_tree(int u, const Graph& Q, Tree* T, set<int>& visited, unordered_set<int>& non_tree_edges) {
//     visited.insert(u);

//     for (const int& v : Q.nodes.at(u).neighbor_labels) {
//         if (visited.find(v) == visited.end()) {
//             // Add the edge (u, v) to the tree
//             TreeNode* child = new TreeNode();
//             child->label = v;
//             child->parent = T->root; // The root will be updated to the correct parent in the DFS
//             T->root->children.push_back(child);

//             // Recursively build the tree
//             dfs_build_tree(v, Q, T, visited, non_tree_edges);
//         } else {
//             // If the edge is a back edge, it's a non-tree edge
//             non_tree_edges.insert(v);
//         }
//     }
// }
void dfs_build_tree(int u, const Graph& Q, Tree* T, set<int>& visited, unordered_set<int>& non_tree_edges) {
    visited.insert(u);

    auto it = Q.nodes.find(u);
    if (it != Q.nodes.end()) {
        for (const int& v : it->second.neighbor_labels) {
            if (visited.find(v) == visited.end()) {
                // Add the edge (u, v) to the tree
                TreeNode* child = new TreeNode();
                child->label = v;
                child->parent = T->root; // The root will be updated to the correct parent in the DFS
                T->root->children.push_back(child);

                // Recursively build the tree
                dfs_build_tree(v, Q, T, visited, non_tree_edges);
            } else {
                // If the edge is a back edge, it's a non-tree edge
                non_tree_edges.insert(v);
            }
        }
    } else {
        cerr << "Node " << u << " not found in query graph Q during DFS." << endl;
    }
}

// DecomposeQuery function
// Tree* DecomposeQuery(const Graph& Q) {
//     Tree* T = new Tree;

//     // Step 1: Select the root of T based on selectivity
//     int root_vertex = -1;
//     double min_selectivity = numeric_limits<double>::max();

//     for (const auto& node : Q.nodes) {
//         double selectivity = calculate_selectivity(node.first, g, Q); // Assume g is the global data graph
//         if (selectivity < min_selectivity) {
//             min_selectivity = selectivity;
//             root_vertex = node.first;
//         }
//     }

//     // Create the root node for T
//     T->root = new TreeNode();
//     T->root->label = root_vertex;

//     // Step 2: Perform DFS to build T and determine non-tree edges
//     set<int> visited;
//     unordered_set<int> non_tree_edges;
//     dfs_build_tree(root_vertex, Q, T, visited, non_tree_edges);

//     // The tree T is now built, and non_tree_edges contains all the non-tree edges

//     // Additional processing if necessary
//     // ...

//     return T;
// }

Tree* DecomposeQuery(const Graph& Q) {
    Tree* T = new Tree;

    // Step 1: Select the root of T based on selectivity
    int root_vertex = -1;
    double min_selectivity = numeric_limits<double>::max();

    for (const auto& node : Q.nodes) {
        double selectivity = calculate_selectivity(node.first, g, Q); // Assume g is the global data graph
        if (selectivity < min_selectivity) {
            min_selectivity = selectivity;
            root_vertex = node.first;
        }
    }

    if (root_vertex == -1) {
        cerr << "Failed to select a root vertex. Tree decomposition aborted." << endl;
        return nullptr;
    }

    // Create the root node for T
    T->root = new TreeNode();
    T->root->label = root_vertex;

    // Step 2: Perform DFS to build T and determine non-tree edges
    set<int> visited;
    unordered_set<int> non_tree_edges;
    dfs_build_tree(root_vertex, Q, T, visited, non_tree_edges);

    // The tree T is now built, and non_tree_edges contains all the non-tree edges

    return T;
}
