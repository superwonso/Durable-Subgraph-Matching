// TDTreeNode.cpp
#include "TDTreeNode.h"

TDTreeNode::TDTreeNode(int qID, int bloomSize, int numHashes) 
    : queryVertexID(qID), bloomFilter(new BloomFilter(bloomSize, numHashes)) {}

TDTreeNode::~TDTreeNode() {
    delete bloomFilter;
    for(auto child : children){
        delete child;
    }
}