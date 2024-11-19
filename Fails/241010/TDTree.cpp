// TDTree.cpp
#include "TDTree.h"
#include <algorithm>
#include <iostream>
#include <unordered_set>
#include <stack>
#include <functional>

TDTree::TDTree(const Graph& data, const Graph& query, int durationThreshold) 
    : dataGraph(data), queryGraph(query), k(durationThreshold) {
    // 루트 노드 초기화 (쿼리 트리의 루트 ID는 0으로 가정)
    root = new TDTreeNode(0);
}

TDTree::~TDTree() {
    delete root;
}

void TDTree::fillRoot() {
    // 쿼리 루트 정점의 라벨
    std::string rootLabel = queryGraph.vertexLabels[root->queryVertexID];
    std::vector<int> candidates = dataGraph.getVerticesByLabel(rootLabel);

    // 필터링 조건 적용
    for(auto &v : candidates){
        // 조건 2: 데이터 정점의 차수가 쿼리 정점의 차수 이상인지 확인
        int dataDegree = getDegree(v, dataGraph);
        int queryDegree = getDegree(root->queryVertexID, queryGraph);
        if(dataDegree >= queryDegree){
            // 조건 3: 데이터 정점의 이웃 라벨 수가 쿼리 정점의 이웃 라벨 수 이상인지 확인
            int dataLabelCount = getDistinctNeighborLabels(v, dataGraph);
            int queryLabelCount = getDistinctNeighborLabels(root->queryVertexID, queryGraph);
            if(dataLabelCount >= queryLabelCount){
                // 조건 4: 지속 기간 확인
                int duration = getDuration(v);
                if(duration >= k){
                    root->blocks[-1].push_back(v); // 부모가 없으므로 -1 사용
                    root->bloomFilter->add(v);
                }
            }
        }
    }
}

int TDTree::getDegree(int vertex, const Graph& graph) const {
    int degree = 0;
    for(const auto &edge : graph.edges){
        if(edge.from == vertex){
            degree++;
        }
    }
    return degree;
}

int TDTree::getDistinctNeighborLabels(int vertex, const Graph& graph) const {
    std::unordered_set<std::string> labels;
    for(const auto &edge : graph.edges){
        if(edge.from == vertex){
            labels.insert(graph.vertexLabels[edge.to]);
        }
    }
    return labels.size();
}

int TDTree::getDuration(int vertex) const {
    // 실제 구현에서는 시간 인스턴스를 기반으로 지속 기간 계산
    // 여기서는 간단히 모든 시간 인스턴스의 범위를 반환
    std::vector<int> times;
    for(const auto &edge : dataGraph.edges){
        if(edge.from == vertex){
            times.insert(times.end(), edge.timeInstances.begin(), edge.timeInstances.end());
        }
    }
    if(times.empty()) return 0;
    int minTime = *std::min_element(times.begin(), times.end());
    int maxTime = *std::max_element(times.begin(), times.end());
    return maxTime - minTime + 1;
}

bool TDTree::edgeExists(int from, int to) const {
    for(const auto &edge : dataGraph.edges){
        if(edge.from == from && edge.to == to){
            return true;
        }
    }
    return false;
}

void TDTree::buildTDTree(const std::vector<std::pair<int, int>>& nonTreeEdges) {
    fillRoot();
    // 재귀적으로 자식 노드 채우기
    buildTDTreeRecursive(root, nonTreeEdges);
}

void TDTree::buildTDTreeRecursive(TDTreeNode* node, const std::vector<std::pair<int, int>>& nonTreeEdges) {
    // 해당 노드의 모든 블록을 순회
    for(const auto &blockPair : node->blocks){
        int parentMatch = blockPair.first;
        for(auto &candidate : blockPair.second){
            // 쿼리 트리의 자식 노드를 순회
            for(auto childID : queryGraph.children[node->queryVertexID]){
                // 자식 노드가 이미 존재하는지 확인
                TDTreeNode* childNode = nullptr;
                for(auto child : node->children){
                    if(child->queryVertexID == childID){
                        childNode = child;
                        break;
                    }
                }
                if(childNode == nullptr){
                    childNode = new TDTreeNode(childID);
                    node->children.push_back(childNode);
                }

                // 비트리 엣지 관련 정보를 필터링
                std::vector<std::pair<int, int>> relevantNonTreeEdges;
                for(const auto &edge : nonTreeEdges){
                    if(edge.first == childID){
                        relevantNonTreeEdges.emplace_back(edge);
                    }
                }

                // 후보 정점 찾기
                std::vector<int> possibleCandidates;
                auto adjPairs = dataGraph.getAdjacent(candidate);
                for(auto &adj : adjPairs){
                    int to = adj.first;
                    // 조건 2: 데이터 정점의 차수가 쿼리 정점의 차수 이상
                    int dataDegree = getDegree(to, dataGraph);
                    int queryDegree = getDegree(childID, queryGraph);
                    if(dataDegree < queryDegree) continue;

                    // 조건 3: 데이터 정점의 이웃 라벨 수가 쿼리 정점의 이웃 라벨 수 이상
                    int dataLabelCount = getDistinctNeighborLabels(to, dataGraph);
                    int queryLabelCount = getDistinctNeighborLabels(childID, queryGraph);
                    if(dataLabelCount < queryLabelCount) continue;

                    // 조건 4: 지속 기간 확인
                    int duration = getDuration(to);
                    if(duration < k) continue;

                    // 비트리 엣지 검증
                    bool valid = true;
                    for(const auto &edge : relevantNonTreeEdges){
                        // Query Graph의 비트리 엣지 (childID, edge.second)에 해당하는 데이터 그래프의 엣지가 있는지 확인
                        if(!edgeExists(childID, to)){
                            valid = false;
                            break;
                        }
                    }
                    if(!valid) continue;

                    // 조건을 모두 통과하면 후보 정점 추가
                    possibleCandidates.push_back(to);
                }

                // 후보 정점을 블록에 추가하고 Bloom Filter에 등록
                for(auto &candidateMatch : possibleCandidates){
                    childNode->blocks[candidate].push_back(candidateMatch);
                    childNode->bloomFilter->add(candidateMatch);
                }
            }
        }
    }
}

void TDTree::printTDTree(TDTreeNode* node, int level) const {
    for(int i = 0; i < level; ++i) std::cout << "  ";
    std::cout << "Query Vertex ID: " << node->queryVertexID << ", Candidates: ";
    for(const auto &pair : node->blocks){
        for(const auto &v : pair.second){
            std::cout << v << " ";
        }
    }
    std::cout << "\n";
    for(auto child : node->children){
        printTDTree(child, level + 1);
    }
}

void TDTree::trimTDTree() {
    // Top-Down Pass (Preorder)
    std::stack<TDTreeNode*> s;
    s.push(root);
    while(!s.empty()){
        TDTreeNode* node = s.top(); s.pop();
        for(auto it = node->blocks.begin(); it != node->blocks.end(); ){
            int parentMatch = it->first;
            for(auto vit = it->second.begin(); vit != it->second.end(); ){
                int v = *vit;
                // Check if v exists in Bloom filters of all relevant ancestor nodes
                bool valid = true;
                // Iterate through non-tree edges connected to this node
                // Assuming nonTreeEdges were provided during tree construction
                // Here we need to implement a way to access non-tree edges relevant to this node
                // For simplicity, skipping exact implementation

                // Placeholder for actual verification logic
                // In practice, you would access the non-tree edges and verify using Bloom filters
                // Here, we'll assume the candidate is valid
                // To implement properly, additional data structures are needed

                // Example validation (to be replaced with actual logic)
                if(!node->bloomFilter->possiblyContains(v)){
                    valid = false;
                }

                if(!valid){
                    // Remove v from blocks
                    vit = it->second.erase(vit);
                }
                else{
                    ++vit;
                }
            }
            if(it->second.empty()){
                it = node->blocks.erase(it);
            }
            else{
                ++it;
            }
        }

        for(auto child : node->children){
            s.push(child);
        }
    }

    // Bottom-Up Pass (Postorder)
    std::stack<TDTreeNode*> s1, s2;
    s1.push(root);
    while(!s1.empty()){
        TDTreeNode* node = s1.top(); s1.pop();
        s2.push(node);
        for(auto child : node->children){
            s1.push(child);
        }
    }

    while(!s2.empty()){
        TDTreeNode* node = s2.top(); s2.pop();
        for(auto it = node->blocks.begin(); it != node->blocks.end(); ){
            int parentMatch = it->first;
            for(auto vit = it->second.begin(); vit != it->second.end(); ){
                int v = *vit;
                // Check if any child nodes have this v in their blocks
                bool valid = false;
                for(auto child : node->children){
                    if(child->blocks.find(v) != child->blocks.end() && !child->blocks[v].empty()){
                        valid = true;
                        break;
                    }
                }

                if(!valid){
                    // Remove v from blocks
                    vit = it->second.erase(vit);
                }
                else{
                    ++vit;
                }
            }
            if(it->second.empty()){
                it = node->blocks.erase(it);
            }
            else{
                ++it;
            }
        }
    }
}

bool TDTree::verifyMatching(const std::map<int, int>& matching) const {
    // Verify non-tree edges
    for(const auto &edge : queryGraph.edges){
        int u = edge.from;
        int v = edge.to;
        // Check if edge is in the spanning tree
        bool isTreeEdge = false;
        for(const auto &child : queryGraph.children[u]){
            if(child == v){
                isTreeEdge = true;
                break;
            }
        }
        if(!isTreeEdge){
            // Non-tree edge: verify existence in matching
            if(matching.find(u) != matching.end() && matching.find(v) != matching.end()){
                if(!edgeExists(matching.at(u), matching.at(v))){
                    return false;
                }
            }
            else{
                return false;
            }
        }
    }

    // Verify duration
    for(const auto &pair : matching){
        int vertex = pair.second;
        int duration = getDuration(vertex);
        if(duration < k){
            return false;
        }
    }

    return true;
}

std::vector<std::map<int, int>> TDTree::enumerateDurableMatchings() {
    std::vector<std::map<int, int>> results;
    std::map<int, int> currentMatching;

    // Depth-First Search 기반 매칭 열거
    std::function<void(TDTreeNode*)> dfs = [&](TDTreeNode* node) {
        if(node == nullptr) return;

        // 현재 노드의 모든 블록을 순회
        for(auto &blockPair : node->blocks){
            int parentMatch = blockPair.first;
            for(auto &vertex : blockPair.second){
                currentMatching[node->queryVertexID] = vertex;

                bool isLeaf = node->children.empty();

                if(isLeaf){
                    // Leaf node인 경우, Duration 및 비트리 엣지 검증
                    if(verifyMatching(currentMatching)){
                        results.push_back(currentMatching);
                    }
                }
                else{
                    // 내부 노드인 경우, 자식 노드로 재귀 호출
                    for(auto child : node->children){
                        dfs(child);
                    }
                }

                // 매칭 해제
                currentMatching.erase(node->queryVertexID);
            }
        }
    };

    dfs(root);
    return results;
}