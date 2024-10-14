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
#include <random>
#include <unordered_map>

// 함수 선언
Graph readGraphFromFile(const std::string& filename);
Graph readGraphFromFileWithLabels(const std::string& filename);

std::vector<std::pair<int, int>> decomposeQueryGraph(const Graph& queryGraph, int rootID);

int main(int argc, char* argv[]){
    if(argc != 4){
        std::cerr << "사용법: " << argv[0] << " <데이터 그래프 파일> <쿼리 그래프 파일> <최소 지속 기간 k>\n";
        return 1;
    }

    std::string dataGraphFile = argv[1];
    std::string queryGraphFile = argv[2];
    int k = std::stoi(argv[3]);

    std::cout << "Data Graph File: " << dataGraphFile << "\n";
    // 그래프 파일 읽기
    Graph dataGraph = readGraphFromFile(dataGraphFile);
    Graph queryGraph = readGraphFromFileWithLabels(queryGraphFile);

    std::cout << "Data Graph: " << dataGraph.numVertices << " vertices, " << dataGraph.edges.size() << " edges\n";
    // Query Decomposition: 스패닝 트리와 비트리 엣지 분리
    int rootID = 0; // 루트 정점 ID (필요 시 동적으로 선택)
    std::vector<std::pair<int, int>> nonTreeEdges = decomposeQueryGraph(queryGraph, rootID);

    std::cout << "스패닝 트리 엣지들을 부모-자식 관계에 추가\n";
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
    
    std::cout<< "TD-Tree 생성\n";
    // TD-Tree 생성
    TDTree tdTree(dataGraph, queryGraph, k);
    tdTree.buildTDTree(nonTreeEdges);
    std::cout<< "TD-Tree 생성 완료\n";

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

Graph readGraphFromFile(const std::string& filename) {
    Graph graph;
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "파일을 열 수 없습니다: " << filename << "\n";
        exit(1);
    }

    infile >> graph.numVertices;
    graph.vertexLabels.resize(graph.numVertices);

    // 레이블 목록 정의
    std::vector<std::string> predefinedLabels = {"A", "B", "C", "D", "E"};
    std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<int> dist(0, predefinedLabels.size() - 1);

    // 각 정점에 무작위로 레이블 할당
    for (int i = 0; i < graph.numVertices; ++i) {
        graph.vertexLabels[i] = predefinedLabels[dist(rng)];
    }

    int numEdges;
    infile >> numEdges;
    graph.edges.resize(numEdges);

    for (int i = 0; i < numEdges; ++i) {
        infile >> graph.edges[i].from >> graph.edges[i].to;
        int time;
        while (infile >> time) {
            graph.edges[i].timeInstances.push_back(time);
            if (infile.peek() == '\n' || infile.peek() == EOF) break;
        }
    }

    infile.close();

    // children 벡터 초기화
    graph.children.resize(graph.numVertices, std::vector<int>());

    return graph;
}

Graph readGraphFromFileWithLabels(const std::string& filename) {
    Graph queryGraph;
    std::ifstream infile(filename);
    if (!infile.is_open()) {
        std::cerr << "쿼리 그래프 파일을 열 수 없습니다: " << filename << "\n";
        exit(1);
    }

    std::string label1, label2;
    std::vector<std::pair<std::string, std::string>> edgesList;

    // 파일에서 모든 간선을 읽어들임
    while (infile >> label1 >> label2) {
        edgesList.emplace_back(std::make_pair(label1, label2));
    }

    // 쿼리 그래프의 정점에 고유 ID 할당
    std::unordered_map<std::string, int> queryLabelToId;
    int currentId = 0;
    for(auto &edgePair : edgesList){
        if(queryLabelToId.find(edgePair.first) == queryLabelToId.end()){
            queryLabelToId[edgePair.first] = currentId++;
        }
        if(queryLabelToId.find(edgePair.second) == queryLabelToId.end()){
            queryLabelToId[edgePair.second] = currentId++;
        }
    }

    // 쿼리 그래프의 정점 라벨 설정
    queryGraph.numVertices = currentId;
    queryGraph.vertexLabels.resize(queryGraph.numVertices);
    for(const auto &pair : queryLabelToId){
        queryGraph.vertexLabels[pair.second] = pair.first;
    }

    // 쿼리 그래프의 간선 추가 (시간 인스턴스는 비워둠)
    for(auto &edgePair : edgesList){
        int id1 = queryLabelToId[edgePair.first];
        int id2 = queryLabelToId[edgePair.second];
        queryGraph.edges.push_back({id1, id2, {}});
    }

    // children 벡터 초기화
    queryGraph.children.resize(queryGraph.numVertices, std::vector<int>());

    infile.close();

    return queryGraph;
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