#include <bits/stdc++.h>
using namespace std;

int main(int argc, char* argv[]){
    // 명령줄 인수로부터 입력 및 출력 파일 이름을 받습니다.
    if(argc != 3){
        cerr << "사용법: " << argv[0] << " <입력파일> <출력파일>\n";
        return 1;
    }

    string input_file = argv[1];
    string output_file = argv[2];
    
    // 입력 파일 스트림 열기
    ifstream infile(input_file);
    if(!infile.is_open()){
        cerr << "입력 파일을 열 수 없습니다: " << input_file << endl;
        return 1;
    }
    
    // (v1, v2) 쌍을 키로 하고, 해당 키에 대한 snapshot_num 리스트를 값으로 가지는 맵
    map<pair<int, int>, vector<int>> data;
    
    string line;
    // 입력 파일의 모든 줄을 처리
    while(getline(infile, line)){
        // 빈 줄은 건너뜁니다.
        if(line.empty()) continue;
        
        // 각 줄을 파싱하기 위한 스트림
        stringstream ss(line);
        int v1, v2, snapshot_num;
        
        // v1, v2, snapshot_num을 읽어옵니다.
        if(!(ss >> v1 >> v2 >> snapshot_num)){
            // 만약 형식이 맞지 않으면 건너뜁니다.
            continue;
        }
        
        // (v1, v2) 키에 snapshot_num을 추가합니다.
        data[{v1, v2}].push_back(snapshot_num);
    }
    
    infile.close(); // 입력 파일 닫기
    
    // 출력 파일 스트림 열기
    ofstream outfile(output_file);
    if(!outfile.is_open()){
        cerr << "출력 파일을 열 수 없습니다: " << output_file << endl;
        return 1;
    }
    
    // 맵을 순회하며 각 (v1, v2) 쌍에 대한 snapshot_num 리스트를 출력합니다.
    for(auto &[key, snapshots] : data){
        int v1 = key.first;
        int v2 = key.second;
        
        // snapshot_num 리스트를 정렬합니다.
        vector<int> sorted_snapshots = snapshots;
        sort(sorted_snapshots.begin(), sorted_snapshots.end());
        
        // sorted_snapshots.erase(unique(sorted_snapshots.begin(), sorted_snapshots.end()), sorted_snapshots.end());
        
        // snapshot_num 리스트를 콤마로 구분된 문자열으로 변환합니다.
        string s;
        for(int i = 0; i < sorted_snapshots.size(); ++i){
            if(i > 0) s += ",";
            s += to_string(sorted_snapshots[i]);
        }
        
        // 결과를 출력 파일에 씁니다.
        outfile << v1 << " " << v2 << " " << s << "\n";
    }
    
    outfile.close(); // 출력 파일 닫기
    
    return 0;
}
