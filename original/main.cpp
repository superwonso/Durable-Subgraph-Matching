#include <iostream>
#include "TDTree.h"
#include "query_decomposition.h"
#include "Utils.h"

int main(){
    // File paths for the temporal graph and query graph
    std::string temporalGraphFile = "temporal_graph.txt";
    std::string queryGraphFile = "query_graph.txt";

    // Read the temporal graph
    Graph temporalGraph;
    if(!readTemporalGraph(temporalGraphFile, temporalGraph)){
        std::cerr << "Failed to read temporal graph." << std::endl;
        return -1;
    }

    // Read the query graph
    Graph queryGraph;
    if(!readQueryGraph(queryGraphFile, queryGraph)){
        std::cerr << "Failed to read query graph." << std::endl;
        return -1;
    }

    // Define label_counts based on temporal graph labels
    // Assuming label_counts map contains counts of each label in the data graph
    std::unordered_map<std::string, int> label_counts;
    for(int u = 0; u < temporalGraph.num_vertices; ++u){
        for(const auto& edge : temporalGraph.adj[u]){
            label_counts[edge.label]++;
        }
    }

    // Perform query decomposition
    QueryDecomposition qd = decomposeQuery(queryGraph, label_counts);

    // Minimum duration threshold
    int k = 3; // Example value, set as needed

    // Create and build the TDTree
    TDTree tdTree(temporalGraph, qd, k);

    // Print the TD-Tree
    tdTree.print();

    return 0;
}
