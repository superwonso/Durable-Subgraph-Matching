#include <bits/stdc++.h>
using namespace std;

struct TemporalEdge { int u, v, t; };
struct EdgeRecord { int u, v; vector<int> times; };

// 스냅샷을 연속 구간으로 묶어 문자열로 포맷
static string formatTimeIntervals(const vector<int>& times_in) {
    if (times_in.empty()) return "[]";
    vector<int> times = times_in;
    // 사전에 정렬/유니크가 되어 있다고 가정하지만 안전하게 다시 보장
    sort(times.begin(), times.end());
    times.erase(unique(times.begin(), times.end()), times.end());
    string out;
    int start = times[0], prev = times[0];
    for (size_t i = 1; i < times.size(); ++i) {
        if (times[i] == prev + 1) {
            prev = times[i];
        } else {
            if (!out.empty()) out += ", ";
            if (start == prev) out += to_string(start);
            else out += to_string(start) + "-" + to_string(prev);
            start = prev = times[i];
        }
    }
    if (!out.empty()) out += ", ";
    if (start == prev) out += to_string(start);
    else out += to_string(start) + "-" + to_string(prev);
    return "[" + out + "]";
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // CLI 옵션
    string input_path, query_path, output_path = "tdtree_output.txt";
    unsigned int seed = 42;
    int k = 3; // 후보 간선으로 인정할 최소 스냅샷 수
    int max_blocks = 1000000000;       // 기본 무제한 출력
    int max_edges_per_block = 1000000000;
    bool strict_root_filter = true;
    bool directed = false;

    for (int i = 1; i < argc; ++i) {
        string arg = argv[i];
        if ((arg == "-i" || arg == "--input") && i + 1 < argc) {
            input_path = argv[++i];
        } else if ((arg == "-q" || arg == "--query") && i + 1 < argc) {
            query_path = argv[++i];
        } else if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_path = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            seed = static_cast<unsigned int>(stoul(argv[++i]));
        } else if (arg == "-k" && i + 1 < argc) {
            k = stoi(argv[++i]); if (k < 1) k = 1;
        } else if (arg == "--max-blocks" && i + 1 < argc) {
            max_blocks = stoi(argv[++i]); if (max_blocks < 1) max_blocks = 1;
        } else if (arg == "--max-edges" && i + 1 < argc) {
            max_edges_per_block = stoi(argv[++i]); if (max_edges_per_block < 1) max_edges_per_block = 1;
        } else if (arg == "--strict-root-filter") {
            strict_root_filter = true;
        } else if (arg == "--directed") {
            directed = true;
        } else if (arg == "-h" || arg == "--help") {
            cerr <<
            "Usage: " << argv[0] << " -i edges.txt -q query.txt [-o result.txt]\n"
            "       [--seed 42] [-k 1] [--strict-root-filter] [--directed]\n"
            "       [--max-blocks N] [--max-edges M]\n";
            return 0;
        }
    }
    if (input_path.empty()) {
        cerr << "Error: input temporal edge file path required (-i)\n";
        return 1;
    }
    if (query_path.empty()) {
        cerr << "Error: query file path required (-q)\n";
        return 1;
    }

    // 1) 입력 temporal edges 읽기
    vector<TemporalEdge> edges;
    {
        ifstream fin(input_path);
        if (!fin) {
            cerr << "Error: cannot open input file: " << input_path << "\n";
            return 1;
        }
        long long U, V, T;
        while (fin >> U >> V >> T) {
            edges.push_back(TemporalEdge{(int)U, (int)V, (int)T});
        }
    }
    if (edges.empty()) {
        cerr << "Error: no edges read from " << input_path << "\n";
        return 1;
    }

    // 2) 데이터 정점 수집
    unordered_set<int> vertex_set;
    for (const auto& e : edges) {
        vertex_set.insert(e.u);
        vertex_set.insert(e.v);
    }
    vector<int> vertices(vertex_set.begin(), vertex_set.end());
    sort(vertices.begin(), vertices.end());

    // 3) 데이터 정점 레이블 무작위 배정 (A~E)
    vector<char> labelOptions = {'A','B','C','D','E'};
    unordered_map<int, char> dataLabel;
    mt19937 rng(seed);
    uniform_int_distribution<int> dist(0, (int)labelOptions.size() - 1);
    for (int vid : vertices) dataLabel[vid] = labelOptions[dist(rng)];

    // 4) 인접(유/무향) + 스냅샷 목록 구성
    // adj[u][v] = edge(u,v)의 모든 스냅샷 시점
    unordered_map<int, unordered_map<int, vector<int>>> adj;
    for (const auto& e : edges) {
        adj[e.u][e.v].push_back(e.t);
        if (!directed) adj[e.v][e.u].push_back(e.t);
    }
    for (auto& kv1 : adj) {
        for (auto& kv2 : kv1.second) {
            auto& tvec = kv2.second;
            sort(tvec.begin(), tvec.end());
            tvec.erase(unique(tvec.begin(), tvec.end()), tvec.end());
        }
    }

    // 5) 라벨별 데이터 정점 목록
    unordered_map<char, vector<int>> dataVerticesByLabel;
    for (const auto& kv : dataLabel) dataVerticesByLabel[kv.second].push_back(kv.first);
    for (auto& kv : dataVerticesByLabel) sort(kv.second.begin(), kv.second.end());

    // 6) 질의 그래프 읽기 (각 줄: "라벨 라벨")
    unordered_map<char, vector<char>> queryAdj;
    {
        ifstream fq(query_path);
        if (!fq) {
            cerr << "Error: cannot open query file: " << query_path << "\n";
            return 1;
        }
        string a, b;
        while (fq >> a >> b) {
            if (a.empty() || b.empty()) continue;
            char ca = a[0], cb = b[0];
            if (!(ca >= 'A' && ca <= 'Z') || !(cb >= 'A' && cb <= 'Z')) {
                // 알파벳 라벨만 허용
                continue;
            }
            queryAdj[ca].push_back(cb);
            queryAdj[cb].push_back(ca);
        }
    }
    if (queryAdj.empty()) {
        cerr << "Error: query graph is empty.\n";
        return 1;
    }

    // 7) 질의 정점 degree 계산(중복 제거)
    unordered_map<char, int> qDegree;
    for (const auto& kv : queryAdj) qDegree[kv.first] = 0;
    for (const auto& kv : queryAdj) {
        unordered_set<char> uniq(kv.second.begin(), kv.second.end());
        qDegree[kv.first] = (int)uniq.size();
    }

    // 8) Selectivity 기반 루트 라벨 선택: argmin ( |V(label)| / deg(label) )
    char rootLabel = 0;
    double bestSelect = numeric_limits<double>::infinity();
    int bestCand = INT_MAX;
    for (const auto& kv : qDegree) {
        char lbl = kv.first;
        int deg = kv.second;
        int candCount = dataVerticesByLabel.count(lbl) ? (int)dataVerticesByLabel[lbl].size() : 0;
        if (deg == 0 || candCount == 0) continue;
        double sel = double(candCount) / double(deg);
        if (sel < bestSelect || (abs(sel - bestSelect) < 1e-12 &&
             (candCount < bestCand || (candCount == bestCand && lbl < rootLabel)))) {
            bestSelect = sel; bestCand = candCount; rootLabel = lbl;
        }
    }
    if (rootLabel == 0) {
        cerr << "Error: could not select a root (no candidates for query labels).\n";
        return 1;
    }

    // 9) 루트의 직계 자식 라벨(1-hop)
    vector<char> children;
    {
        unordered_set<char> childs(queryAdj[rootLabel].begin(), queryAdj[rootLabel].end());
        for (char c : childs) children.push_back(c);
        sort(children.begin(), children.end());
    }

    // 10) 후보 집합 구성
    // 데이터 정점 degree/이웃라벨/정점 지속성 전처리 (strict 필터용)
    unordered_map<int, int> dataDeg;
    unordered_map<int, unordered_set<char>> dataNbrLabelSet;
    unordered_map<int, vector<int>> vertexTimes; // 정점이 등장한 모든 스냅샷(incident edge 기반)
    for (const auto& kv1 : adj) {
        int u = kv1.first;
        dataDeg[u] = (int)kv1.second.size();
        auto& s = dataNbrLabelSet[u];
        for (const auto& kv2 : kv1.second) {
            int v = kv2.first;
            s.insert(dataLabel[v]);
            // 정점 등장 시각 수집
            for (int t : kv2.second) vertexTimes[u].push_back(t);
        }
    }
    for (auto& kv : vertexTimes) {
        auto& vec = kv.second;
        sort(vec.begin(), vec.end());
        vec.erase(unique(vec.begin(), vec.end()), vec.end());
    }

    vector<int> rootCandidates;
    if (dataVerticesByLabel.count(rootLabel)) rootCandidates = dataVerticesByLabel[rootLabel];
    sort(rootCandidates.begin(), rootCandidates.end());

    if (strict_root_filter) {
        // 논문식 루트 후보 필터(간단 구현): deg(v) >= deg_Q(root), 이웃라벨수 >= |N_Q(root)|, 정점 지속성 >= k
        int qDegRoot = qDegree[rootLabel];
        unordered_set<char> qNbrLabSet(queryAdj[rootLabel].begin(), queryAdj[rootLabel].end());
        int qNbrLabelCnt = (int)qNbrLabSet.size();
        vector<int> filtered; filtered.reserve(rootCandidates.size());
        for (int v : rootCandidates) {
            int degv = dataDeg.count(v) ? dataDeg[v] : 0;
            int lblcnt = dataNbrLabelSet.count(v) ? (int)dataNbrLabelSet[v].size() : 0;
            int vdur = vertexTimes.count(v) ? (int)vertexTimes[v].size() : 0;
            if (degv >= qDegRoot && lblcnt >= qNbrLabelCnt && vdur >= k) {
                filtered.push_back(v);
            }
        }
        rootCandidates.swap(filtered);
    }

    unordered_map<char, vector<int>> childCandidates;
    for (char cl : children) {
        if (dataVerticesByLabel.count(cl)) childCandidates[cl] = dataVerticesByLabel[cl];
        else childCandidates[cl] = {};
        sort(childCandidates[cl].begin(), childCandidates[cl].end());
    }

    // 11) 루트 후보(parent)별, 자식 라벨 후보(child)로 연결되는 간선(스냅샷 수 >= k) 수집
    // blocksPerChild[childLabel] = vector of (parentDataVertex, list of edges parent->childCandidate with times)
    unordered_map<char, vector<pair<int, vector<EdgeRecord>>>> blocksPerChild;
    for (char cl : children) {
        auto& blocks = blocksPerChild[cl];
        for (int parent : rootCandidates) {
            vector<EdgeRecord> recs;
            auto it = adj.find(parent);
            if (it != adj.end()) {
                for (const auto& kv : it->second) {
                    int neigh = kv.first;
                    const auto& times = kv.second;
                    if (dataLabel[neigh] == cl && (int)times.size() >= k) {
                        recs.push_back(EdgeRecord{parent, neigh, times});
                    }
                }
            }
            if (!recs.empty()) {
                sort(recs.begin(), recs.end(), [](const EdgeRecord& a, const EdgeRecord& b){
                    if (a.u != b.u) return a.u < b.u;
                    return a.v < b.v;
                });
                blocks.push_back({parent, move(recs)});
            }
        }
    }

    // ===== (추가) 후보 개수 집계 =====
    // TD-tree 정의에 따른 후보 개수:
    //  - 루트: |Vcand(root)| = rootCandidates.size()
    //  - 비루트(자식 라벨 cl): cand_cnt(cl) = Σ_b |b.Vcand| (= 각 블록의 recs.size() 합)
    unordered_map<char, size_t> tdCandCountByChild;      // Σ_b |b.Vcand|
    unordered_map<char, size_t> uniqueChildCountByChild; // 유일 자식 후보 정점 수 (진단용)
    size_t totalEdgePairsAll = 0;                         // 모든 자식에 대한 Σ_b |b.Vcand| 합

    for (char cl : children) {
        const auto& blocks = blocksPerChild[cl];
        size_t totalEdges = 0;
        unordered_set<int> uniqChilds;
        for (const auto& blk : blocks) {
            totalEdges += blk.second.size();             // recs.size() = |b.Vcand|
            for (const auto& rec : blk.second) uniqChilds.insert(rec.v);
        }
        tdCandCountByChild[cl] = totalEdges;
        uniqueChildCountByChild[cl] = uniqChilds.size();
        totalEdgePairsAll += totalEdges;
    }
    // ===============================

    // 12) 결과 파일 저장
    ofstream fout(output_path);
    if (!fout) {
        cerr << "Error: cannot open output file: " << output_path << "\n";
        return 1;
    }

    // 헤더
    auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    fout << "=== Partial TD-tree (root + 1-hop children) ===\n";
    fout << "Input: " << input_path << "\n";
    fout << "Query: " << query_path << "\n";
    fout << "Seed: " << seed << "\n";
    fout << "Directed: " << (directed ? "true" : "false") << "\n";
    fout << "k (min snapshots per edge): " << k << "\n";
    fout << "Strict root filter: " << (strict_root_filter ? "true" : "false") << "\n";
    fout << "Generated at: " << std::put_time(std::localtime(&now), "%F %T") << "\n\n";

    fout << "Selected root query-vertex label: " << rootLabel
         << "  (selectivity = " << fixed << setprecision(3) << bestSelect
         << " = |V(" << rootLabel << ")| / deg(" << rootLabel << "))\n";
    fout.unsetf(std::ios::floatfield);

    // 라벨 분포
    vector<char> labs = {'A','B','C','D','E'};
    fout << "Data vertices per label: ";
    for (size_t i = 0; i < labs.size(); ++i) {
        int c = dataVerticesByLabel.count(labs[i]) ? (int)dataVerticesByLabel[labs[i]].size() : 0;
        fout << labs[i] << ":" << c << (i + 1 < labs.size() ? " " : "");
    }
    fout << "\n\n";

    // 루트 후보 및 자식 요약
    fout << "Root node [" << rootLabel << "] candidates (|Vcand(root)|): "
         << rootCandidates.size() << " vertices\n";
    if (!children.empty()) {
        fout << "Root's children in query tree: ";
        for (size_t i = 0; i < children.size(); ++i) {
            if (i) fout << ", ";
            fout << children[i] << " (deg=" << qDegree[children[i]]
                 << ", |V(label)|=" << (dataVerticesByLabel.count(children[i]) ? (int)dataVerticesByLabel[children[i]].size() : 0) << ")";
        }
        fout << "\n";
    } else {
        fout << "(Root has no neighbors in the query graph)\n";
    }

    // 자식별 블록/간선 출력(컷오프 적용) + TD-tree 후보 개수 병기
    for (char cl : children) {
        const auto& blocks = blocksPerChild[cl];
        size_t totalEdges = tdCandCountByChild[cl]; // = Σ_b |b.Vcand|

        fout << "\n-- Child node [" << cl << "] --\n";
        fout << "Candidate data vertices with label " << cl << ": " << childCandidates[cl].size() << "\n";
        fout << "Blocks (root-candidates that connect to label " << cl << "): " << blocks.size() << "\n";
        fout << "Total candidate edges (Σ_b |b.Vcand|) = " << totalEdges << "  <-- TD-tree 후보 개수\n";
        fout << "Unique child candidates connected to some root = " << uniqueChildCountByChild[cl] << "\n";

        int printedBlocks = 0;
        for (const auto& blk : blocks) {
            if (printedBlocks >= max_blocks) {
                fout << "  ... (" << (blocks.size() - printedBlocks) << " more blocks not shown)\n";
                break;
            }
            int parent = blk.first;
            const auto& recs = blk.second;
            fout << "  [Parent data vertex " << parent << "] edges: " << recs.size() << "\n";
            int printedEdges = 0;
            for (const auto& rec : recs) {
                if (printedEdges >= max_edges_per_block) {
                    fout << "    ... (" << (recs.size() - printedEdges) << " more edges not shown in this block)\n";
                    break;
                }
                fout << "    (" << rec.u << " -> " << rec.v << ")  times=" << formatTimeIntervals(rec.times) << "\n";
                printedEdges++;
            }
            printedBlocks++;
        }
    }

    // === 글로벌 요약: 모든 자식에 대한 TD-tree 후보 개수 합계 등 ===
    fout << "\n== Candidate summary (TD-tree definition) ==\n";
    fout << "Root |Vcand(root)| = " << rootCandidates.size() << "\n";
    size_t sumUniqueChildren = 0;
    for (char cl : children) {
        fout << "Child [" << cl << "]  Σ_b |b.Vcand| = " << tdCandCountByChild[cl]
             << " ,  unique child candidates = " << uniqueChildCountByChild[cl] << "\n";
        sumUniqueChildren += uniqueChildCountByChild[cl];
    }
    fout << "Global  Σ_b |b.Vcand| over all children = " << totalEdgePairsAll << "\n";
    fout << "Global  sum of unique child candidates    = " << sumUniqueChildren << "\n";

    fout << "\nDone.\n";
    fout.close();

    // 콘솔에는 파일 경로만 간단 안내
    cout << "Results saved to: " << output_path << "\n";
    return 0;
}
