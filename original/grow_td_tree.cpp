#include <iostream>
#include <vector>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <string>
#include <cstdlib>
#include <ctime>
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
        const auto& edge_map = g.edges[v1];  // This is unordered_map<int, TimeEdge>

        // Find if there is an edge between v1 and v2
        auto it = edge_map.find(v2);
        if (it != edge_map.end()) {
            // Edge found, add all time instances to result
            result.insert(it->second.time_instances.begin(), it->second.time_instances.end());
        }
    }

    return result;
}

// Decompose graph into tree and non-tree edge set
Tree* convert_graph_to_tree(Graph G) {
    Tree* T = new Tree();
    T->root = new TreeNode();
    T->root->label = 0;
    T->root->parent = nullptr;
    T->root->children = {};
    T->root->V_cand = {};
    T->root->bloom_filter = new BloomFilter(100, 3);
    
    // Initialize the root's candidate set
    for (const auto& pair : G.nodes) {
        T->root->V_cand.insert(pair.first);
    }

    for (const auto& pair : G.nodes) {
        int v = pair.first;
        TreeNode* parent = T->root;

        for (int neighbor : pair.second.neighbor_labels) {
            if (v < neighbor) {
                TreeNode* child = new TreeNode();
                child->label = neighbor;
                child->parent = parent;
                child->children = {};
                child->V_cand = {};
                child->bloom_filter = new BloomFilter(100, 3);

                // Initialize the candidate set for the child node
                for (const auto& pair : G.nodes) {
                    if (pair.second.label == neighbor) {
                        child->V_cand.insert(pair.first);
                    }
                }

                parent->children.push_back(child);
                parent = child;
            }
        }
    }

    return T;
}


// Algorithm 1: GrowTDTree
Tree* GrowTDTree(Graph G, Graph Q, Tree* T, int k) {
    ::g = G;
    ::q = Q;
    ::k = k;
    T = init_tree(T);
    cout << "Tree initialized" << endl;
    fill_root(T->root);

    for (TreeNode* c : T->root->children) {
        for (int v : T->root->V_cand) {
            set<int> TS_set = TS(c->id, v);
            fill_node(c, v, TS_set, G, k);
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
    cout << "Def bloom filter" << endl;
    b->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for the new node
    c->children.push_back(b);
    cout << "Def vars" << endl;

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
            // Attach TS_result to v_prime
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

Tree* init_tree(const Tree* query_tree) {
    // Create a new tree T (TD-tree) based on the structure of the input query tree
    Tree* td_tree = new Tree();
    
    // Initialize the root node of the TD-tree based on the query tree's root
    td_tree->root = new TreeNode();
    td_tree->root->label = query_tree->root->label;
    td_tree->root->parent = nullptr;
    td_tree->root->children = {};
    td_tree->root->V_cand = {}; // Candidate set will be filled later
    td_tree->root->bloom_filter = new BloomFilter(100, 3); // Example BloomFilter initialization

    // Recursive helper function to initialize the TD-tree based on the query tree
    function<void(TreeNode*, TreeNode*)> init_subtree = [&](TreeNode* td_node, TreeNode* query_node) {
        // For each child in the query tree, create the corresponding child in the TD-tree
        for (TreeNode* query_child : query_node->children) {
            // Create a new child node for the TD-tree
            TreeNode* td_child = new TreeNode();
            td_child->label = query_child->label;
            td_child->parent = td_node;
            td_child->children = {};
            td_child->V_cand = {}; // Candidate set will be filled later
            td_child->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for each child node

            // Add the child node to the current TD-tree node
            td_node->children.push_back(td_child);

            // Recursively initialize the children of this node
            init_subtree(td_child, query_child);
        }
    };

    // Initialize the entire tree structure recursively
    init_subtree(td_tree->root, query_tree->root);

    return td_tree;
}



int main() {
    clock_t start, finish;
    double duration;
    Tree* T = new Tree;

    start = clock();
    cout << "Start" << endl;
    Graph G = read_graph_from_file("../Dataset/testdata.txt");
    cout << "Graph G read" << endl;
    Graph Q = read_graph_from_file("../Dataset/query.txt");
    cout << "Graph Q read" << endl;
    Tree* result = GrowTDTree(G, Q, T, k);
    cout << "Tree grown" << endl;
    // Further processing with result

    finish = clock();
    duration = (double)(finish - start);
    cout << duration << "ms to run code" << endl;
    save_tree_to_file(result, "./grow_td_tree.txt");

    return 0;
}
