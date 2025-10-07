#include <bits/stdc++.h>
using namespace std;

/*
 * T&P Selectivity (Topology & Temporal) — Query Decomposition + Candidate Selection ONLY
 *
 * What this program does (reflecting the two slides you provided):
 *  1) Read a temporal graph as triples: "u v t" (ints). Randomly assign labels A~E to vertices.
 *  2) Read a query graph as pairs of labels per line: "A C" etc. Query vertices are labels.
 *  3) Compute Topological Selectivity (TurboISO-style) for each query vertex label L:
 *         S_topo(L) = |V_L| / deg_Q(L).
 *     Pick the root query vertex as the label whose S_topo is MINIMUM.
 *  4) Compute Temporal Selectivity for each query edge (L1,L2):
 *         Adur(L) = average #unique timestamps of data vertices with label L.
 *         T_sel(L1,L2) = (Adur(L1) + Adur(L2)) / 2.
 *     Build the query TREE by keeping edges with SMALLER T_sel.
 *     Implementation-wise we run Kruskal (min-weight spanning tree on T_sel);
 *     equivalently, this removes (drops) high T_sel edges from cycles — exactly as the slide says.
 *  5) Candidate selection (no full TD-tree, no enumeration):
 *     - For each query vertex label L: candidate vertex set = all data vertices with label L.
 *     - For each tree edge (Lpar, Lchi): candidate DATA EDGES = all (u,v) where
 *         label[u]==Lpar, label[v]==Lchi, and edge(u,v) exists in the data graph.
 *       Optionally filter by minimum duration k on the edge (|timestamps| >= k).
 *
 * Output is written to a file (default: tp_selectivity_output.txt):
 *   - Per-vertex S_topo with counts
 *   - Root (by S_topo)
 *   - Per-edge T_sel and the chosen query tree (MST by T_sel) + removed edges
 *   - Candidate counts and (optionally bounded) lists of candidate data edges per tree edge
 *   - **TD-tree style metrics per tree edge**: #blocks and Σ_b|b.Vcand|
 *
 * Notes:
 *  - We do NOT build TD-tree / Stage-1/2 / enumeration here. This is strictly decomposition + candidates.
 *  - Directed edges: by default the data graph is undirected (use --directed to change it).
 *  - Labels are random but reproducible via --seed.
 */

// -------- Utilities --------
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

struct DSU {
    vector<int> p, r;
    DSU(){}
    DSU(int n){ init(n); }
    void init(int n){ p.resize(n); r.assign(n,0); iota(p.begin(), p.end(), 0); }
    int find(int x){ return (p[x]==x)?x:(p[x]=find(p[x])); }
    bool unite(int a,int b){ a=find(a); b=find(b); if(a==b) return false; if(r[a]<r[b]) swap(a,b); p[b]=a; if(r[a]==r[b]) r[a]++; return true; }
};

int main(int argc, char** argv){
    ios::sync_with_stdio(false);
    cin.tie(nullptr);

    // ---- CLI ----
    string input_path, query_path, output_path="tp_selectivity_output.txt";
    unsigned int seed=42;
    bool directed=false;
    int k=1; // min snapshots for a candidate DATA EDGE (tree-edge candidates)
    int max_edges_list_per_tree_edge = 1000000000; // limit output size per tree edge

    for (int i=1;i<argc;++i){
        string arg = argv[i];
        if ((arg=="-i"||arg=="--input") && i+1<argc) input_path = argv[++i];
        else if ((arg=="-q"||arg=="--query") && i+1<argc) query_path = argv[++i];
        else if ((arg=="-o"||arg=="--output") && i+1<argc) output_path = argv[++i];
        else if (arg=="--seed" && i+1<argc) seed = (unsigned)stoul(argv[++i]);
        else if (arg=="--directed") directed=true;
        else if ((arg=="-k") && i+1<argc) { k = stoi(argv[++i]); if (k<1) k=1; }
        else if ((arg=="--max-list") && i+1<argc) { max_edges_list_per_tree_edge = stoi(argv[++i]); if(max_edges_list_per_tree_edge<0) max_edges_list_per_tree_edge=0; }
        else if (arg=="-h"||arg=="--help"){
            cerr<<"Usage: "<<argv[0]<<" -i edges.txt -q query.txt [-o out.txt]\n"
                <<"  [--seed 42] [--directed] [-k 1] [--max-list 1000]\n";
            return 0;
        }
    }
    if (input_path.empty() || query_path.empty()){
        cerr<<"Error: need -i and -q\n"; return 1;
    }

    using clk = chrono::high_resolution_clock;
    auto T0 = clk::now();

    // ---- Read temporal data edges ----
    struct Edge{int u,v,t;};
    vector<Edge> raw;
    {
        ifstream fin(input_path);
        if(!fin){ cerr<<"Error: cannot open "<<input_path<<"\n"; return 1; }
        long long U,V,T; while(fin>>U>>V>>T){ raw.push_back({(int)U,(int)V,(int)T}); }
    }
    if (raw.empty()){ cerr<<"Error: input edges empty\n"; return 1; }

    // ---- Build vertex set ----
    unordered_set<int> vset;
    for (auto &e : raw){ vset.insert(e.u); vset.insert(e.v); }
    vector<int> vertices(vset.begin(), vset.end());
    sort(vertices.begin(), vertices.end());

    // ---- Assign random labels (A..E) ----
    vector<char> labelOptions = {'A','B','C','D','E'};
    mt19937 rng(seed);
    uniform_int_distribution<int> dist(0,(int)labelOptions.size()-1);
    unordered_map<int,char> dataLabel;
    for (int vid : vertices) dataLabel[vid] = labelOptions[dist(rng)];

    // ---- Build adjacency with timestamp lists ----
    unordered_map<int, unordered_map<int, vector<int>>> adj;
    for (auto &e : raw){
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

    // ---- Compute vertex durations (dur(v) = #unique timestamps incident to v) ----
    unordered_map<int, vector<int>> vertexTimes;
    for (auto &kv1 : adj){
        int u = kv1.first;
        for (auto &kv2 : kv1.second){
            int v = kv2.first;
            for (int t : kv2.second){ vertexTimes[u].push_back(t); vertexTimes[v].push_back(t); }
        }
    }
    for (auto &kv : vertexTimes){
        auto &tv = kv.second;
        sort(tv.begin(), tv.end());
        tv.erase(unique(tv.begin(), tv.end()), tv.end());
    }

    // ---- Data-by-label, and Adur(L) ----
    unordered_map<char, vector<int>> dataByLabel;
    for (auto &kv : dataLabel) dataByLabel[kv.second].push_back(kv.first);
    for (auto &kv : dataByLabel) sort(kv.second.begin(), kv.second.end());

    auto Adur = [&](char L)->double{
        auto it = dataByLabel.find(L);
        if (it==dataByLabel.end() || it->second.empty()) return 0.0;
        long long sum=0;
        for (int v : it->second){
            auto it2 = vertexTimes.find(v);
            int dur = (it2==vertexTimes.end())?0:(int)it2->second.size();
            sum += dur;
        }
        return (double)sum / (double)it->second.size();
    };

    // ---- Read query (labels as vertices, pairs as edges) ----
    unordered_map<char, vector<char>> qAdj;
    vector<pair<char,char>> qEdges;
    {
        ifstream fq(query_path);
        if (!fq){ cerr<<"Error: cannot open "<<query_path<<"\n"; return 1; }
        string a,b;
        while (fq>>a>>b){
            if (a.empty()||b.empty()) continue;
            char A=a[0], B=b[0];
            if (!(A>='A'&&A<='Z'&&B>='A'&&B<='Z')) continue;
            qAdj[A].push_back(B);
            qAdj[B].push_back(A);
            qEdges.push_back({A,B});
        }
    }
    if (qAdj.empty()){ cerr<<"Error: query is empty\n"; return 1; }

    // ---- Query vertex set and degrees ----
    vector<char> qVerts; qVerts.reserve(qAdj.size());
    for (auto &kv : qAdj) qVerts.push_back(kv.first);
    sort(qVerts.begin(), qVerts.end());
    unordered_map<char,int> qDeg;
    for (auto &kv : qAdj){
        unordered_set<char> s(kv.second.begin(), kv.second.end());
        qDeg[kv.first] = (int)s.size();
    }

    auto T_pre = clk::now();

    // ---- (1) Topological Selectivity (TurboISO-style): S_topo = |V_L| / deg_Q ----
    struct TopoInfo { char L; int cand; int deg; double sel; };
    vector<TopoInfo> topoInfos;
    topoInfos.reserve(qVerts.size());
    for (char L : qVerts){
        int cand = dataByLabel.count(L)?(int)dataByLabel[L].size():0;
        int deg  = qDeg.count(L)?qDeg[L]:0;
        double s = (deg==0||cand==0)?numeric_limits<double>::infinity(): (double)cand / (double)deg;
        topoInfos.push_back({L, cand, deg, s});
    }
    // root: min S_topo (tie: smaller cand, then lexicographically smaller label)
    char rootLabel = 0; double bestTopo = numeric_limits<double>::infinity(); int bestCand=INT_MAX;
    for (auto &ti : topoInfos){
        if (ti.deg==0 || ti.cand==0) continue;
        if (ti.sel < bestTopo || (fabs(ti.sel-bestTopo)<1e-12 && (ti.cand<bestCand || (ti.cand==bestCand && ti.L<rootLabel)))){
            bestTopo = ti.sel; bestCand = ti.cand; rootLabel = ti.L;
        }
    }
    if (rootLabel==0){ cerr<<"Error: cannot choose root by topological selectivity\n"; return 1; }

    auto T_topo = clk::now();

    // ---- (2) Temporal Selectivity for query edges ----
    struct QEdgeSel { char a,b; double tsel; double a_adur; double b_adur; };
    vector<QEdgeSel> edgeSel;
    edgeSel.reserve(qEdges.size());
    for (auto &e : qEdges){
        char A=e.first, B=e.second;
        double aA = Adur(A), aB = Adur(B);
        double tsel = (aA + aB) / 2.0; // average of endpoint Adurs
        edgeSel.push_back({A,B,tsel,aA,aB});
    }

    // ---- Build query tree by keeping edges with SMALLER Temporal Selectivity (Kruskal) ----
    // Map labels to 0..n-1 indices
    unordered_map<char,int> idx;
    for (int i=0;i<(int)qVerts.size();++i) idx[qVerts[i]] = i;

    // Sort edges by ascending tsel
    sort(edgeSel.begin(), edgeSel.end(), [&](const QEdgeSel& x, const QEdgeSel& y){
        if (x.tsel != y.tsel) return x.tsel < y.tsel;
        if (x.a != y.a) return x.a < y.a;
        return x.b < y.b;
    });

    DSU dsu((int)qVerts.size());
    vector<pair<char,char>> treeEdges, removedEdges;
    for (auto &es : edgeSel){
        int u = idx[es.a], v = idx[es.b];
        if (dsu.unite(u,v)){
            treeEdges.push_back({es.a, es.b});
        } else {
            // would close a cycle -> drop this (high tsel edges get dropped first since sorted ascending keep min)
            removedEdges.push_back({es.a, es.b});
        }
    }
    // It is possible the query is disconnected; in that case we will get a forest.
    // (Most query patterns are connected, though.)

    // ---- Orient the tree edges from root (BFS) ----
    unordered_map<char, vector<char>> treeAdj;
    for (auto &e : treeEdges){
        treeAdj[e.first].push_back(e.second);
        treeAdj[e.second].push_back(e.first);
    }
    unordered_map<char, char> parent; parent[rootLabel]=0;
    vector<char> bfsOrder;
    queue<char>q; q.push(rootLabel);
    while(!q.empty()){
        char u = q.front(); q.pop();
        bfsOrder.push_back(u);
        for (char v : treeAdj[u]) if (!parent.count(v)) { parent[v]=u; q.push(v); }
    }
    vector<pair<char,char>> oriented; // parent->child
    for (auto &e : treeEdges){
        char a=e.first, b=e.second;
        if (parent[b]==a) oriented.push_back({a,b});
        else if (parent[a]==b) oriented.push_back({b,a});
        else {
            // if one of endpoints not reachable from root (disconnected), direct arbitrarily from a->b
            oriented.push_back({a,b});
        }
    }

    auto T_temporal = clk::now();

    // ---- Candidate selection ----
    // (A) Per-vertex candidates: simply by label
    unordered_map<char, vector<int>> vertexCandidates = dataByLabel;

    // (B) Per-tree-edge candidate DATA edges (flat) + TD-tree style block metric
    struct CandEdge { int u,v; vector<int> ts; };
    unordered_map<string, vector<CandEdge>> candPerTreeEdge;
    auto keyOf = [](char a, char b){ string s; s.push_back(a); s.push_back('-'); s.push_back(b); return s; };
    // TD-tree style metric per tree-edge:
    //   - blocks:    number of parent data vertices (label Lp) that have >=1 child candidate (label Lc)
    //   - sumVcand:  Σ_b |b.Vcand|  (sum of child-candidate counts over all parent blocks)
    // NOTE: this follows TD-tree semantics; in undirected graphs with Lp==Lc
    //       the same unordered pair {u,v} may contribute to two different parent blocks.
    unordered_map<string, pair<size_t,size_t>> tdBlockMetric; // key -> (blocks, sumVcand)
    size_t grandTotalBlocks = 0, grandTotalTDsum = 0;

    for (auto &pr : oriented){
        char Lp = pr.first, Lc = pr.second;
        string key = keyOf(Lp,Lc);
        auto &vec = candPerTreeEdge[key];
        // (1) Build flat candidate (u,v) list as before
        for (auto &kv1 : adj) {
            int u = kv1.first;
            if (dataLabel[u] != Lp) continue;
            for (auto &kv2 : kv1.second) {
                int v = kv2.first;
                if (dataLabel[v] != Lc) continue;
                const auto &ts = kv2.second;
                if ((int)ts.size() < k) continue;
                // Keep flat list deterministic
                if (!directed) {
                    // We do NOT attempt to remove duplicates across parent blocks here,
                    // but for the flat list we keep (u,v) with u<=v to avoid printing twice.
                    if (u > v) continue;
                }
                vec.push_back({u, v, ts});
            }
        }
        // sort by (u,v) for determinism
        sort(vec.begin(), vec.end(), [](const CandEdge& a, const CandEdge& b){
            if (a.u != b.u) return a.u < b.u;
            return a.v < b.v;
        });

        // (2) TD-tree style metric (blocks & Σ_b|b.Vcand|) — group by parent candidate (label Lp)
        size_t blocks = 0, sumVcand = 0;
        auto itParents = dataByLabel.find(Lp);
        if (itParents != dataByLabel.end()) {
            for (int u : itParents->second) {
                auto itNei = adj.find(u);
                if (itNei == adj.end()) continue;
                size_t local = 0;
                for (auto &kv2 : itNei->second) {
                    int v = kv2.first;
                    if (dataLabel[v] != Lc) continue;
                    const auto &ts = kv2.second;
                    if ((int)ts.size() < k) continue;
                    // TD-tree semantics: count every child in this parent block.
                    ++local;
                }
                if (local > 0) {
                    ++blocks;
                    sumVcand += local;
                }
            }
        }
        tdBlockMetric[key] = {blocks, sumVcand};
        grandTotalBlocks += blocks;
        grandTotalTDsum  += sumVcand;
    }

    auto T_cands = clk::now();

    // ---- Write output ----
    ofstream fout(output_path);
    if (!fout){ cerr<<"Error: cannot open output "<<output_path<<"\n"; return 1; }

    auto now = chrono::system_clock::to_time_t(chrono::system_clock::now());
    fout << "=== T&P Selectivity — Decomposition + Candidate Selection ONLY ===\n";
    fout << "Input: " << input_path << "\n";
    fout << "Query: " << query_path << "\n";
    fout << "Directed: " << (directed?"true":"false") << "\n";
    fout << "k (min snapshots per candidate data edge): " << k << "\n";
    fout << "Generated at: " << put_time(localtime(&now), "%F %T") << "\n\n";

    // Topological selectivity table
    fout << "[Topological Selectivity per query vertex label]\n";
    fout << "Format: Label  deg_Q  |V_L|  S_topo=|V_L|/deg_Q\n";
    fout << fixed << setprecision(6);
    for (auto &ti : topoInfos){
        fout << "  " << ti.L << "    " << ti.deg << "     " << ti.cand << "     ";
        if (isinf(ti.sel)) fout << "inf\n"; else fout << ti.sel << "\n";
    }
    fout << "\nSelected ROOT (min S_topo): " << rootLabel
         << "  (S_topo=" << bestTopo << ", |V_L|=" << bestCand << ", deg_Q=" << qDeg[rootLabel] << ")\n\n";
    fout.unsetf(std::ios::floatfield);

    // Temporal selectivity per edge
    fout << "[Temporal Selectivity per query edge]\n";
    fout << "Format: (A,B)  Adur(A)  Adur(B)  T_sel=(Adur(A)+Adur(B))/2\n";
    fout << fixed << setprecision(3);
    for (auto &es : edgeSel){
        fout << "  (" << es.a << "," << es.b << ")  " << es.a_adur << "  " << es.b_adur << "  " << ((es.a_adur+es.b_adur)/2.0) << "\n";
    }
    fout.unsetf(std::ios::floatfield);
    fout << "\n";

    // Tree edges + removed edges
    fout << "[Query tree chosen by T&P]\n";
    fout << "Tree edges (kept; prefer SMALLER temporal selectivity):\n";
    for (auto &e : oriented) fout << "  " << e.first << " -> " << e.second << "\n";
    if (!removedEdges.empty()){
        fout << "Removed edges (to break cycles):\n";
        for (auto &e : removedEdges) fout << "  (" << e.first << "," << e.second << ")\n";
    }
    fout << "\n";

    // Candidate sets
    fout << "[Candidate sets]\n";
    // Vertex candidates (only sizes; list could be huge)
    for (char L : qVerts){
        size_t cnt = dataByLabel.count(L)?dataByLabel[L].size():0;
        fout << "  Label " << L << " : |Vcand| = " << cnt << "\n";
    }
    fout << "\n";

    // Candidate data edges per tree edge
    size_t grandTotalEdges = 0;
    for (auto &pr : oriented){
        string key = keyOf(pr.first, pr.second);
        auto &vec = candPerTreeEdge[key];
        grandTotalEdges += vec.size();
        auto bm = tdBlockMetric[key];
        fout << "Tree-edge [" << pr.first << " -> " << pr.second << "]  "
             << "candidate data edges: " << vec.size()
             << "  | TD-tree blocks: " << bm.first
             << "  | Σ_b|b.Vcand|: " << bm.second << "\n";
        int printed = 0;
        for (auto &ce : vec){
            if (printed >= max_edges_list_per_tree_edge){
                fout << "  ... (" << (vec.size()-printed) << " more not shown)\n";
                break;
            }
            fout << "  (" << ce.u << " -> " << ce.v << ")  times=" << formatTimeIntervals(ce.ts) << "\n";
            ++printed;
        }
    }
    fout << "\nTotal candidate DATA EDGES across all tree edges: " << grandTotalEdges << "\n";
    fout << "Total TD-tree Σ_b|b.Vcand| across all tree edges: " << grandTotalTDsum
         << "  (total blocks=" << grandTotalBlocks << ")\n";

    // ---- Timing ----
    auto dur_ms = [](auto t1, auto t2){ return chrono::duration<double, milli>(t2 - t1).count(); };
    double t_pre   = dur_ms(T0,     T_pre);
    double t_topo  = dur_ms(T_pre,  T_topo);
    double t_temp  = dur_ms(T_topo, T_temporal);
    double t_cand  = dur_ms(T_temporal, T_cands);
    double t_total = dur_ms(T0,     T_cands);

    fout << "\n[Timing (ms)]\n";
    fout << fixed << setprecision(3);
    fout << "Preprocess (read/build): " << t_pre   << "\n";
    fout << "Topological selectivity & root: " << t_topo  << "\n";
    fout << "Temporal selectivity & tree (Kruskal): " << t_temp  << "\n";
    fout << "Candidate selection (lists/counts): " << t_cand  << "\n";
    fout << "Total: " << t_total << "\n";
    fout << "\nDone.\n";
    fout.close();

    cout << "Results saved to: " << output_path << "\n";
    return 0;
}
