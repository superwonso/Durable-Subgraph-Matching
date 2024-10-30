#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <map>
#include <algorithm>

using namespace std;

bool hasConsecutiveSnapshots(const vector<int>& snapshots) {
    for (size_t i = 1; i < snapshots.size(); i++) {
        if (snapshots[i] == snapshots[i-1] + 1) {
            return true;
        }
    }
    return false;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <input_file>" << endl;
        return 1;
    }

    // 입력 파일 열기
    ifstream inFile(argv[1]);
    if (!inFile) {
        cerr << "Error: Cannot open input file " << argv[1] << endl;
        return 1;
    }

    // 출력 파일 이름 생성 (입력파일명_filtered.txt)
    string outFileName = string(argv[1]);
    size_t dot_pos = outFileName.find_last_of(".");
    if (dot_pos != string::npos) {
        outFileName = outFileName.substr(0, dot_pos);
    }
    outFileName += "_filtered.txt";

    // 출력 파일 열기
    ofstream outFile(outFileName);
    if (!outFile) {
        cerr << "Error: Cannot create output file " << outFileName << endl;
        return 1;
    }

    // 엣지 정보를 저장할 맵
    map<pair<int,int>, vector<int>> edges;
    
    // 파일에서 데이터 읽기
    string line;
    while (getline(inFile, line)) {
        istringstream iss(line);
        int v1, v2, snapshot;
        if (iss >> v1 >> v2 >> snapshot) {
            edges[{v1, v2}].push_back(snapshot);
        }
    }
    inFile.close();

    // 연속된 스냅샷이 있는 엣지만 출력 파일에 쓰기
    for (const auto& edge : edges) {
        vector<int> snapshots = edge.second;
        sort(snapshots.begin(), snapshots.end());
        snapshots.erase(unique(snapshots.begin(), snapshots.end()), snapshots.end());
        
        if (hasConsecutiveSnapshots(snapshots)) {
            for (int snapshot : snapshots) {
                outFile << edge.first.first << " " 
                       << edge.first.second << " " 
                       << snapshot << endl;
            }
        }
    }

    outFile.close();
    cout << "Filtered data has been written to: " << outFileName << endl;

    return 0;
}