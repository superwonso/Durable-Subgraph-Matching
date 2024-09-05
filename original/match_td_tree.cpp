#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <algorithm>
#include "utils.h"
#include "match_td_tree.h"
using namespace std;

// Function to perform DFS and generate the order of nodes
void dfs(TreeNode* node, vector<int>& order) {
    if (node == nullptr) return;

    // Add the current node's ID to the order
    order.push_back(node->id);

    // Recursively visit all children
    for (TreeNode* child : node->children) {
        dfs(child, order);
    }
}

// Placeholder functions for gen_order, backward_edge_test, and duration_test
// Function to generate the DFS order of nodes in the tree
vector<int> gen_order(TreeNode* root) {
    vector<int> order;

    // Perform DFS starting from the root
    dfs(root, order);

    return order;
}


bool backward_edge_test(int v, const vector<int>& M, int i, TreeNode* node, Graph& G, Graph& Q) {
    clock_t start_bet, finish_bet;
    double duration_bet;
    start_bet = clock();
    // Get the node in G corresponding to v
    TreeNode* g_node = &G.nodes[v];

    // Traverse through all backward edges (u_i+1, u_j) where j < i
    for (int j = 0; j < i; ++j) {
        int u_j = M[j];
        TreeNode* q_node = &Q.nodes[u_j];

        // Check if there is an edge from v to u_j in G
        if (g_node->backward_edges.find(u_j) == g_node->backward_edges.end()) {
            return false;
        }
    }
    finish_bet = clock();
    duration_bet = (double)(finish_bet - start_bet);
    // cout << duration_bet << "ms to run bet function" << endl;
    // If all backward edges are valid
    return true;
}

// Function to calculate the cardinality of a given path in the tree
int calculate_cardinality(TreeNode* node) {
    int cardinality = 0;

    // Traverse the tree path from the node to the leaf node
    while (node != nullptr) {
        // Add the size of V_cand to the cardinality
        cardinality += node->V_cand.size();
        
        // Move to the first child (assuming this is part of a path)
        if (!node->children.empty()) {
            node = node->children[0];
        } else {
            node = nullptr; // Reached a leaf node
        }
    }

    return cardinality;
}

vector<int> calculate_all_cardinalities(TreeNode* root) {
    vector<int> cardinalities;

    // Traverse all paths from root to leaves
    for (TreeNode* child : root->children) {
        int card = calculate_cardinality(child);
        cardinalities.push_back(card);
    }

    // Sort the cardinalities in decreasing order
    sort(cardinalities.rbegin(), cardinalities.rend());

    return cardinalities;
}

bool duration_test(set<int>& TS, vector<int>& Pos) {
    return TS.size() >= k;
}

// Function prototypes
void expand(vector<int>& M, int i, vector<int>& Pos, vector<int>& O, Tree* T, Graph& G, Graph& Q, int k);

// Algorithm 3: Match
void Match(Graph& G, Graph& Q, Tree* T, int k) {
    vector<int> O = gen_order(T->root);
    vector<int> M(Q.nodes.size());  // Assuming this is the number of nodes in Q
    vector<int> Pos(Q.nodes.size());

    for (int v : T->root->V_cand) {
        // printf("Algorithm 3 for ");
        M[0] = v;
        // cout<< "Here 1" << endl;
        // Assuming the block cell storing v is the index of v
        Pos[0] = v;
        // cout<< "Here 2" << endl;
        expand(M, 1, Pos, O, T, G, Q, k);
        // cout<< "Expand Fin" << endl;
    }
}

// expand function
void expand(vector<int>& M, int i, vector<int>& Pos, vector<int>& O, Tree* T, Graph& G, Graph& Q, int k) {
  TreeNode* ui_plus1 = T->root; // Replace with the actual way to get TreeNode from O[i]

    for (int v : ui_plus1->V_cand) {
        // Perform the backward edge test
        if (!backward_edge_test(v, M, i, ui_plus1, G, Q)) {
            printf("Backward edge test : false");
            continue;
        }
        // printf("Algorithm 3 expand for");
        // If ui+1 is a leaf node, perform the duration test
        if (ui_plus1->children.empty()) {
            // Calculate the TS set for ui+1
            set<int> TS_v = ui_plus1->TS;
            set<int> intersection_TS = TS_v;

            for (int j = 0; j < i; ++j) {
                TreeNode* uj = &T->root[j]; // Replace with actual node retrieval
                set<int> Pos_TS = uj->TS;  // Assuming Pos[j] maps directly to uj
                set<int> temp_intersection;
                set_intersection(intersection_TS.begin(), intersection_TS.end(), Pos_TS.begin(), Pos_TS.end(),
                                 inserter(temp_intersection, temp_intersection.begin()));
                intersection_TS = temp_intersection;
            }

            // If the size of TS is less than k, skip this v
            if (intersection_TS.size() < k) {
                continue;
            }
        }

        // If valid, set M[i] and Pos[i] and expand further
        M[i] = v;
        Pos[i] = v;  // Assuming this is the correct way to set Pos

        expand(M, i + 1, Pos, O, T, G, Q, k);
    }
}

// int main() {
//     // Example usage
//     Graph G;
//     Graph Q;
//     Tree* T = new Tree;

//     Match(G, Q, T, k);

//     // Further processing

//     return 0;
// }
