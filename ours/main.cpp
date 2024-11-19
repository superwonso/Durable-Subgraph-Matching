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
    std::unordered_map<std::string, int> label_counts;

    for (int u = 0; u < temporalGraph.adj.size(); ++u) {
        for (const auto& edge : temporalGraph.adj[u]) {
            label_counts[edge.label]++;
        }
    }

    std::unordered_map<std::string, int> label_vertex_counts; // 각 레이블의 정점 개수 저장
    std::unordered_map<std::string, int> label_total_durations; // 각 레이블의 총 지속 시간 저장
    std::unordered_map<int, int> vertex_durations; // 각 정점의 총 지속 시간 저장

    // 정점별 간선 정보를 활용하여 지속 시간 계산
    for (int u = 0; u < temporalGraph.adj.size(); ++u) {
        int total_duration = 0;

        for (const auto& edge : temporalGraph.adj[u]) {
            // 각 간선이 존재하는 스냅샷의 개수를 이용해 지속 시간 계산
            auto it = temporalGraph.edge_time_instances.find({u, edge.to});
            if (it != temporalGraph.edge_time_instances.end()) {
                const auto& time_instances = it->second;
                int duration = time_instances.size();  // 간선이 존재하는 스냅샷의 개수
                total_duration += duration;
            }
        }

        // 현재 정점에 대한 총 지속 시간 저장
        vertex_durations[u] = total_duration;

        // 레이블별 지속 시간 및 정점 개수 누적
        const std::string& label = temporalGraph.vertex_labels[u];
        label_vertex_counts[label]++;
        label_total_durations[label] += total_duration;
    }

    // 각 레이블의 평균 수명을 계산
    std::unordered_map<std::string, double> label_average_lifespans;
    for (const auto& label_entry : label_vertex_counts) {
        const std::string& label = label_entry.first;
        int vertex_count = label_entry.second;

        if (vertex_count > 0) {
            label_average_lifespans[label] = static_cast<double>(label_total_durations[label]) / vertex_count;
        } else {
            label_average_lifespans[label] = 0.0; // 정점이 없는 경우 평균 수명은 0으로 설정
        }
    }

    // // 평균 수명 확인을 위한 출력
    // for (const auto& label_avg : label_average_lifespans) {
    //     std::cout << "Label: " << label_avg.first << ", Average Lifespan: " << label_avg.second << std::endl;
    // }

    std::cout << "Label Counted " << std::endl;
    // Perform query decomposition
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    QueryDecomposition qd = decomposeQuery(queryGraph, label_counts, label_average_lifespans);
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
