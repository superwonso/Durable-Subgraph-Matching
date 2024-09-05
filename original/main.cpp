#include <iostream>
#include <ctime>
#include "bloom_filter.h"
#include "utils.h"
#include "grow_td_tree.h"
#include "match_td_tree.h"
#include "trim_td_tree.h"
#include "decompose_query.h"
using namespace std;

extern int k;  // Global variable k

int main() {
    clock_t start, finish;
    double duration;

    // Load the graph data from files
    cout << "Loading graph data..." << endl;
    Graph G = read_graph_from_file("../Dataset/testdata.txt");
    cout << "Graph data loaded." << endl;
    cout << "Loading query data..." << endl;
    Graph Q = read_graph_from_file("../Dataset/Query.txt");
    cout << "Query data loaded." << endl;
    
    Tree* T = new Tree;
    start = clock();
    // Initialize the tree
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
    save_tree_to_file(T, "./candidates.txt");
    // Phase 3: Enumeration of Durable Subgraph Matchings
    cout << "Phase 3: Enumeration of Durable Subgraph Matchings..." << endl;
    Match(G, Q, T, k);
    cout << "Enumeration of Durable Subgraph Matchings completed." << endl;

    // Optionally trim the TD-Tree to optimize further queries
    cout << "Trimming TD-Tree for optimization..." << endl;
    T = TrimTDTree(T);
    cout << "TD-Tree trimming completed." << endl;

    finish = clock();
    duration = (double)(finish - start);
    cout << duration << "ms to run code" << endl;

    // Further processing with the trimmed TD-Tree or output results
    // ...
    save_tree_to_file(T, "./output.txt");
    // Clean up memory
    delete T;

    return 0;
}
