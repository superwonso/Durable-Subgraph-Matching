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
