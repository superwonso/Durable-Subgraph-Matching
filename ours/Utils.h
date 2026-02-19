#ifndef UTILS_H
#define UTILS_H

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <set>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iostream>
#include <random>

// Structure to represent an edge with a label
struct Edge {
    int to;
    std::string label;
};

// Graph structure with edge labels and time instances
struct Graph {
    int num_vertices;
    std::vector<std::vector<Edge>> adj; // Adjacency list with edge labels
    std::vector<std::string> vertex_labels; // Labels of vertices
    // Custom hash function for pair<int, int>
    struct pair_hash {
        inline std::size_t operator()(const std::pair<int, int>& v) const {
            return v.first * 31 + v.second;
        }
    };
    std::unordered_map<std::pair<int, int>, std::unordered_set<int>, pair_hash> edge_time_instances; // Time instances for each edge

// Graph 구조체 내부에 추가
size_t getMemoryUsage() const {
    size_t total = sizeof(Graph);
    // 인접 리스트 메모리
    for (const auto& neighbors : adj) {
        total += sizeof(std::vector<Edge>) + (neighbors.capacity() * sizeof(Edge));
    }
    // 정점 레이블 메모리
    for (const auto& label : vertex_labels) {
        total += label.capacity();
    }
    // 시간 정보(Map & Set) 메모리 - 대략적인 추정
    for (const auto& entry : edge_time_instances) {
        total += sizeof(std::pair<int, int>) + sizeof(std::unordered_set<int>);
        total += entry.second.size() * sizeof(int); // Set 내부 요소
    }
    return total;
}

};

// Function to compute the intersection of two unordered_sets (Time Instance Sets)
std::unordered_set<int> intersectTimeSets(const std::unordered_set<int>& set1, const std::unordered_set<int>& set2);

// Bloom Filter Class
class BloomFilter {
public:
    BloomFilter(size_t size, size_t num_hashes);
    void add(int item);
    bool possiblyContains(int item) const;
    void clear();

private:
    size_t size_;
    size_t num_hashes_;
    std::vector<bool> bits_;
    std::hash<int> hash_fn;

    // Generates hash values for an item
    std::vector<size_t> getHashes(int item) const;
};

// Utility Functions for Graph Operations
namespace GraphUtils {
    // Checks if there is an edge between u and v in the adjacency list
    bool hasEdge(const std::vector<std::vector<Edge>>& adj, int u, int v);

    // Counts the number of distinct labels among the neighbors of a vertex
    int countDistinctNeighborLabels(const std::vector<std::vector<Edge>>& adj, int vertex);
}

// Functions to read graphs from files
bool readTemporalGraph(const std::string& filename, Graph& graph);
bool readQueryGraph(const std::string& filename, Graph& queryGraph);

#endif // UTILS_H
