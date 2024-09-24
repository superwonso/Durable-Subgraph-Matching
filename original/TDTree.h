// TDTree.h
#ifndef TDTREE_H
#define TDTREE_H

#include "Graph.h"
#include "TDTreeNode.h"
#include <string>
#include <vector>
#include <map>
#include <utility>

class TDTree {
public:
    TDTreeNode* root;
    Graph dataGraph;
    Graph queryGraph;
    int k; // 최소 지속 기간

    TDTree(const Graph& data, const Graph& query, int durationThreshold);
    ~TDTree();

    void buildTDTree(const std::vector<std::pair<int, int>>& nonTreeEdges);
    void printTDTree(TDTreeNode* node, int level = 0) const;

    // 트리 트리밍
    void trimTDTree();

    // 매칭 열거
    std::vector<std::map<int, int>> enumerateDurableMatchings();

private:
    // 후보 정점 찾기
    void fillRoot();
    void buildTDTreeRecursive(TDTreeNode* node, const std::vector<std::pair<int, int>>& nonTreeEdges);

    // 지속 기간 계산
    int getDuration(int vertex) const;

    // 정점의 차수 계산
    int getDegree(int vertex, const Graph& graph) const;

    // 정점의 이웃 라벨 중 고유한 라벨 수 계산
    int getDistinctNeighborLabels(int vertex, const Graph& graph) const;

    // 특정 엣지가 데이터 그래프에 존재하는지 확인
    bool edgeExists(int from, int to) const;

    // 매칭 검증
    bool verifyMatching(const std::map<int, int>& matching) const;
};

#endif // TDTREE_H