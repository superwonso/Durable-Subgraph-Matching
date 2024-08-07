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
    int label;
    int degree;
    unordered_set<int> neighbor_labels;
    int duration;
    TreeNode* parent;
    vector<TreeNode*> children;
    set<int> V_cand;  // Candidate vertices
    BloomFilter* bloom_filter; // Add BloomFilter here
};

struct Graph {
    unordered_map<int, TreeNode> nodes;
};

struct Tree {
    TreeNode* root;
};

// Global variables
extern Graph g;
extern Graph q;
extern int k;

#endif // UTILS_H
