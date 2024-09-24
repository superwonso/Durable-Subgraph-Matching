// main.cpp
#include "Graph.h"
#include "TDTree.h"
#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <stack>
#include <algorithm>

// 함수 선언
Graph readGraphFromFile(const std::string& filename);
std::vector<std::pair<int, int>> decomposeQueryGraph(const Graph& queryGraph, int rootID);

int main(int argc, char* argv[]){
    if(argc != 4){
        std::cerr << "사용법: " << argv[0] << " <데이터 그래프 파일> <쿼리 그래프 파일> <최소 지속 기간 k>\n";
        return 1;
    }

    std::string dataGraphFile = argv[1];
    std::string queryGraphFile = argv[2];
    int k = std::stoi(argv[3]);

    // 그래프 파일 읽기
    Graph dataGraph = readGraphFromFile(dataGraphFile);
    Graph queryGraph = readGraphFromFile(queryGraphFile);

    // Query Decomposition: 스패닝 트리와 비트리 엣지 분리
    int rootID = 0; // 루트 정점 ID (필요 시 동적으로 선택)
    std::vector<std::pair<int, int>> nonTreeEdges = decomposeQueryGraph(queryGraph, rootID);

    // 스패닝 트리 엣지들을 부모-자식 관계에 추가
    std::vector<int> parent(queryGraph.numVertices, -1);
    std::stack<int> s;
    std::vector<bool> visited(queryGraph.numVertices, false);

    s.push(rootID);
    visited[rootID] = true;

    while(!s.empty()){
        int u = s.top(); s.pop();
        for(const auto &edge : queryGraph.edges){
            if(edge.from == u){
                int v = edge.to;
                if(!visited[v]){
                    visited[v] = true;
                    parent[v] = u;
                    queryGraph.children[u].push_back(v);
                    s.push(v);
                }
            }
        }
    }

    // TD-Tree 생성
    TDTree tdTree(dataGraph, queryGraph, k);
    tdTree.buildTDTree(nonTreeEdges);

    // 트리 트리밍
    tdTree.trimTDTree();

    // 매칭 열거
    std::vector<std::map<int, int>> durableMatchings = tdTree.enumerateDurableMatchings();

    // 결과 출력
    std::cout << "Durable Subgraph Matchings:\n";
    for(const auto &matching : durableMatchings){
        std::cout << "{ ";
        for(const auto &pair : matching){
            std::cout << "Q" << pair.first << "->G" << pair.second << " ";
        }
        std::cout << "}\n";
    }

    return 0;
}

// 그래프 파일 읽기 함수
Graph readGraphFromFile(const std::string& filename) {
    Graph graph;
    std::ifstream infile(filename);
    if(!infile.is_open()){
        std::cerr << "파일을 열 수 없습니다: " << filename << "\n";
        exit(1);
    }

    infile >> graph.numVertices;
    graph.vertexLabels.resize(graph.numVertices);
    for(int i=0; i<graph.numVertices; ++i){
        infile >> graph.vertexLabels[i];
    }

    int numEdges;
    infile >> numEdges;
    graph.edges.resize(numEdges);
    for(int i=0; i<numEdges; ++i){
        infile >> graph.edges[i].from >> graph.edges[i].to;
        std::string line;
        std::getline(infile, line); // 각 간선의 시간 인스턴스를 읽기 위해 나머지 라인 읽기
        std::stringstream ss(line);
        int time;
        while(ss >> time){
            graph.edges[i].timeInstances.push_back(time);
        }
    }

    infile.close();
    return graph;
}

// Query Decomposition 함수: DFS를 사용하여 스패닝 트리와 비트리 엣지 분리
std::vector<std::pair<int, int>> decomposeQueryGraph(const Graph& queryGraph, int rootID){
    std::vector<std::pair<int, int>> nonTreeEdges;
    std::stack<int> s;
    std::vector<bool> visited(queryGraph.numVertices, false);
    std::vector<int> parent(queryGraph.numVertices, -1);

    s.push(rootID);
    visited[rootID] = true;

    while(!s.empty()){
        int u = s.top(); s.pop();
        for(const auto &edge : queryGraph.edges){
            if(edge.from == u){
                int v = edge.to;
                if(!visited[v]){
                    visited[v] = true;
                    parent[v] = u;
                    s.push(v);
                }
                else{
                    if(parent[u] != v){
                        nonTreeEdges.emplace_back(std::make_pair(u, v));
                    }
                }
            }
        }
    }

    return nonTreeEdges;
}