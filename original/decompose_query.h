#ifndef DECOMPOSE_QUERY_H
#define DECOMPOSE_QUERY_H

#include <iostream>
#include <vector>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <limits>
#include "utils.h"

using namespace std;

// Function declarations

// Helper function to calculate the selectivity of a vertex
double calculate_selectivity(int vertex, const Graph& G, const Graph& Q);

// Helper function to perform DFS and build the spanning tree T
void dfs_build_tree(int u, const Graph& Q, Tree* T, set<int>& visited, unordered_set<int>& non_tree_edges);

// Main function to decompose the query graph Q into a spanning tree T
Tree* DecomposeQuery(const Graph& Q);

#endif // DECOMPOSE_QUERY_H
