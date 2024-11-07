#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <set>
#include <algorithm>
#include <chrono>

using namespace std;

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <input_file>" << endl;
        return 1;
    }

    ifstream inFile(argv[1]);
    if (!inFile) {
        cerr << "Error: Cannot open file " << argv[1] << endl;
        return 1;
    }

    map<pair<int,int>, vector<int>> edges;
    string line;
    
    // 파일에서 데이터 읽기
    while (getline(inFile, line)) {
        istringstream iss(line);
        int v1, v2;
        if (!(iss >> v1 >> v2)) continue;
        
        int snapshot;
        while (iss >> snapshot) {
            if (iss.peek() == ',') iss.ignore();
            edges[{v1,v2}].push_back(snapshot);
        }
    }

    chrono::steady_clock::time_point start = chrono::steady_clock::now();

    // 연속된 스냅샷을 가진 엣지 카운트
    int consecutive_count = 0;
    for (auto& edge : edges) {
        vector<int>& snapshots = edge.second;
        sort(snapshots.begin(), snapshots.end());
        snapshots.erase(unique(snapshots.begin(), snapshots.end()), snapshots.end());
        
        // 연속된 스냅샷이 있는지 확인
        for (size_t i = 1; i < snapshots.size(); i++) {
            if (snapshots[i] == snapshots[i-1] + 1) {
                consecutive_count++;
                break;  // 하나라도 연속된 것이 있으면 다음 엣지로
            }
        }
    }

    chrono::steady_clock::time_point end = chrono::steady_clock::now();
    chrono::milliseconds millisec = chrono::duration_cast<chrono::milliseconds>(end - start);
    cout << "Time consumed: " << millisec.count() << " ms" << endl;
    cout << "Total edges: " << edges.size() << endl;
    cout << "Edges with consecutive snapshots: " << consecutive_count << endl;

    return 0;
}