#ifndef GROW_TD_TREE_H
#define GROW_TD_TREE_H

#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include "bloom_filter.h"
#include "utils.h"

using namespace std;

// Global variables
extern int k;

struct TimeEdge {
    int v2;  // The second vertex in the edge
    set<int> time_instances;  // Set of time instances associated with the edge
};

struct TreeNode {
    int label;
    int degree;
    unordered_set<int> neighbor_labels;
    int duration;
    TreeNode* parent;
    vector<TreeNode*> children;
    set<int> V_cand;  // Candidate vertices
    BloomFilter* bloom_filter; // Pointer to a Bloom filter for this node
    set<int> TS; // Time instances associated with this node
};

struct Graph {
    unordered_map<int, TreeNode> nodes;
    unordered_map<int, vector<TimeEdge>> edges; // Maps each vertex to its outgoing edges
};

struct Tree {
    TreeNode* root;
};

// Function declarations
bool tree_edge_test(int v1, int v2, TreeNode* node, TreeNode* parent);
bool non_tree_edge_test(int v, TreeNode* node);
set<int> TS(int v1, int v2);
void fill_node(TreeNode* c, int v, set<int> TS_set, Graph& G, int k);
void fill_root(TreeNode* root);
Tree* init_tree(Tree* T);
Graph read_graph_from_file(const string& filename);
Tree* GrowTDTree(Graph G, Graph Q, Tree* T, int k);
set<int> custom_set_intersection(const set<int>& set1, const set<int>& set2);

#endif // GROW_TD_TREE_H
