#ifndef UTILS_H
#define UTILS_H

#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include <iostream>
#include <string>
#include <algorithm>
#include "bloom_filter.h"

using namespace std;

// Structure definitions

struct TimeEdge {
    int v1;
    int v2;
    set<int> time_instances;  // Set of time instances associated with the edge
};

struct TreeNode {
    int id; // Node ID
    int label; // Node label
    int degree; // Node degree
    int duration; // Duration
    unordered_set<int> neighbor_labels; // Neighbor labels
    TreeNode* parent; // Pointer to parent node
    vector<TreeNode*> children; // List of child nodes
    set<int> V_cand;  // Candidate vertices
    BloomFilter* bloom_filter; // Pointer to a Bloom filter for this node
    unordered_set<int> backward_edges;  // A set of valid backward edges for this node
    set<int> TS;  // Time instances associated with this node
};

class Graph {
public:
    unordered_map<int, TreeNode> nodes; // Nodes in the graph
    unordered_map<int, vector<TimeEdge>> edges; // Edges in the graph, mapping from node to a list of edges

    void add_edge(int v1, int v2, int timeinstance); // Function to add an edge to the graph
};


struct Tree {
    TreeNode* root; // Root of the tree
};

// Global variables
extern Graph g;
extern Graph q;
extern int k;
extern Graph G;
extern Graph Q;

// Function declarations
void save_graph_to_file(Graph G, const string& filename);
void save_tree_to_file(Tree* T, const string& filename);

#endif // UTILS_H
