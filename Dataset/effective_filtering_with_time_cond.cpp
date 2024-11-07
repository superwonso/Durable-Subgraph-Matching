#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <utility>
#include <algorithm>
#include <chrono>

using namespace std;

// pair를 위한 해시 함수 (향상된 충돌 방지)
struct pair_hash {
    size_t operator()(const pair<int, int>& p) const {
        // Boost의 hash_combine 유사 방식 사용
        return hash<int>()(p.first) * 31 + hash<int>()(p.second);
    }
};

int main(int argc, char* argv[]) {
    // 프로그램 사용법 검사
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

    // 출력 파일 이름 생성
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

    // 엣지 정보를 저장할 해시맵과 스냅샷을 저장할 해시셋
    unordered_map<pair<int, int>, unordered_set<int>, pair_hash> edges;

    // 입력 데이터의 유효성 검증을 위한 변수
    int invalid_lines = 0;

    // 파일에서 데이터 읽기
    string line;
    while (getline(inFile, line)) {
        if (line.empty()) continue; // 빈 라인 스킵
        istringstream iss(line);
        int v1, v2, snapshot;
        if (iss >> v1 >> v2 >> snapshot) {
            // 유향 엣지이므로 순서를 유지
            edges[{v1, v2}].insert(snapshot);
        } else {
            invalid_lines++;
        }
    }
    inFile.close();

    if (invalid_lines > 0) {
        cerr << "Warning: " << invalid_lines << " invalid lines were skipped." << endl;
    }

    // 처리 시작 시간 기록
    chrono::steady_clock::time_point start = chrono::steady_clock::now();

    // 필터링된 엣지를 저장할 임시 맵
    unordered_map<pair<int, int>, vector<int>, pair_hash> filtered_edges;

    // 엣지 필터링: 단 한 번이라도 연속된 스냅샷을 가진 엣지만 유지
    for (const auto& edge : edges) {
        const auto& snapshot_set = edge.second;

        if (snapshot_set.size() < 2) {
            // 스냅샷이 하나뿐이라면 연속된 스냅샷이 없으므로 스킵
            continue;
        }

        // 정렬된 스냅샷 벡터 생성
        vector<int> sorted_snapshots(snapshot_set.begin(), snapshot_set.end());
        sort(sorted_snapshots.begin(), sorted_snapshots.end());

        // 연속된 스냅샷이 존재하는지 확인
        bool has_consecutive = false;
        for (size_t i = 1; i < sorted_snapshots.size(); ++i) {
            if (sorted_snapshots[i] == sorted_snapshots[i - 1] + 1) {
                has_consecutive = true;
                break;
            }
        }

        if (has_consecutive) {
            // 필터링된 엣지에 추가
            filtered_edges[edge.first] = move(sorted_snapshots);
        }
    }

    // 처리 종료 시간 기록
    chrono::steady_clock::time_point end = chrono::steady_clock::now();
    chrono::milliseconds millisec = chrono::duration_cast<chrono::milliseconds>(end - start);
    cout << "Time consumed: " << millisec.count() << " ms" << endl;
    cout << "Total original edges: " << edges.size() << endl;
    cout << "Total filtered edges: " << filtered_edges.size() << endl;

    // 필터링된 엣지를 출력 파일에 쓰기
    // 버퍼링을 통해 I/O 성능 최적화
    const size_t BUFFER_SIZE = 100000;
    vector<string> output_buffer;
    output_buffer.reserve(BUFFER_SIZE);

    for (const auto& edge : filtered_edges) {
        const auto& v1 = edge.first.first;
        const auto& v2 = edge.first.second;
        const auto& snapshots = edge.second;
        for (int snapshot : snapshots) {
            output_buffer.emplace_back(to_string(v1) + " " + to_string(v2) + " " + to_string(snapshot));
            if (output_buffer.size() >= BUFFER_SIZE) {
                // 버퍼가 가득 찼으면 파일에 쓰기
                for (const auto& out_line : output_buffer) {
                    outFile << out_line << "\n";
                }
                output_buffer.clear();
            }
        }
    }

    // 남은 버퍼 내용 쓰기
    for (const auto& out_line : output_buffer) {
        outFile << out_line << "\n";
    }

    outFile.close();
    cout << "Filtered data has been written to: " << outFileName << endl;

    return 0;
}
