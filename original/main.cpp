#include <iostream>
#include <chrono>
#include "TDTree.h"
#include "query_decomposition.h"
#include "Utils.h"

int main(int argc, char* argv[]){
    // File paths for the temporal graph and query graph

    if(argc != 4){
        std::cerr << "사용법: " << argv[0] << " <데이터 그래프 파일> <쿼리 그래프 파일> <최소 지속 기간 k>\n";
        return 1;
    }

    std::string temporalGraphFile = argv[1];
    std::string queryGraphFile = argv[2];
    // Minimum duration threshold
    int k = std::stoi(argv[3]);

    // Read the temporal graph
    Graph temporalGraph;
    if(!readTemporalGraph(temporalGraphFile, temporalGraph)){
        std::cerr << "Failed to read temporal graph." << std::endl;
        return -1;
    }
    std::cout << "Temporal Graph Read" << std::endl;
    // Read the query graph
    Graph queryGraph;
    if(!readQueryGraph(queryGraphFile, queryGraph)){
        std::cerr << "Failed to read query graph." << std::endl;
        return -1;
    }
    std::cout<< "Query Graph Read" << std::endl;
    // Define label_counts based on temporal graph labels
    // Assuming label_counts map contains counts of each label in the data graph
    std::cout << "Start Label Counting" << std::endl;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::unordered_map<std::string, int> label_counts;

    for (int u = 0; u < temporalGraph.adj.size(); ++u) {
        for (const auto& edge : temporalGraph.adj[u]) {
            label_counts[edge.label]++;
        }
    }
    
    std::cout << "Label Counted " << std::endl;
    // Perform query decomposition
    QueryDecomposition qd = decomposeQuery(queryGraph, label_counts);
    std::cout << "Query Decomposed" << std::endl; 
    // Create and build the TDTree
    TDTree tdTree(temporalGraph, queryGraph ,qd, k);
    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
    std::chrono::milliseconds millisec = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    std::cout << "Time consumed: " << millisec.count() << " ms" << std::endl;
    std::cout << "TDTree Created" << std::endl;
    // Print the TD-Tree
    // tdTree.print_res();

    return 0;
}
