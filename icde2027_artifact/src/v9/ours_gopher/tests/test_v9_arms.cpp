// Requested 200-graph small gate only: reuse v8/v7 fixture oracle, no old driver.
#define main preserved_v5_driver_not_called_by_v9
#include "test_v5_matcher.cpp"
#undef main
#include "../WStoreFile.h"
#include "../WindowStore.h"

namespace {
void nativeV9(const Fixture& f, const QueryPlan& plan, const TDTree& tree,
    const fs::path& store, const std::string& sha, const std::string& arm, NativeResult& actual) {
    actual=NativeResult{};
    Observation observation; observation.fixture=&f;
    OperatorCounters counters;
    GopherMatcherOptions options;
    options.threads=16; options.order_mode=OrderMode::Paper;
    options.parallel_mode=ParallelMode::Roots; options.early_prune=true;
    options.executor.memo_mode=MemoMode::None; options.executor.collect_statistics=false;
    options.executor.ops=&counters;
    options.direct_p4=arm!="dfs"; options.window_algebra=arm!="dfs";
    options.raw_bitset=arm=="bitset";
    options.window_pad_raw_words=arm=="win_s_w6";
    options.window_block_union=arm=="win_s_union";
    if (arm!="dfs") { options.wstore_path=store.string(); options.graph_file_sha256=sha; }
    options.exact_match_observer=observe; options.exact_match_observer_context=&observation;
    GopherMatcher matcher(f.graph,f.query,plan,tree,f.k,options);
    actual.summary=matcher.run(MatchOutputMode::CountOnly);
    actual.answers=std::move(observation.answers);
    actual.invalid=observation.invalid; actual.duplicate=observation.duplicate;
}

void checkFixtureV9(const fs::path& out, const Fixture& f, std::size_t instance, std::ofstream& log) {
    const auto scratch=out/("fixture_"+std::to_string(instance));
    fs::create_directory(scratch);
    std::ostringstream text;
    for (const auto& e:f.graph.temporal_edges) for (const auto& r:e.activeIntervals())
        for (int t=r.start;t<=r.end;++t)
            text << f.graph.externalId(e.u) << ' ' << f.graph.externalId(e.v) << ' ' << t << '\n';
    const auto input=scratch/"graph.txt", eroded=scratch/"eroded.store", raw=scratch/"raw.store";
    writeNew(input,text.str());
    const auto sha=wstore_file::graphFileSha256(input.string());
    wstore_file::build(f.graph,input.string(),"v9_small_gate",{f.k},eroded.string());
    wstore_file::build(f.graph,input.string(),"v9_small_gate",{1},raw.string());
    const auto decomposition=decompositionFor(f);
    const TDTree tree(f.graph,f.query,decomposition,f.k);
    const auto plan=makeQueryPlan(f.query,decomposition,tree,OrderMode::Paper);
    const auto expected=independentOracle(f);
    std::uint64_t raw_words=0;
    for (const std::string arm : {"dfs","bitset","win_s","win_s_w6","win_s_union"}) {
        NativeResult actual;
        try {
            nativeV9(f,plan,tree,arm=="bitset"?raw:eroded,sha,arm,actual);
            require(!actual.invalid && !actual.duplicate && !actual.summary.count_overflow,"invalid answers");
            require(actual.answers==expected && actual.summary.match_count==expected.size(),
                    "mapping/normalized interval answer set differs from independent oracle");
            if (arm=="bitset") raw_words=actual.summary.v9_window_words;
            if (arm=="win_s_w6") require(actual.summary.v9_window_words==raw_words,"padding width mismatch");
            log << instance << ',' << f.k << ',' << f.shape << ',' << f.horizon << ',' << arm << ','
                << expected.size() << ',' << actual.summary.v9_window_words << ','
                << actual.summary.v9_union_blocks_checked << ',' << actual.summary.v9_union_blocks_skipped
                << ",PASS\n";
        } catch (const std::exception& error) {
            counterexample(out,f,arm,error.what(),expected,actual);
            throw;
        }
    }
    log.flush();
}
}

int main(int argc,char** argv) {
    if (argc!=2) { std::cerr << "usage: test_v9_arms NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out=argv[1];
    if (fs::exists(out)) { std::cerr << "output already exists\n"; return 2; }
    fs::create_directories(out);
    try {
        std::ofstream log(out/"exact_sets.csv");
        log << "instance,k,shape,horizon,arm,answers,physical_words,union_checked,union_skipped,status\n";
        std::mt19937_64 random(kSeed);
        for (std::size_t i=0;i<200;++i) {
            const auto f=randomFixture(random,2+static_cast<int>((i/7)%4),kShapes[i%7],
                                       static_cast<int>((i/28)%4),static_cast<int>(i));
            checkFixtureV9(out,f,i,log);
        }
        // One targeted original Bitcoin span: k>=4 gives physical 6 versus logical 5 words.
        auto boundary=loopFixture("positive_both_loops",323); boundary.k=5;
        for (int t=0;t<5;++t) {
            boundary.histories[1][1][t]=1;
            boundary.histories[1][1][322-t]=1;
        }
        finishGraph(boundary);
        require(!independentOracle(boundary).empty(),"323-snapshot boundary must be positive");
        checkFixtureV9(out,boundary,200,log);
        writeNew(out/"PASS.json","{\"status\":\"PASS\",\"random_graphs\":200,\"boundary_graphs\":1,"
            "\"arms\":5,\"native_matcher_calls\":1005,\"threads\":16,\"answer_sets_equal\":true,\"performance_runs\":0}\n");
        std::cout << "PASS: 200 random graphs and one 323-snapshot boundary, five arms, exact answer sets\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
