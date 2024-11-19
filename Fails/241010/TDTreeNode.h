// TDTreeNode.h
#ifndef TDTREENODE_H
#define TDTREENODE_H

#include <map>
#include <vector>
#include "BloomFilter.h"

struct TDTreeNode {
    int queryVertexID; // 쿼리 그래프의 정점 ID
    std::map<int, std::vector<int>> blocks; // 부모 정점 ID -> 후보 정점 ID 목록
    BloomFilter* bloomFilter;

    // 자식 노드들
    std::vector<TDTreeNode*> children;

    TDTreeNode(int qID, int bloomSize = 1000, int numHashes = 3);
    ~TDTreeNode();
};

#endif // TDTREENODE_H