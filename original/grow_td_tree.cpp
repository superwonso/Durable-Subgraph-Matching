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
#include "decompose_query.h"

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
    // cout << "Getting TS for edge between " << v1 << " and " << v2 << endl;
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
    // cout << "TS set size: " << result.size() << endl;
    return result;
}

// // Decompose graph into tree and non-tree edge set
// Tree* convert_graph_to_tree(Graph G) {
//     Tree* T = new Tree();
//     T->root = new TreeNode();
//     T->root->label = 0;
//     T->root->parent = nullptr;
//     T->root->children = {};
//     T->root->V_cand = {};
//     T->root->bloom_filter = new BloomFilter(100, 3);
    
//     // Initialize the root's candidate set
//     for (const auto& pair : G.nodes) {
//         T->root->V_cand.insert(pair.first);
//     }

//     for (const auto& pair : G.nodes) {
//         int v = pair.first;
//         TreeNode* parent = T->root;

//         for (int neighbor : pair.second.neighbor_labels) {
//             if (v < neighbor) {
//                 TreeNode* child = new TreeNode();
//                 child->label = neighbor;
//                 child->parent = parent;
//                 child->children = {};
//                 child->V_cand = {};
//                 child->bloom_filter = new BloomFilter(100, 3);

//                 // Initialize the candidate set for the child node
//                 for (const auto& pair : G.nodes) {
//                     if (pair.second.label == neighbor) {
//                         child->V_cand.insert(pair.first);
//                     }
//                 }

//                 parent->children.push_back(child);
//                 parent = child;
//             }
//         }
//     }

//     return T;
// }


// Algorithm 1: GrowTDTree
Tree* GrowTDTree(Graph G, Graph Q, Tree* T, int k) {
    ::g = G;
    ::q = Q;
    ::k = k;
    cout << "Init tree" << endl;
    T = init_tree(T);
    cout << "Tree initialized" << endl;
    fill_root(T->root);

    if (T->root->children.empty()) {
        cout << "No children found in root. Exiting." << endl;
        return T;
    }

    for (TreeNode* c : T->root->children) {
        // cout << "Processing child with label: " << c->label << endl;
        for (int v : T->root->V_cand) {
            set<int> TS_set = TS(c->id, v);
            // cout << "Filling node for candidate vertex: " << v << endl;
            fill_node(c, v, TS_set, G, k);
        }
    }
    cout << "Finished growing tree." << endl;
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
    // cout << "Filling node for vertex " << v << " with TS_set size: " << TS_set.size() << endl;
    TreeNode* b = new TreeNode;
    b->label = v;
    b->V_cand = {};
    // cout << "Def bloom filter" << endl;
    b->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for the new node
    c->children.push_back(b);
    // cout << "Def vars" << endl;

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


// // fill_root function
// void fill_root(TreeNode* root) {
//     // Iterate over all vertices in the data graph G
//     cout << "Filling root's candidate set" << endl;

//     for (const auto& pair : g.nodes) {
//         int v = pair.first;
        
//         // Step 1: Create a new block b with b.v_par = v and b.V_cand = empty set
//         TreeNode* b = new TreeNode();
//         b->label = v;
//         b->parent = root;
//         b->V_cand = {}; // Initially empty candidate set
//         b->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for the new block
        
//         // Step 2: Find all edges (v, v') in G such that the edge satisfies the internal node conditions
//         for (const auto& neighbor : g.nodes[v].neighbor_labels) {
//             if (tree_edge_test(v, neighbor, b, root)) {
//                 b->V_cand.insert(neighbor); // Add v' to the candidate set
//                 b->bloom_filter->add(to_string(neighbor)); // Add to bloom filter
//             }
//         }

//         // Step 3: If b is a leaf node (i.e., no children), attach a set of time instances to v'
//         if (b->children.empty()) {
//             for (const int& v_cand : b->V_cand) {
//                 set<int> TS_set = TS(v, v_cand);
//                 b->TS.insert(TS_set.begin(), TS_set.end()); // Attach the time instance set to b
//             }
//         }

//         // Associate the new block with the root node
//         root->children.push_back(b);
//     }
//     cout << "Root V_cand size: " << root->V_cand.size() << endl;

// }

// fill_root function
void fill_root(TreeNode* root) {
    // Step 1: Print debug information
    cout << "Filling root's candidate set" << endl;

    // Step 2: Iterate over all vertices in the data graph G
    for (const auto& pair : g.nodes) {
        int v = pair.first;
        
        // Step 3: Create a new block (node) b for each vertex v in G
        TreeNode* b = new TreeNode();
        b->label = v;
        b->parent = root;
        b->V_cand = {}; // Initially empty candidate set
        b->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for the new block

        // Step 4: Find all edges (v, v') in G that satisfy internal node conditions
        for (const auto& neighbor : g.nodes[v].neighbor_labels) {
            if (tree_edge_test(v, neighbor, b, root)) {
                // Step 5: Add v' to b.V_cand and Bloom filter
                b->V_cand.insert(neighbor); // Add to candidate set
                b->bloom_filter->add(to_string(neighbor)); // Add to Bloom filter
            }
        }

        // Step 6: If b is a leaf node (i.e., no children), attach a set of time instances to v'
        if (b->children.empty()) {
            for (const int& v_cand : b->V_cand) {
                set<int> TS_set = TS(v, v_cand); // Get the time instances
                b->TS.insert(TS_set.begin(), TS_set.end()); // Attach the time instance set to b
            }
        }

        // Step 7: Associate the new block (node) with the root
        root->children.push_back(b);

        // Step 8: Add vertex v to the root's Bloom filter and candidate set
        root->V_cand.insert(v);
        root->bloom_filter->add(to_string(v)); // Also add to Bloom filter for fast vertex containment check
    }

    // Step 9: Print debug information to verify the size of root's V_cand
    cout << "Root V_cand size: " << root->V_cand.size() << endl;
}


// Tree* init_tree(const Tree* query_tree) {
//     // Create a new tree T (TD-tree) based on the structure of the input query tree
//     Tree* td_tree = new Tree();
    
//     // Initialize the root node of the TD-tree based on the query tree's root
//     td_tree->root = new TreeNode();
//     td_tree->root->label = query_tree->root->label;
//     td_tree->root->parent = nullptr;
//     td_tree->root->children = {};
//     td_tree->root->V_cand = {}; // Candidate set will be filled later
//     td_tree->root->bloom_filter = new BloomFilter(100, 3); // Example BloomFilter initialization
//     cout << "Root initialized" << endl;
//     TreeNode* td_child;
//     // Recursive helper function to initialize the TD-tree based on the query tree
//     function<void(TreeNode*, TreeNode*)> init_subtree = [&](TreeNode* td_node, TreeNode* query_node) {
//         // For each child in the query tree, create the corresponding child in the TD-tree
//         cout << "Init subtree" << endl;
//         for (TreeNode* query_child : query_node->children) {
//             cout << "Init child: " << query_child <<endl;
//             // Create a new child node for the TD-tree
//             td_child = new TreeNode();
//             td_child->label = query_child->label;
//             td_child->parent = td_node;
//             td_child->children = {};
//             td_child->V_cand = {}; // Candidate set will be filled later
//             td_child->bloom_filter = new BloomFilter(100, 3); // Initialize BloomFilter for each child node

//             // Add the child node to the current TD-tree node
//             td_node->children.push_back(td_child);

//             // Recursively initialize the children of this node
//             init_subtree(td_child, query_child);
//         }
//         cout << "Subtree initialized" << endl;
//     };

//     // Initialize the entire tree structure recursively
//     cout << "Init subtree recursiv" << endl;
//     init_subtree(td_tree->root, query_tree->root);

//     return td_tree;
// }

Tree* init_tree(const Tree* query_tree) {
    // Create a new TD-tree
    Tree* td_tree = new Tree();
    
    // Initialize the root node of the TD-tree based on the query tree's root
    cout << "Initialize the root node of the TD-tree based on the query tree's root" << endl;
    td_tree->root = new TreeNode();
    td_tree->root->label = query_tree->root->label;
    td_tree->root->id = query_tree->root->id;
    td_tree->root->parent = nullptr;  // Root has no parent
    td_tree->root->bloom_filter = new BloomFilter(100, 3);  // Initialize the bloom filter
    td_tree->root->children = {};
    td_tree->root->V_cand = {};  // Candidate set will be filled later

    // Recursive helper function to copy the structure of the query tree to the TD-tree
    std::function<void(TreeNode*, TreeNode*)> copy_structure = [&](TreeNode* td_node, TreeNode* query_node) {
        cout << "copy structure" << endl;
        for (TreeNode* query_child : query_node->children) {
            // Create a new child node in the TD-tree
            cout << "Create a new child node in the TD-tree" << endl;
            TreeNode* td_child = new TreeNode();
            td_child->label = query_child->label;
            td_child->id = query_child->id;
            td_child->parent = td_node;  // Set parent to the current TD-tree node
            td_child->bloom_filter = new BloomFilter(100, 3);  // Initialize bloom filter for each child
            td_child->children = {};
            td_child->V_cand = {};  // Candidate set will be filled later
            
            // Add the new child to the current TD-tree node's children
            td_node->children.push_back(td_child);
            
            // Recursively copy the structure
            copy_structure(td_child, query_child);
        }
    };

    // Start recursive copying from the root
    copy_structure(td_tree->root, query_tree->root);

    return td_tree;
}

// int main() {
//     clock_t start, finish;
//     double duration;
//     Tree* T = new Tree;
//     set<int> available_labels = {1, 2, 3};

//     start = clock();
//     cout << "Start" << endl;

//     Graph G = read_graph_from_file("../Dataset/testdata.txt", available_labels);
//     cout << "Graph G read" << endl;

//     Graph Q = read_graph_from_file("../Dataset/query.txt", available_labels);
//     cout << "Graph Q read" << endl;
//     T = DecomposeQuery(Q);
//     Tree* result = GrowTDTree(G, Q, T, k);
//     if (result != nullptr) {
//         cout << "Tree grown" << endl;
//     } else {
//         cout << "Tree growth failed." << endl;
//     }

//     // Further processing with result
//     finish = clock();
//     duration = (double)(finish - start);
//     cout << duration << "ms to run code" << endl;
//     save_tree_to_file(result, "./grow_td_tree.txt");

//     return 0;
// }
