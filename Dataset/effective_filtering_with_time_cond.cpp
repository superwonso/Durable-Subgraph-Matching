#include <iostream>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <utility>
#include <algorithm>
#include <chrono>

using namespace std;

// 향상된 pair 해시 함수 (Boost의 hash_combine 유사 방식)
struct pair_hash {
    size_t operator()(const pair<int, int>& p) const {
        size_t seed = hash<int>()(p.first);
        seed ^= hash<int>()(p.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        return seed;
    }
};

// 엣지를 저장할 타입 정의
using EdgeKey = pair<int, int>;
using Edges = unordered_map<EdgeKey, vector<int>, pair_hash>;
using FilteredEdges = unordered_map<EdgeKey, vector<int>, pair_hash>;

int main(int argc, char* argv[]) {
    // 프로그램 사용법 검사
    if (argc != 2) {
        cerr << "Usage: " << argv[0] << " <input_file>" << endl;
        return 1;
    }

    // 입력 파일 열기
    ifstream inFile(argv[1], ios::in | ios::binary);
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
    ofstream outFile(outFileName, ios::out | ios::binary);
    if (!outFile) {
        cerr << "Error: Cannot create output file " << outFileName << endl;
        return 1;
    }

    // 엣지 정보를 저장할 해시맵과 스냅샷을 저장할 벡터
    Edges edges;
    edges.reserve(850000); // 예상 엣지 수에 따라 조정

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
            edges[{v1, v2}].emplace_back(snapshot);
        } else {
            invalid_lines++;
        }
    }
    inFile.close();

    if (invalid_lines > 0) {
        cerr << "Warning: " << invalid_lines << " invalid lines were skipped." << endl;
    }

    // 필터링된 엣지를 저장할 맵
    FilteredEdges filtered_edges;
    filtered_edges.reserve(edges.size()); // 예상 크기에 따라 조정 가능

    // 처리 시작 시간 기록
    chrono::steady_clock::time_point filter_start = chrono::steady_clock::now();

    // 엣지 필터링: 단 한 번이라도 연속된 스냅샷을 가진 엣지만 유지
    for (auto& edge : edges) {
        auto& snapshot_vector = edge.second;

        if (snapshot_vector.size() < 2) {
            // 스냅샷이 하나뿐이라면 연속된 스냅샷이 없으므로 스킵
            continue;
        }

        // 스냅샷을 정렬 및 중복 제거
        sort(snapshot_vector.begin(), snapshot_vector.end());
        snapshot_vector.erase(unique(snapshot_vector.begin(), snapshot_vector.end()), snapshot_vector.end());

        // 연속된 스냅샷이 존재하는지 확인
        bool has_consecutive = false;
        for (size_t i = 1; i < snapshot_vector.size(); ++i) {
            if (snapshot_vector[i] == snapshot_vector[i - 1] + 1) {
                has_consecutive = true;
                break;
            }
        }

        if (has_consecutive) {
            // 필터링된 엣지에 추가 (이동 시멘틱스 사용)
            filtered_edges.emplace(edge.first, move(snapshot_vector));
        }
    }

    // 처리 종료 시간 기록
    chrono::steady_clock::time_point filter_end = chrono::steady_clock::now();
    chrono::milliseconds filter_millisec = chrono::duration_cast<chrono::milliseconds>(filter_end - filter_start);
    cout << "Time consumed (filtering): " << filter_millisec.count() << " ms" << endl;
    cout << "Total original edges: " << edges.size() << endl;
    cout << "Total filtered edges: " << filtered_edges.size() << endl;

    // 필터링된 엣지를 출력 파일에 쓰기
    // I/O 성능 최적화를 위해 별도의 시간 측정
    chrono::steady_clock::time_point io_start = chrono::steady_clock::now();

    // 효율적인 I/O를 위해 버퍼를 사용
    const size_t BUFFER_SIZE = 100000;
    vector<string> output_buffer;
    output_buffer.reserve(BUFFER_SIZE);

    for (const auto& edge : filtered_edges) {
        const auto& v1 = edge.first.first;
        const auto& v2 = edge.first.second;
        const auto& snapshots = edge.second;
        for (int snapshot : snapshots) {
            // 문자열을 미리 생성하여 버퍼에 추가
            output_buffer.emplace_back(to_string(v1) + " " + to_string(v2) + " " + to_string(snapshot));
            if (output_buffer.size() >= BUFFER_SIZE) {
                // 버퍼가 가득 찼으면 파일에 쓰기
                string bulk_output;
                bulk_output.reserve(BUFFER_SIZE * 20); // 예상 한 줄의 평균 길이에 따라 조정
                for (const auto& out_line : output_buffer) {
                    bulk_output += out_line;
                    bulk_output += '\n';
                }
                outFile.write(bulk_output.data(), bulk_output.size());
                output_buffer.clear();
            }
        }
    }

    // 남은 버퍼 내용 쓰기
    if (!output_buffer.empty()) {
        string bulk_output;
        bulk_output.reserve(output_buffer.size() * 20);
        for (const auto& out_line : output_buffer) {
            bulk_output += out_line;
            bulk_output += '\n';
        }
        outFile.write(bulk_output.data(), bulk_output.size());
    }

    // 출력 종료 시간 기록
    chrono::steady_clock::time_point io_end = chrono::steady_clock::now();
    chrono::milliseconds io_millisec = chrono::duration_cast<chrono::milliseconds>(io_end - io_start);
    cout << "Time consumed (I/O): " << io_millisec.count() << " ms" << endl;

    outFile.close();
    cout << "Filtered data has been written to: " << outFileName << endl;

    return 0;
}
