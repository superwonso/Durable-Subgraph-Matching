#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include "utils.h"
#include "grow_td_tree.h"
using namespace std;

// Global variables
extern int k;

// Placeholder functions for tree_edge_test and non_tree_edge_test
bool tree_edge_test(int v1, int v2, TreeNode* node, TreeNode* parent) {
    // Get the nodes from the graphs
    TreeNode& node_v1 = g.nodes[v1];
    TreeNode& node_parent = q.nodes[parent->label];

    // Condition 1: The label of v1 is the same as the label of the parent node
    if (node_v1.label != node_parent.label) {
        return false;
    }

    // Condition 2: The degree of v1 in g is greater than or equal to the degree of the parent node in Q
    if (node_v1.degree < node_parent.degree) {
        return false;
    }

    // Condition 3: The number of distinct labels of v1's neighbors in g is greater than or equal to that of the parent's neighbors in Q
    if (node_v1.neighbor_labels.size() < node_parent.neighbor_labels.size()) {
        return false;
    }

    // Condition 4: The duration of v1 in g is at least k
    if (node_v1.duration < k) {
        return false;
    }

    // All conditions are satisfied
    return true;
}

bool non_tree_edge_test(int v, TreeNode* node) {
    TreeNode* current = node;
    while (current->parent != nullptr) {
        if (current->parent->bloom_filter->possiblyContains(to_string(v))) {
            return false;
        }
        current = current->parent;
    }
    return true;
}

// Placeholder function for TS and intersection of TS
// Function to get the time instances of the edge between two vertices
set<int> TS(int v1, int v2) {
    set<int> result;

    // Find the edges for v1 in the graph
    if (g.edges.find(v1) != g.edges.end()) {
        for (const TimeEdge& edge : g.edges[v1]) {
            // Check if the edge connects v1 and v2
            if (edge.v2 == v2) {
                result.insert(edge.time_instances.begin(), edge.time_instances.end());
            }
        }
    }

    // Find the edges for v2 in the graph (to account for undirected edges)
    if (g.edges.find(v2) != g.edges.end()) {
        for (const TimeEdge& edge : g.edges[v2]) {
            // Check if the edge connects v2 and v1
            if (edge.v2 == v1) {
                result.insert(edge.time_instances.begin(), edge.time_instances.end());
            }
        }
    }

    return result;
}

// Function prototypes
void fill_node(TreeNode* c, int v, set<int> TS_set, Graph& G, int k);
void fill_root(TreeNode* root);
Tree* init_tree(Tree* T);
Graph read_graph_from_file(const string& filename);

// Algorithm 1: GrowTDTree
Tree* GrowTDTree(Graph G, Graph Q, Tree* T, int k) {
    ::g = G;
    ::q = Q;
    ::k = k;
    T = init_tree(T);
    fill_root(T->root);

    for (TreeNode* c : T->root->children) {
        for (int v : T->root->V_cand) {
            fill_node(c, v, set<int>{1, 2, 3}, G, k);  // Assuming a set<int> {1, 2, 3} as TS set
        }
    }
    return T;
}

set<int> custom_set_intersection(const set<int>& set1, const set<int>& set2) {
    set<int> intersection;
    for (int elem : set1) {
        if (set2.find(elem) != set2.end()) {
            intersection.insert(elem);
        }
    }
    return intersection;
}

// fill_node function
void fill_node(TreeNode* c, int v, set<int> TS_set, Graph& G, int k) {
    TreeNode* b = new TreeNode;
    b->label = v;
    b->V_cand = {};
    b->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for the new node
    c->children.push_back(b);

    for (int v_prime : G.nodes[v].neighbor_labels) {
        if (!tree_edge_test(v_prime, v, c, c->parent)) continue;
        if (!non_tree_edge_test(v_prime, c)) continue;

        set<int> TS_intersect = custom_set_intersection(TS_set, TS(v, v_prime));

        if (TS_intersect.size() < k) continue;

        b->V_cand.insert(v_prime);
        b->bloom_filter->add(to_string(v_prime)); // Add v_prime to the Bloom filter in b
        
        if (c->children.empty()) {
            // Assuming TS is a function that returns a set
            set<int> TS_result = TS(v, v_prime);
            // Attach TS_result to v_prime (not shown, as implementation detail is missing)
        } else {
            for (TreeNode* c_prime : c->children) {
                fill_node(c_prime, v_prime, TS_intersect, G, k);
            }
        }
    }
}

// fill_root function
void fill_root(TreeNode* root) {
    // Implement the actual fill root logic here
    // Example: Add all vertices with the same label as the root's label to V_cand
    for (const auto& pair : g.nodes) {
        if (pair.second.label == root->label) {
            root->V_cand.insert(pair.first);
        }
    }
}

// init_tree function
Tree* init_tree(Tree* T) {
    // Initialize the tree T here
    T->root = new TreeNode;
    T->root->parent = nullptr;  // Root's parent is undefined
    T->root->V_cand = {0, 1, 2};  // Example candidate vertices for the root
    T->root->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for the root
    return T;
}

// Function to read graph from file
Graph read_graph_from_file(const string& filename) {
    ifstream file(filename);
    Graph G;
    string line;
    while (getline(file, line)) {
        stringstream ss(line);
        int vertex;
        vector<int> edges;
        while (ss >> vertex) {
            edges.push_back(vertex);
        }
        // Assuming the first vertex in each line is the node and rest are its neighbors
        if (!edges.empty()) {
            int node = edges[0];
            TreeNode tree_node;
            for (size_t i = 1; i < edges.size(); ++i) {
                tree_node.neighbor_labels.insert(edges[i]);
            }
            G.nodes[node] = tree_node;
        }
    }
    return G;
}

// int main() {
//     Graph G = read_graph_from_file("../Dataset/bitcoin-temporal-timeinstance.txt");
//     Graph Q = read_graph_from_file("../Dataset/query.txt");
//     Tree* T = new Tree;

//     Tree* result = GrowTDTree(G, Q, T, k);

//     // Further processing with result

//     return 0;
// }
