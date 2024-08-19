#ifndef UTILS_H
#define UTILS_H

#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include "bloom_filter.h"

using namespace std;

// Structure definitions
struct TreeNode {
    int id;
    int label;
    int degree;
    unordered_set<int> neighbor_labels;
    int duration;
    TreeNode* parent;
    vector<TreeNode*> children;
    set<int> V_cand;  // Candidate vertices
    BloomFilter* bloom_filter; // Add BloomFilter here
    unordered_set<int> backward_edges;  // A set of valid backward edges for this node
    set<int> TS;  // Time instances associated with this node
    vector<TreeNode*> children;

};

struct TimeEdge {
    int v1;
    int v2;
    set<int> time_instances;
};

class Graph {
public:
    unordered_map<int, TreeNode> nodes;
    unordered_map<int, vector<TimeEdge>> edges; // Map from node to a list of edges

    void add_edge(int v1, int v2, int timeinstance);
};

struct Tree {
    TreeNode* root;
};

// Global variables
extern Graph g;
extern Graph q;
extern int k;
extern Graph G;
extern Graph Q;
#endif // UTILS_H
