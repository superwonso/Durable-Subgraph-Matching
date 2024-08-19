#ifndef MATCH_TD_TREE_H
#define MATCH_TD_TREE_H

#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include "utils.h"

using namespace std;

// Function prototypes

// Function to perform DFS and generate the order of node IDs in the tree
vector<int> gen_order(TreeNode* root);

// Function to calculate the cardinality of a given path in the tree
int calculate_cardinality(TreeNode* node);

// Function to calculate the cardinalities of all paths in the tree
vector<int> calculate_all_cardinalities(TreeNode* root);

// Function to test backward edges during matching
bool backward_edge_test(int v, const vector<int>& M, int i, TreeNode* node, Graph& G, Graph& Q);

// Function to test the duration for k-durability
bool duration_test(set<int>& TS, vector<int>& Pos);

// Function to expand the matching process
void expand(vector<int>& M, int i, vector<int>& Pos, vector<int>& O, Tree* T, Graph& G, Graph& Q, int k);

// Main function for the Match algorithm
void Match(Graph& G, Graph& Q, Tree* T, int k);

#endif // MATCH_TD_TREE_H
