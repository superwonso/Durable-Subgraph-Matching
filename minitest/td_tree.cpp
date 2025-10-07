#include <bits/stdc++.h>
using namespace std;

// ============ Bloom Filter (double hashing) ============
struct Bloom {
    size_t m_bits = 0;    // number of bits
    int k_hash = 3;       // number of hash functions
    uint64_t seed1 = 0x9e3779b97f4a7c15ULL, seed2 = 0xda942042e4dd58b5ULL;
    vector<uint64_t> bits; // bit array in 64-bit words

    static uint64_t splitmix64(uint64_t x) {
        x += 0x9e3779b97f4a7c15ULL;
        x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
        x = (x ^ (x >> 27)) * 0x94d049bb133111eb;
        return x ^ (x >> 31);
    }
    void init(size_t m, int k, uint64_t s1, uint64_t s2) {
        m_bits = max<size_t>(m, 1024);
        k_hash = max(1, k);
        seed1 = s1; seed2 = s2;
        bits.assign((m_bits + 63) / 64, 0ULL);
    }
    inline void setbit(size_t pos) { bits[pos >> 6] |= (1ULL << (pos & 63)); }
    inline bool testbit(size_t pos) const { return (bits[pos >> 6] >> (pos & 63)) & 1ULL; }
    inline size_t idx(uint64_t x, int i) const {
        uint64_t h1 = splitmix64(x ^ seed1);
        uint64_t h2 = splitmix64(x ^ seed2) | 1ULL;
        return (size_t)((h1 + i * h2) % m_bits);
    }
    void add(uint64_t x) { for (int i = 0; i < k_hash; ++i) setbit(idx(x, i)); }
    bool possiblyContains(uint64_t x) const {
        for (int i = 0; i < k_hash; ++i) if (!testbit(idx(x, i))) return false;
        return true; // maybe
    }
};

// ============ Temporal / TD-tree data structures ============
struct TemporalEdge { int u, v, t; };
struct Cell { int v; vector<int> ts; };
struct Block { int vpar; vector<Cell> vcand; };

struct TDNode {
    char qlabel = 0;
    char parent = 0;
    vector<char> children;
    bool isRoot = false;
    bool isLeaf = false;

    vector<int> rootVcand;                // root only
    vector<Block> blocks;                 // non-root only
    unordered_map<int,size_t> blockIndex; // vpar -> block idx

    // membership for non-tree verification
    Bloom bloom;
    unordered_set<int> memberSet;

    void rebuildIndex() {
        blockIndex.clear();
        for (size_t i = 0; i < blocks.size(); ++i) blockIndex[blocks[i].vpar] = i;
    }
    void rebuildMemberSet() {
        memberSet.clear();
        if (isRoot) memberSet.insert(rootVcand.begin(), rootVcand.end());
        else for (auto &b : blocks) for (auto &c : b.vcand) memberSet.insert(c.v);
    }
    void rebuildBloom(size_t m_bits, int k_hash, uint64_t s1, uint64_t s2) {
        bloom.init(m_bits, k_hash, s1, s2);
        if (isRoot) for (int v : rootVcand) bloom.add((uint64_t)v);
        else for (auto &b : blocks) for (auto &c : b.vcand) bloom.add((uint64_t)c.v);
    }
};

// ============ Utils ============
static vector<int> intersect_sorted(const vector<int>& a, const vector<int>& b) {
    vector<int> out; out.reserve(min(a.size(), b.size()));
    size_t i=0, j=0;
    while (i<a.size() && j<b.size()) {
        if (a[i] < b[j]) ++i;
        else if (b[j] < a[i]) ++j;
        else { out.push_back(a[i]); ++i; ++j; }
    }
    return out;
}
static string formatTimeIntervals(const vector<int>& times_in) {
    if (times_in.empty()) return "[]";
    vector<int> t = times_in;
    sort(t.begin(), t.end());
    t.erase(unique(t.begin(), t.end()), t.end());
    string out;
    int start=t[0], prev=t[0];
    for (size_t i=1;i<t.size();++i){
        if (t[i]==prev+1){ prev=t[i]; }
        else {
            if (!out.empty()) out += ", ";
            if (start==prev) out += to_string(start);
            else out += to_string(start) + "-" + to_string(prev);
            start = prev = t[i];
        }
    }
    if (!out.empty()) out += ", ";
    if (start==prev) out += to_string(start);
    else out += to_string(start) + "-" + to_string(prev);
    return "[" + out + "]";
}

int main(int argc, char** argv) {
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // ===== CLI =====
    string input_path, query_path, output_path = "tdtree_output.txt";
    unsigned int seed = 42;
    int k = 3;                 // 최소 지속 스냅샷 수
    bool directed = true;     // 기본 유향
    bool strict_root_filter = true;
    bool trim_rebuild_bloom = false; // [변경 2] 기본: 트리밍 중 Bloom 재구성 안 함

    for (int i=1;i<argc;++i){
        string arg = argv[i];
        if ((arg=="-i"||arg=="--input") && i+1<argc) input_path=argv[++i];
        else if ((arg=="-q"||arg=="--query") && i+1<argc) query_path=argv[++i];
        else if ((arg=="-o"||arg=="--output") && i+1<argc) output_path=argv[++i];
        else if (arg=="--seed" && i+1<argc) seed=(unsigned)stoul(argv[++i]);
        else if (arg=="-k" && i+1<argc) { k=stoi(argv[++i]); if (k<1) k=1; }
        else if (arg=="--undirected") directed=false;
        else if (arg=="--no-strict-root-filter") strict_root_filter=false;
        else if (arg=="--trim-rebuild-bloom") trim_rebuild_bloom=true;
        else if (arg=="-h"||arg=="--help"){
            cerr<<"Usage: "<<argv[0]<<" -i edges.txt -q query.txt [-o out.txt]\n"
                <<"  [-k 3] [--undirected] [--no-strict-root-filter] [--trim-rebuild-bloom]\n";
            return 0;
        }
    }
    if (input_path.empty() || query_path.empty()) { cerr<<"need -i and -q\n"; return 1; }

    // ===== 타이밍 시작 =====
    using clk = chrono::high_resolution_clock;
    auto T0 = clk::now();

    // ===== 입력 읽기 =====
    struct Edge{int u,v,t;};
    vector<Edge> edges;
    {
        ifstream fin(input_path);
        if (!fin){ cerr<<"cannot open input "<<input_path<<"\n"; return 1; }
        long long U,V,T;
        while (fin>>U>>V>>T) edges.push_back({(int)U,(int)V,(int)T});
    }
    if (edges.empty()){ cerr<<"no edges\n"; return 1; }

    // 정점/전체 시각
    unordered_set<int> vset;
    vector<int> allTimesVec;
    for (auto &e: edges){ vset.insert(e.u); vset.insert(e.v); allTimesVec.push_back(e.t); }
    sort(allTimesVec.begin(), allTimesVec.end());
    allTimesVec.erase(unique(allTimesVec.begin(), allTimesVec.end()), allTimesVec.end());

    vector<int> vertices(vset.begin(), vset.end());
    sort(vertices.begin(), vertices.end());

    // 데이터 정점 라벨 무작위(A~E)
    vector<char> labelOptions = {'A','B','C','D','E'};
    mt19937 rng(seed);
    uniform_int_distribution<int> dist(0,(int)labelOptions.size()-1);
    unordered_map<int,char> dataLabel;
    for (int vid : vertices) dataLabel[vid] = labelOptions[dist(rng)];

    // 인접/시간집합
    unordered_map<int, unordered_map<int, vector<int>>> adj;
    for (auto &e: edges){
        adj[e.u][e.v].push_back(e.t);
        if (!directed) adj[e.v][e.u].push_back(e.t);
    }
    for (auto &kv1 : adj){
        for (auto &kv2 : kv1.second){
            auto &tv = kv2.second;
            sort(tv.begin(), tv.end());
            tv.erase(unique(tv.begin(), tv.end()), tv.end());
        }
    }

    // [변경 1] 라벨별 이웃 인덱스 구축
    unordered_map<int, array<vector<int>, 26>> nbrByLabel;
    nbrByLabel.reserve(adj.size()*2);
    for (auto &kv1 : adj){
        int u = kv1.first;
        auto &arr = nbrByLabel[u];
        for (auto &kv2 : kv1.second){
            int v = kv2.first;
            char lab = dataLabel[v];
            int idx = (lab>='A' && lab<='Z') ? (lab-'A') : 0;
            arr[idx].push_back(v);
        }
        for (int i=0;i<26;++i){
            auto &vec = arr[i];
            if (!vec.empty()){
                sort(vec.begin(), vec.end());
                vec.erase(unique(vec.begin(), vec.end()), vec.end());
            }
        }
    }

    // 정점 지속시간 dur(v)
    unordered_map<int, vector<int>> vertexTimes;
    for (auto &e: edges){ vertexTimes[e.u].push_back(e.t); vertexTimes[e.v].push_back(e.t); }
    for (auto &kv: vertexTimes){
        auto &tv = kv.second; sort(tv.begin(), tv.end());
        tv.erase(unique(tv.begin(), tv.end()), tv.end());
    }

    // degree / neighbor label set
    unordered_map<int,int> dataDeg;
    unordered_map<int, unordered_set<char>> nbrLblSet;
    for (auto &kv1 : adj){
        int u = kv1.first;
        dataDeg[u] = (int)kv1.second.size();
        for (auto &kv2 : kv1.second) nbrLblSet[u].insert(dataLabel[kv2.first]);
    }

    // 질의 읽기(라벨-라벨)
    unordered_map<char, vector<char>> qAdj;
    vector<pair<char,char>> qEdges;
    {
        ifstream fq(query_path);
        if (!fq){ cerr<<"cannot open query "<<query_path<<"\n"; return 1; }
        string a,b;
        while (fq>>a>>b){
            if (a.empty()||b.empty()) continue;
            char A=a[0], B=b[0];
            if (!(A>='A'&&A<='Z'&&B>='A'&&B<='Z')) continue;
            qAdj[A].push_back(B); qAdj[B].push_back(A);
            qEdges.push_back({A,B});
        }
    }
    if (qAdj.empty()){ cerr<<"empty query\n"; return 1; }

    // 질의 degree
    unordered_map<char,int> qDeg;
    for (auto &kv : qAdj){
        unordered_set<char> s(kv.second.begin(), kv.second.end());
        qDeg[kv.first] = (int)s.size();
    }

    // 라벨별 데이터 후보
    unordered_map<char, vector<int>> dataByLabel;
    for (auto &kv : dataLabel) dataByLabel[kv.second].push_back(kv.first);
    for (auto &kv : dataByLabel) sort(kv.second.begin(), kv.second.end());

    auto T_pre = clk::now(); // Preprocess 완료

    // ===== 루트 선택 (|V(label)| / deg) =====
    char rootLabel = 0; double bestSel = numeric_limits<double>::infinity(); int bestCand=INT_MAX;
    for (auto &kv : qDeg){
        char L = kv.first; int d = kv.second;
        int cand = dataByLabel.count(L)?(int)dataByLabel[L].size():0;
        if (d==0 || cand==0) continue;
        double s = double(cand)/double(d);
        if (s<bestSel || (fabs(s-bestSel)<1e-12 && (cand<bestCand || (cand==bestCand && L<rootLabel)))){
            bestSel=s; bestCand=cand; rootLabel=L;
        }
    }
    if (rootLabel==0){ cerr<<"cannot choose root\n"; return 1; }

    auto T_root = clk::now(); // Root selection 완료

    // ===== DFS 트리 구성 =====
    unordered_map<char,char> parent; parent[rootLabel]=0;
    unordered_map<char, vector<char>> treeChildren;
    unordered_set<long long> treeEdgeSet;
    auto selVal=[&](char L)->double{
        int d=qDeg[L]; int c=dataByLabel.count(L)?(int)dataByLabel[L].size():0;
        if (d==0||c==0) return 1e100; return double(c)/double(d);
    };
    unordered_set<char> visited;
    function<void(char)> dfs = [&](char u){
        visited.insert(u);
        vector<char> nbrs = qAdj[u];
        vector<char> cand;
        for (char w : nbrs) if (!visited.count(w)) cand.push_back(w);
        sort(cand.begin(), cand.end(), [&](char a,char b){
            double sa=selVal(a), sb=selVal(b);
            if (sa!=sb) return sa<sb; return a<b;
        });
        for (char w : cand){
            parent[w]=u;
            treeChildren[u].push_back(w);
            long long key = ((long long)min(u,w)<<8) | (long long)max(u,w);
            treeEdgeSet.insert(key);
            dfs(w);
        }
    };
    dfs(rootLabel);

    // 비트리 간선
    vector<pair<char,char>> nonTreeEdges;
    auto isTreeEdge=[&](char a,char b){
        long long key = ((long long)min(a,b)<<8) | (long long)max(a,b);
        return treeEdgeSet.count(key)>0;
    };
    auto isAncestor=[&](char anc,char x){
        while (x!=0){ if (x==anc) return true; x=parent[x]; }
        return false;
    };
    for (auto &e : qEdges){
        char a=e.first,b=e.second;
        if (!isTreeEdge(a,b)) nonTreeEdges.push_back({a,b});
    }

    // ===== TD-node 구성 =====
    unordered_map<char, TDNode> TD;
    function<void(char)> buildNodes = [&](char u){
        TD[u].qlabel=u; TD[u].parent=parent[u];
        TD[u].children=treeChildren[u];
        TD[u].isRoot=(u==rootLabel);
        TD[u].isLeaf=TD[u].children.empty();
        for (char c : TD[u].children) buildNodes(c);
    };
    buildNodes(rootLabel);

    // Bloom 초기화
    size_t m_bits_nodes = max<size_t>(8192, vertices.size()*8);
    for (auto &kv : TD){
        char lab = kv.first;
        TD[lab].bloom.init(m_bits_nodes, 3, ((uint64_t)seed<<1) ^ lab, ((uint64_t)seed<<2) ^ (lab*131ULL));
    }

    // 루트 후보 필터
    vector<int> rootCand;
    if (dataByLabel.count(rootLabel)) rootCand = dataByLabel[rootLabel];
    auto uniqNbrQ=[&](char L){ unordered_set<char> s(qAdj[L].begin(), qAdj[L].end()); return (int)s.size(); };
    if (strict_root_filter){
        vector<int> filt; filt.reserve(rootCand.size());
        for (int v : rootCand){
            int degv = dataDeg.count(v)?dataDeg[v]:0;
            int lblc = nbrLblSet.count(v)?(int)nbrLblSet[v].size():0;
            int dur  = vertexTimes.count(v)?(int)vertexTimes[v].size():0;
            if (degv>=qDeg[rootLabel] && lblc>=uniqNbrQ(rootLabel) && dur>=k) filt.push_back(v);
        }
        rootCand.swap(filt);
    }
    sort(rootCand.begin(), rootCand.end());
    TD[rootLabel].rootVcand = rootCand;
    TD[rootLabel].rebuildMemberSet();
    for (int v : rootCand) TD[rootLabel].bloom.add((uint64_t)v);

    // 조상 맵
    unordered_map<char, vector<char>> nonTreeAnc;
    for (auto &e : nonTreeEdges){
        char a=e.first, b=e.second;
        if (isAncestor(a,b)) nonTreeAnc[b].push_back(a);
        else if (isAncestor(b,a)) nonTreeAnc[a].push_back(b);
        else { nonTreeAnc[b].push_back(a); nonTreeAnc[a].push_back(b); }
    }

    auto existsEdgeToNodeCandidates = [&](int v0, char ancLabel)->bool{
        auto it = adj.find(v0); if (it==adj.end()) return false;
        TDNode &A = TD[ancLabel];
        for (auto &kv : it->second){
            int w = kv.first;
            if (!A.bloom.possiblyContains((uint64_t)w)) continue;
            if (!A.memberSet.count(w)) continue;
            return true;
        }
        return false;
    };

    // ===== Stage-1 성장 =====
    auto T_grow_start = clk::now();

    function<void(char,int,const vector<int>&)> fill_node =
        [&](char c, int vpar, const vector<int>& TS_in){
            TDNode &node = TD[c];
            Block blk; blk.vpar = vpar;

            auto itNei = adj.find(vpar);
            if (itNei != std::end(adj)) {
                // [변경 1] 라벨별 이웃만 순회
                int cidx = (c>='A' && c<='Z') ? (c-'A') : 0;
                auto itArr = nbrByLabel.find(vpar);
                if (itArr != nbrByLabel.end()){
                    const auto &candNbrs = itArr->second[cidx];
                    for (int w : candNbrs){
                        auto itEdge = itNei->second.find(w);
                        if (itEdge == itNei->second.end()) continue;
                        const auto &edgeTS = itEdge->second;
                        if ((int)edgeTS.size() < k) continue;
                        int degw = dataDeg.count(w)?dataDeg[w]:0;
                        int lblcw = nbrLblSet.count(w)?(int)nbrLblSet[w].size():0;
                        if (degw < qDeg[c]) continue;
                        if (lblcw < uniqNbrQ(c)) continue;

                        bool okNT = true;
                        for (char anc : nonTreeAnc[c]) {
                            if (!existsEdgeToNodeCandidates(w, anc)){ okNT=false; break; }
                        }
                        if (!okNT) continue;

                        vector<int> TSnew = TS_in.empty() ? edgeTS : intersect_sorted(TS_in, edgeTS);
                        if ((int)TSnew.size() < k) continue;

                        Cell cell; cell.v = w;
                        if (node.isLeaf) cell.ts = TSnew;
                        blk.vcand.push_back(std::move(cell));

                        TD[c].memberSet.insert(w);
                        TD[c].bloom.add((uint64_t)w);
                    }
                }
            }
            if (!blk.vcand.empty()) {
                TD[c].blocks.push_back(std::move(blk));
                if (!node.isLeaf) {
                    const Block &last = TD[c].blocks.back();
                    for (const auto &cell : last.vcand) {
                        auto itEdge = adj[vpar].find(cell.v);
                        if (itEdge == adj[vpar].end()) continue;
                        const vector<int> &TSedge = itEdge->second;
                        vector<int> TSnew = TS_in.empty() ? TSedge : intersect_sorted(TS_in, TSedge);
                        if ((int)TSnew.size() < k) continue;
                        for (char cc : node.children) fill_node(cc, cell.v, TSnew);
                    }
                }
            }
        };

    for (char c : TD[rootLabel].children)
        for (int vpar : TD[rootLabel].rootVcand)
            fill_node(c, vpar, allTimesVec);

    auto T_grow_end = clk::now(); // Stage-1 완료

    // ===== Stage-2 트리밍 =====
    auto T_trim_start = clk::now();

    function<void(char,int)> removeBlocksByParent;
    function<void(char,int)> removeCandidateCascade;

    removeBlocksByParent = [&](char nodeLabel, int vpar){
        TDNode &node = TD[nodeLabel];
        vector<Block> kept; kept.reserve(node.blocks.size());
        for (auto &b : node.blocks){
            if (b.vpar == vpar){
                for (const auto &cell : b.vcand)
                    for (char cc : node.children) removeBlocksByParent(cc, cell.v);
            } else kept.push_back(std::move(b));
        }
        node.blocks.swap(kept);
        // [변경 2] 트리밍 중 Bloom/멤버 재구성 생략(옵션으로만)
        if (trim_rebuild_bloom) node.rebuildBloom(m_bits_nodes, 3, ((uint64_t)seed<<1) ^ nodeLabel, ((uint64_t)seed<<2) ^ (nodeLabel*131ULL));
        node.rebuildIndex();
    };

    removeCandidateCascade = [&](char uLabel, int v){
        TDNode &u = TD[uLabel];
        if (u.isRoot){
            vector<int> keep;
            for (int r : u.rootVcand) if (r != v) keep.push_back(r);
            u.rootVcand.swap(keep);
            if (trim_rebuild_bloom) u.rebuildBloom(m_bits_nodes, 3, ((uint64_t)seed<<1) ^ uLabel, ((uint64_t)seed<<2) ^ (uLabel*131ULL));
            for (char c : u.children) removeBlocksByParent(c, v);
        } else {
            for (auto &b : u.blocks){
                vector<Cell> keep;
                for (auto &c : b.vcand) if (c.v != v) keep.push_back(std::move(c));
                b.vcand.swap(keep);
            }
            { vector<Block> keep; for (auto &b : u.blocks) if (!b.vcand.empty()) keep.push_back(std::move(b)); u.blocks.swap(keep); }
            for (char c : u.children) removeBlocksByParent(c, v);
            if (trim_rebuild_bloom) u.rebuildBloom(m_bits_nodes, 3, ((uint64_t)seed<<1) ^ uLabel, ((uint64_t)seed<<2) ^ (uLabel*131ULL));
        }
        u.rebuildIndex();
    };

    vector<char> preorder;
    function<void(char)> pre=[&](char u){ preorder.push_back(u); for (char c : TD[u].children) pre(c); };
    pre(rootLabel);

    bool changed = true;
    while (changed){
        changed=false;
        unordered_map<char, unordered_map<int,bool>> childHasBlock;
        for (auto &kv : TD){
            char lab = kv.first;
            if (TD[lab].isRoot) continue;
            unordered_map<int,bool> M;
            for (auto &b : TD[lab].blocks) M[b.vpar] = true;
            childHasBlock[lab] = move(M);
        }
        for (char uLabel : preorder){
            TDNode &u = TD[uLabel];
            if (u.isLeaf) continue;
            unordered_set<int> Ucand;
            if (u.isRoot) for (int r : u.rootVcand) Ucand.insert(r);
            else for (auto &b : u.blocks) for (auto &c : b.vcand) Ucand.insert(c.v);
            for (int v : Ucand){
                bool ok = true;
                for (char c : u.children) if (!childHasBlock[c].count(v)) { ok=false; break; }
                if (!ok){ removeCandidateCascade(uLabel, v); changed = true; }
            }
        }
    }

    for (auto &kv : TD) TD[kv.first].rebuildIndex();

    auto T_trim_end = clk::now(); // Stage-2 완료

    // ===== 경로 추출 =====
    auto T_path_start = clk::now();

    vector<vector<char>> paths;
    function<void(char, vector<char>&)> collectPath = [&](char u, vector<char>& cur){
        cur.push_back(u);
        if (TD[u].isLeaf) paths.push_back(cur);
        else for (char w : TD[u].children) collectPath(w, cur);
        cur.pop_back();
    };
    { vector<char> cur; collectPath(rootLabel, cur); }

    vector<pair<int,int>> order;
    for (int i=0;i<(int)paths.size();++i){
        char leaf = paths[i].back();
        size_t csum=0; for (auto &b : TD[leaf].blocks) csum += b.vcand.size();
        order.push_back({(int)csum, i});
    }
    sort(order.begin(), order.end());

    auto T_path_end = clk::now(); // 경로 추출/정렬 완료

    // ===== 경로별 열거 =====
    auto T_enum_start = clk::now();

    struct Match { unordered_map<char,int> asg; vector<int> ts; };
    auto enumerate_path = [&](const vector<char>& path)->vector<Match>{
        vector<Match> out;
        if (TD[rootLabel].rootVcand.empty()) return out;

        function<void(int,int,vector<int>&,unordered_map<char,int>&)> dfsPath =
            [&](int depth, int vpar, vector<int>& TS, unordered_map<char,int>& asg){
                if (depth==(int)path.size()){
                    Match m; m.asg = asg; m.ts = TS; out.push_back(std::move(m));
                    return;
                }
                char cur = path[depth];
                auto itIdx = TD[cur].blockIndex.find(vpar);
                if (itIdx == TD[cur].blockIndex.end()) return;
                const Block &blk = TD[cur].blocks[itIdx->second];
                for (const auto &cell : blk.vcand){
                    int v = cell.v;
                    auto itEdge = adj[vpar].find(v);
                    if (itEdge == adj[vpar].end()) continue;
                    vector<int> TS2 = TS.empty() ? itEdge->second : intersect_sorted(TS, itEdge->second);
                    if ((int)TS2.size() < k) continue;
                    asg[cur] = v;
                    if (depth==(int)path.size()-1){
                        Match m; m.asg = asg; m.ts = TS2; out.push_back(std::move(m));
                    } else {
                        dfsPath(depth+1, v, TS2, asg);
                    }
                    asg.erase(cur);
                }
            };

        unordered_map<char,int> asg;
        vector<int> TS0 = allTimesVec; // universe
        asg[rootLabel] = -1;
        for (int r : TD[rootLabel].rootVcand){
            asg[rootLabel] = r;
            if (path.size()==1){
                Match m; m.asg = asg; m.ts = TS0; out.push_back(std::move(m));
            }else{
                dfsPath(1, r, TS0, asg);
            }
        }
        return out;
    };

    vector<vector<Match>> pathMatches(paths.size());
    for (auto &pi : order){
        int idx = pi.second;
        pathMatches[idx] = enumerate_path(paths[idx]);
    }

    auto T_enum_end = clk::now(); // 경로별 열거 완료

    // ===== 경로 간 조인 =====
    auto T_join_start = clk::now();

    auto labels_set = [&](const vector<char>& v)->unordered_set<char>{
        return unordered_set<char>(v.begin(), v.end());
    };
    auto set_intersection_labels = [&](const unordered_set<char>& A, const unordered_set<char>& B){
        vector<char> S; S.reserve(min(A.size(), B.size()));
        for (char x : A) if (B.count(x)) S.push_back(x);
        sort(S.begin(), S.end());
        return S;
    };
    auto getEdgeTS = [&](int a, int b)->const vector<int>*{
        auto it = adj.find(a); if (it!=adj.end()){
            auto it2 = it->second.find(b);
            if (it2 != it->second.end()) return &(it2->second);
        }
        if (!directed){
            auto itb = adj.find(b);
            if (itb!=adj.end()){
                auto it2 = itb->second.find(a);
                if (it2!=itb->second.end()) return &(it2->second);
            }
        }
        return nullptr;
    };
    auto satisfies_nonTree_edges = [&](const unordered_map<char,int>& asg, vector<int>& TS)->bool{
        for (auto &e : qEdges){
            char a=e.first, b=e.second;
            auto ia = asg.find(a), ib = asg.find(b);
            if (ia==asg.end() || ib==asg.end()) continue;
            const vector<int>* ets = getEdgeTS(ia->second, ib->second);
            if (!ets) return false;
            TS = intersect_sorted(TS, *ets);
            if ((int)TS.size() < k) return false;
        }
        return true;
    };

    vector<Match> cur; // partial merge
    vector<char> curLabels;
    if (!order.empty()){
        cur = pathMatches[order[0].second];
        curLabels = paths[order[0].second];
    }
    for (size_t oi=1; oi<order.size(); ++oi){
        int idx = order[oi].second;
        const vector<char>& pLabels = paths[idx];
        const auto pSet = labels_set(pLabels);
        const auto cSet = labels_set(curLabels);
        vector<char> keyLabels = set_intersection_labels(pSet, cSet);

        unordered_map<string, vector<int>> index; index.reserve(cur.size()*2+1);
        auto makeKey=[&](const unordered_map<char,int>& asg, const vector<char>& labs){
            string k; k.reserve(labs.size()*12);
            for (char L : labs){
                auto it = asg.find(L);
                if (it == asg.end()) { k += L; k += "=#;"; }
                else { k += L; k += '='; k += to_string(it->second); k += ';'; }
            }
            return k;
        };
        for (int i=0;i<(int)cur.size();++i) index[ makeKey(cur[i].asg, keyLabels) ].push_back(i);

        vector<Match> next;
        for (const auto &pm : pathMatches[idx]){
            string key = makeKey(pm.asg, keyLabels);
            auto it = index.find(key);
            if (it==index.end()) continue;
            for (int ci : it->second){
                const auto &m = cur[ci];
                bool ok=true;
                vector<int> TS = m.ts.empty()? pm.ts : intersect_sorted(m.ts, pm.ts);
                if ((int)TS.size() < k) { ok=false; }

                if (ok){
                    for (auto &kv : pm.asg){
                        auto it2 = m.asg.find(kv.first);
                        if (it2!=m.asg.end() && it2->second != kv.second){ ok=false; break; }
                    }
                }
                if (ok){
                    unordered_map<char,int> merged = m.asg;
                    for (auto &kv : pm.asg) merged[kv.first] = kv.second;
                    ok = satisfies_nonTree_edges(merged, TS);
                    if (!ok) continue;
                    Match nm; nm.asg = std::move(merged); nm.ts = std::move(TS);
                    next.push_back(std::move(nm));
                }
            }
        }
        cur.swap(next);
        unordered_set<char> uni(curLabels.begin(), curLabels.end());
        for (char L : pLabels) uni.insert(L);
        curLabels.assign(uni.begin(), uni.end());
        sort(curLabels.begin(), curLabels.end());
        if (cur.empty()) break;
    }

    auto T_join_end = clk::now(); // 경로 조인 완료

    // ===== 출력 =====
    ofstream fout(output_path);
    if (!fout){ cerr<<"cannot open output "<<output_path<<"\n"; return 1; }
    auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    fout << "=== Durable Subgraph Matching (TD-tree + path enumeration) ===\n";
    fout << "Input: " << input_path << "\n";
    fout << "Query: " << query_path << "\n";
    fout << "k="<<k<<"  directed="<<(directed?"true":"false") << "\n";
    fout << "Generated at: " << put_time(localtime(&now), "%F %T") << "\n\n";

    fout << "Root label: " << rootLabel
         << "  (selectivity = |V("<<rootLabel<<")|/deg("<<rootLabel<<") = "
         << fixed << setprecision(3) << bestSel << ")\n";
    if (!trim_rebuild_bloom)
        fout << "(Note) Stage-2 trimming without rebuilding Bloom (fast mode)\n";
    fout.unsetf(std::ios::floatfield);

    // 간단 요약
    fout << "\n[Node summaries]\n";
    for (auto &kv : qAdj){
        char lab = kv.first;
        TDNode &node = TD[lab];
        fout << "Node ["<<lab<<"]  type="<<(node.isRoot?"root":(node.isLeaf?"leaf":"internal"))<<"\n";
        if (node.isRoot){
            fout << "  candidates (|Vcand|): " << node.rootVcand.size() << "\n";
        } else {
            size_t tot=0; for (auto &b : node.blocks) tot += b.vcand.size();
            fout << "  blocks: " << node.blocks.size() << " | Σ_b |b.Vcand| = " << tot << "\n";
        }
    }

    // 경로 및 경로별 매칭 수
    fout << "\n[Paths (root->leaf) and enumerated sizes]\n";
    for (int i=0;i<(int)paths.size();++i){
        for (size_t j=0;j<paths[i].size();++j){
            if (j) fout<<"->"; fout<<paths[i][j];
        }
        fout << " : matches=" << pathMatches[i].size() << "\n";
    }

    // 최종 매칭
    fout << "\n[Final durable matches] count=" << cur.size() << "\n";
    for (size_t idxShow=0; idxShow<cur.size(); ++idxShow){
        const auto &m = cur[idxShow];
        vector<pair<char,int>> arr(m.asg.begin(), m.asg.end());
        sort(arr.begin(), arr.end());
        fout << "Match "<<idxShow<<": ";
        for (size_t i=0;i<arr.size();++i){
            if (i) fout<<", ";
            fout << arr[i].first << "->" << arr[i].second;
        }
        fout << "  |TS|="<<m.ts.size()<<"  TS="<<formatTimeIntervals(m.ts) << "\n";
    }

    // ===== 타이밍 출력 =====
    auto dur_ms = [](auto t1, auto t2){ return chrono::duration<double, milli>(t2 - t1).count(); };
    double t_pre   = dur_ms(T0,     T_pre);
    double t_root  = dur_ms(T_pre,  T_root);
    double t_grow  = dur_ms(T_grow_start, T_grow_end);
    double t_trim  = dur_ms(T_trim_start, T_trim_end);
    double t_path  = dur_ms(T_path_start, T_path_end);
    double t_enum  = dur_ms(T_enum_start, T_enum_end);
    double t_join  = dur_ms(T_join_start, T_join_end);
    double t_total = dur_ms(T0,     T_join_end);

    fout << "\n[Timing (ms)]\n";
    fout << "Preprocess (read + build): " << fixed << setprecision(3) << t_pre  << "\n";
    fout << "Root selection:            " << t_root << "\n";
    fout << "Stage-1 growth:            " << t_grow << "\n";
    fout << "Stage-2 trimming:          " << t_trim << "\n";
    fout << "Path extraction/sort:      " << t_path << "\n";
    fout << "Path enumeration:          " << t_enum << "\n";
    fout << "Join across paths:         " << t_join << "\n";
    fout << "Total:                     " << t_total << "\n";

    fout << "\nDone.\n";
    fout.close();

    cout << "Results saved to: " << output_path << "\n";
    return 0;
}
