#include <iostream>
#include "bloom_filter.h"
#include "utils.h"
#include "grow_td_tree.h"
#include "match_td_tree.h"
#include "trim_td_tree.h"
#include "decompose_query.h"
using namespace std;

int k = 3;  // Global variable k

int main() {
    // Load the graph data from files
    Graph G = read_graph_from_file("graph.txt");
    Graph Q = read_graph_from_file("query.txt");

    // Initialize the tree
    Tree* T = new Tree;

    // Phase 1: Query Decomposition
    cout << "Phase 1: Query Decomposition..." << endl;
    // This would involve creating a spanning tree T and identifying non-tree edges.
    // Assume some function exists that does this, for example:
    T = DecomposeQuery(Q); // Placeholder function
    cout << "Query Decomposition completed." << endl;

    // Phase 2: Search for Candidate Vertex Matchings
    cout << "Phase 2: Search for Candidate Vertex Matchings..." << endl;
    T = GrowTDTree(G, Q, T, k);
    cout << "Candidate Vertex Matchings found." << endl;

    // Phase 3: Enumeration of Durable Subgraph Matchings
    cout << "Phase 3: Enumeration of Durable Subgraph Matchings..." << endl;
    Match(G, Q, T, k);
    cout << "Enumeration of Durable Subgraph Matchings completed." << endl;

    // Optionally trim the TD-Tree to optimize further queries
    cout << "Trimming TD-Tree for optimization..." << endl;
    T = TrimTDTree(T);
    cout << "TD-Tree trimming completed." << endl;

    // Further processing with the trimmed TD-Tree or output results
    // ...
    save_tree_to_file(T, "output.txt");
    // Clean up memory
    delete T;

    return 0;
}
