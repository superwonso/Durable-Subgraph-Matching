#include "utils.h"
#include <fstream>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include "utils.h"
// Define global variables
Graph g;
Graph q;
Graph G;
Graph Q;
int k = 3; // Threshold

using namespace std;

// Function to read graph from file
Graph read_graph_from_file(const string& filename) {
    ifstream file(filename);
    Graph G;
    string line;

    // Seed the random number generator
    srand(static_cast<unsigned int>(time(0)));

    while (getline(file, line)) {
        stringstream ss(line);
        int node1, node2, duration;
        ss >> node1 >> node2 >> duration;

        // Ensure node1 exists and assign a random label (1, 2, or 3)
        if (G.nodes.find(node1) == G.nodes.end()) {
            TreeNode node;
            node.id = node1;
            node.label = rand() % 3 + 1; // Random label between 1 and 3
            G.nodes[node1] = node;
        }

        // Ensure node2 exists and assign a random label (1, 2, or 3)
        if (G.nodes.find(node2) == G.nodes.end()) {
            TreeNode node;
            node.id = node2;
            node.label = rand() % 3 + 1; // Random label between 1 and 3
            G.nodes[node2] = node;
        }

        // Add the edge with the time instance
        G.add_edge(node1, node2, duration);
    }

    return G;
}

//Function to read query graph from file
Graph read_query_graph_from_file(const string& filename) {
    ifstream file(filename);
    Graph Q;
    string line;

    while (getline(file, line)) {
        stringstream ss(line);
        int node1, node2;
        ss >> node1 >> node2;

        // Ensure node1 exists and assign a random label (1, 2, or 3)
        if (G.nodes.find(node1) == G.nodes.end()) {
            TreeNode node;
            node.id = node1;
            node.label = rand() % 3 + 1; // Random label between 1 and 3
            G.nodes[node1] = node;
        }

        // Ensure node2 exists and assign a random label (1, 2, or 3)
        if (G.nodes.find(node2) == G.nodes.end()) {
            TreeNode node;
            node.id = node2;
            node.label = rand() % 3 + 1; // Random label between 1 and 3
            G.nodes[node2] = node;
        }

        // Add the edge without the time instance
        G.add_edge(node1, node2, NULL);
    }

    return G;
}


void save_graph_to_file(const Graph& G, const string& filename) {
    ofstream file(filename);
    if (file.is_open()) {
        // Save node information
        for (const auto& node : G.nodes) {
            file << node.first << " " << node.second.label << " " << node.second.degree << endl;
        }

        // Save edge information
        for (const auto& edge_list : G.edges) {
            int node1 = edge_list.first;
            for (const auto& edge_pair : edge_list.second) {
                const TimeEdge& time_edge = edge_pair.second;  // Extract the TimeEdge object
                file << node1 << " " << time_edge.v2 << " ";
                for (int time_instance : time_edge.time_instances) {
                    file << time_instance << " ";
                }
                file << endl;
            }
        }

        file.close();
    }
}


void save_tree_to_file(Tree* T, const string& filename) {
    ofstream file(filename);
    if (file.is_open()) {
        // Helper function to save a tree node and its children
        function<void(TreeNode*, int)> save_node = [&](TreeNode* node, int depth) {
            if (node == nullptr) return;

            // Save the current node (label, degree, depth for hierarchy visualization)
            file << string(depth, '-') << "Node: " << node->label << ", Degree: " << node->degree << endl;

            // Save candidate vertices for the node
            file << string(depth, '-') << "Candidates: ";
            for (int v : node->V_cand) {
                file << v << " ";
            }
            file << endl;

            // Save the Bloom filter state (if applicable)
            file << string(depth, '-') << "Bloom Filter: ";
            // Assuming you have a way to represent the bloom filter (pseudo-code)
            // file << node->bloom_filter->to_string() << endl;
            file << "[Bloom Filter Data]" << endl; // Placeholder for actual Bloom filter data

            // Recursively save each child node
            for (TreeNode* child : node->children) {
                save_node(child, depth + 1);
            }
        };

        // Start saving from the root of the tree
        save_node(T->root, 0);

        file.close();
    } else {
        cerr << "Failed to open file: " << filename << endl;
    }
}

void Graph::add_edge(int v1, int v2, int timeinstance) {
    // Ensure nodes exist
    if (nodes.find(v1) == nodes.end()) {
        nodes[v1] = TreeNode();
        nodes[v1].id = v1;
    }
    if (nodes.find(v2) == nodes.end()) {
        nodes[v2] = TreeNode();
        nodes[v2].id = v2;
    }

    // Check if the edge already exists in the unordered_map
    if (edges[v1].find(v2) != edges[v1].end()) {
        // Edge exists, add the time instance
        edges[v1][v2].time_instances.insert(timeinstance);
        edges[v2][v1].time_instances.insert(timeinstance); // Symmetric edge
    } else {
        // Edge doesn't exist, create a new TimeEdge
        TimeEdge new_edge = {v1, v2, {timeinstance}};
        edges[v1][v2] = new_edge;
        edges[v2][v1] = new_edge;  // For undirected graph
    }

    // Add neighbor labels
    nodes[v1].neighbor_labels.insert(v2);
    nodes[v2].neighbor_labels.insert(v1);
}
