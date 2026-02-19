#include <iostream>
#include <chrono>
#include <fstream>
#include <unordered_map>
#include <filesystem>
#include "TDTree.h"
#include "query_decomposition.h"
#include "Utils.h"
#include <windows.h>
#include <psapi.h>

size_t getPeakRSS() {
    PROCESS_MEMORY_COUNTERS info;
    GetProcessMemoryInfo(GetCurrentProcess(), &info, sizeof(info));
    return (size_t)info.PeakWorkingSetSize;
}

int main(int argc, char* argv[]){
    // File paths for the temporal graph and query graph

    if(argc != 4){
        std::cerr << "Usage: " << argv[0] << " <Data Graph> <Query Graph> <Minimum Duration k>\n";
        return 1;
    }

    std::string temporalGraphFile = argv[1];
    std::string queryGraphFile = argv[2];
    // Minimum duration threshold
    int k = std::stoi(argv[3]);
    std::string datasetName = std::filesystem::path(temporalGraphFile).stem().string();

    std::unordered_map<std::string, long long> timings;

    // Read the temporal graph
    auto start = std::chrono::steady_clock::now();
    Graph temporalGraph;
    if(!readTemporalGraph(temporalGraphFile, temporalGraph)){
        std::cerr << "Failed to read temporal graph." << std::endl;
        return -1;
    }
    size_t inputGraphMem = temporalGraph.getMemoryUsage();
    timings["readTemporalGraph"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Temporal Graph Read" << std::endl;

    // Read the query graph
    start = std::chrono::steady_clock::now();
    Graph queryGraph;
    if(!readQueryGraph(queryGraphFile, queryGraph)){
        std::cerr << "Failed to read query graph." << std::endl;
        return -1;
    }
    timings["readQueryGraph"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout<< "Query Graph Read" << std::endl;

    // Define label_counts based on temporal graph labels
    // Assuming label_counts map contains counts of each label in the data graph
    std::cout << "Start Label Counting" << std::endl;
    start = std::chrono::steady_clock::now();
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
    timings["labelCounting"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    std::cout << "Label Counted " << std::endl;

    // Perform query decomposition
    start = std::chrono::steady_clock::now();
    QueryDecomposition qd = decomposeQuery(queryGraph, label_counts, label_average_lifespans);
    timings["queryDecomposition"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "Query Decomposed" << std::endl;

    // Create and build the TDTree
    start = std::chrono::steady_clock::now();
    TDTree tdTree(temporalGraph, queryGraph ,qd, k);
    timings["buildTDTree"] = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "TDTree Created" << std::endl;

    // Print matching results
    tdTree.print_res();

    // Save matching results to file
    tdTree.save_res("matching_results_" + datasetName + ".txt");

    // Write timing results to file
    std::ofstream resultFile("timing_results_" + datasetName + ".txt");
    for (const auto& t : timings) {
        resultFile << t.first << ": " << t.second << " ms" << std::endl;
    }

    // 1. TD-Tree 메모리 측정
    size_t tdTreeMem = tdTree.getMemoryUsage();

    // 2. 전체 실행 후 피크 메모리 측정
    // (여기서 매칭 함수 expand() 호출 등이 완료된 후)
    size_t totalPeakMem = getPeakRSS();

    // 3. "Other" 메모리 계산
    // 전체 피크에서 (그래프 + TD-tree)를 뺀 나머지
    size_t usedByKnown = inputGraphMem + tdTreeMem;
    size_t otherMem = (totalPeakMem > usedByKnown) ? (totalPeakMem - usedByKnown) : 0;

    // 결과 출력 (메모리 사용량)
    std::cout << "\n--- Memory Measurement (KB) ---" << std::endl;
    std::cout << "Input Graph Size: " << inputGraphMem / 1024 << " KB" << std::endl;
    std::cout << "DSM (TD-tree): " << tdTreeMem / 1024 << " KB" << std::endl;
    std::cout << "DSM (Other): " << otherMem / 1024 << " KB" << std::endl;
    std::cout << "Total Peak: " << totalPeakMem / 1024 << " KB" << std::endl;

    return 0;
}
