#ifndef TRIM_TD_TREE_H
#define TRIM_TD_TREE_H

#include <vector>
#include <set>
#include <unordered_set>
#include <algorithm>
#include "utils.h"

using namespace std;

// Function prototypes

// Function to perform a non-tree edge test
bool non_edge_edge_test(int v, TreeNode* u);

// Function to remove a block from a tree node
void remove_block(TreeNode* b, TreeNode* u);

// Function to perform preorder traversal on the tree
void preorder_traversal(TreeNode* node, void (*func)(TreeNode*));

// Function to perform postorder traversal on the tree
void postorder_traversal(TreeNode* node, void (*func)(TreeNode*));

// Main function for the TrimTDTree algorithm
Tree* TrimTDTree(Tree* T);

#endif // TRIM_TD_TREE_H
