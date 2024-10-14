// Graph.cpp
#include "Graph.h"

std::vector<int> Graph::getVerticesByLabel(const std::string& label) const {
    std::vector<int> result;
    for(int i = 0; i < numVertices; ++i){
        if(vertexLabels[i] == label){
            result.push_back(i);
        }
    }
    return result;
}

std::vector<std::pair<int, std::vector<int>>> Graph::getAdjacent(int vertex) const {
    std::vector<std::pair<int, std::vector<int>>> adj;
    for(const auto &edge : edges){
        if(edge.from == vertex){
            adj.emplace_back(std::make_pair(edge.to, edge.timeInstances));
        }
    }
    return adj;
}