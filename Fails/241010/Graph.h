// Graph.h
#ifndef GRAPH_H
#define GRAPH_H

#include <vector>
#include <string>
#include <utility>

struct Edge {
    int from;
    int to;
    std::vector<int> timeInstances; // 시간 인스턴스 집합 T_S
};

struct Graph {
    int numVertices;
    std::vector<std::string> vertexLabels;
    std::vector<Edge> edges;

    // Query Tree를 위한 부모-자식 관계
    std::vector<std::vector<int>> children; // children[u]는 u의 자식 노드들의 목록

    // 정점 라벨에 따라 정점 찾기
    std::vector<int> getVerticesByLabel(const std::string& label) const;

    // 특정 정점에 인접한 정점과 간선 시간 인스턴스 찾기
    std::vector<std::pair<int, std::vector<int>>> getAdjacent(int vertex) const;
};

#endif // GRAPH_H