// Stage B correctness, never timing evidence. Reuse the independent bitmask
// oracle and the exact native-interval observer; do not normalize native output.
#define DSM_ADAPTIVE_STAGE_A_LIBRARY
#include "test_adaptive_stage_a.cpp"
#include "../PhysicalKernels.h"

namespace {

DagPasses physicalPasses(int physical, int legacy = 0) {
    DagPasses p;
    p.p1 = legacy & 1; p.p2 = legacy & 2; p.p3 = legacy & 4;
    p.p4 = physical & 1; p.p5 = physical & 2; p.p6 = physical & 4;
    return p;
}

AdaptiveRun physicalNativeRun(const Fixture& f, const QueryPlan& plan, const TDTree& tree,
                             const CandidateDag* dag, std::size_t threads, bool counted = true) {
    ExactObservation observation;
    observation.fixture = &f;
    OperatorCounters ops;
    GopherMatcherOptions options;
    options.threads = threads;
    options.parallel_mode = ParallelMode::Roots;
    options.early_prune = true; options.result_digest = true;
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = false;
    options.executor.ops = counted ? &ops : nullptr;
    options.exact_match_observer = observeExact;
    options.exact_match_observer_context = &observation;
    GopherMatchSummary summary;
    if (dag) {
        GopherMatcher matcher(f.graph, f.query, plan, *dag, tree, f.k, options);
        summary = matcher.run(MatchOutputMode::CountOnly);
    } else {
        GopherMatcher matcher(f.graph, f.query, plan, tree, f.k, options);
        summary = matcher.run(MatchOutputMode::CountOnly);
    }
    require(!observation.duplicate && !observation.invalid_intervals,
            "duplicate answer or non-normalized native active interval");
    require(!summary.count_overflow && observation.answers.size() == summary.match_count,
            "native count differs from complete observed answer set");
    if (counted && dag && dag->passes.physicalAny()) {
        require(ops.ops_physical_raw_candidates == ops.ops_physical_structure_rejections +
                    ops.ops_physical_lmax_rejections + ops.ops_physical_output_candidates,
                "physical raw/structure/Lmax funnel does not reconcile");
        require(ops.ops_physical_output_candidates == ops.ops_physical_injectivity_rejections +
                    ops.ops_physical_temporal_rejections + ops.ops_physical_extensions,
                "physical injectivity/time/extension funnel does not reconcile");
        require(ops.ops_physical_extensions == ops.ops_logical_extensions,
                "physical extension counter differs from legacy logical extensions");
        require(ops.ops_physical_handle_reprobes == 0,
                "fused merge unexpectedly re-probed history handles");
        if (dag->passes.p6) require(ops.ops_physical_neighbor_copy_elements == 0,
                "P6 projected-view path copied neighbor IDs");
    }
    AdaptiveRun result;
    result.answers = std::move(observation.answers);
    result.callback_threads = observation.callback_threads.size();
    result.extensions = ops.ops_logical_extensions.load();
    result.candidates = ops.ops_candidates.load();
    result.summary = std::move(summary);
    return result;
}

void comparePhysical(const fs::path& out, const Fixture& f, const std::string& arm,
                     const Answers& expected, const AdaptiveRun& actual) {
    if (expected != actual.answers) {
        counterexample(out, f, arm, "mapping AND unmodified native active intervals differ", expected, actual.answers);
        throw std::runtime_error("Stage B exact answer-set mismatch: " + arm);
    }
}

void physicalIrChecks(const CandidateDag& dag, const QueryPlan& plan, const Fixture& f,
                      DagPasses passes, double alpha) {
    std::vector<int> position(plan.order.size(), -1);
    for (std::size_t i = 0; i < plan.order.size(); ++i) position[plan.order[i]] = static_cast<int>(i);
    std::map<std::pair<int,int>, unsigned> slot_counts;
    for (const auto& node : dag.nodes) {
        for (auto parent : node.parents) require(parent < node.id, "physical IR not topological");
        if (!passes.physicalAny()) {
            require(dagOpIndex(node.op) < kLegacyDagOpCount, "legacy IR contains physical op");
            require(node.structural_policy == DagStructuralPolicy::LegacyMerge && node.physical_alpha == 1.0 &&
                    node.history_slots.empty() && node.bound_producer == kInvalidDagNode &&
                    node.bound_slot == kInvalidDagHistorySlot && !node.lmax_proven,
                    "all-off/P123 IR changed appended physical metadata defaults");
        }
        if (!passes.p6) require(node.op != DagOp::TdBlockView && node.op != DagOp::OutNeighborView &&
                               node.op != DagOp::InNeighborView, "another pass silently enabled P6 IR");
        if (!passes.p4) require(node.op != DagOp::FusedEdgeProbe && node.op != DagOp::BoundEdgeTimeLookup,
                               "another pass silently enabled P4");
        if (node.op == DagOp::FusedEdgeProbe || node.op == DagOp::RuntimeStructuralChoice) {
            require(node.structural_policy == (passes.p5 ? DagStructuralPolicy::StateCost : DagStructuralPolicy::Probe),
                    "compiled physical strategy differs from flags");
            require(node.physical_alpha == (passes.p5 ? alpha : 1.0), "compiled alpha differs");
            for (const auto& slot : node.history_slots) {
                const int owner = position[slot.from_query_vertex] >= position[slot.to_query_vertex]
                    ? slot.from_query_vertex : slot.to_query_vertex;
                require(owner == node.parameter.first_query_vertex, "wrong last-bound edge slot owner");
                ++slot_counts[{slot.from_query_vertex, slot.to_query_vertex}];
            }
        }
        if (node.op == DagOp::BoundEdgeTimeLookup) {
            require(node.lmax_proven && node.parents == std::vector<DagNodeId>{node.bound_producer},
                    "history lacks explicit proved producer dependency");
            const auto& producer = dag.node(node.bound_producer);
            require(node.bound_slot < producer.history_slots.size(), "bound slot outside producer schema");
            const auto& slot = producer.history_slots[node.bound_slot];
            require(slot.from_query_vertex == node.parameter.first_query_vertex &&
                    slot.to_query_vertex == node.parameter.second_query_vertex, "bound slot directed identity differs");
        }
    }
    if (passes.p4) for (auto arc : f.arcs) {
        const int owner = position[arc.first] >= position[arc.second] ? arc.first : arc.second;
        require(slot_counts[arc] == (owner == plan.root ? 0U : 1U), "missing/duplicate directed history slot");
    }
}

template<class F> void mustReject(F&& fn, const std::string& reason) {
    bool rejected = false;
    try { fn(); } catch (const std::exception&) { rejected = true; }
    require(rejected, reason);
}

void physicalUnits(const fs::path& out) {
    using PhysicalKernels::chooseProbe;
    for (std::uint64_t merge = 0; merge < 200; ++merge) for (std::uint64_t probe = 0; probe < 400; ++probe) {
        require(chooseProbe({probe, merge}, .5) == (probe * 2 <= merge), "alpha .5 boundary");
        require(chooseProbe({probe, merge}, 1) == (probe <= merge), "alpha 1 boundary");
        require(chooseProbe({probe, merge}, 2) == (probe <= merge * 2), "alpha 2 boundary");
    }
    const auto max = std::numeric_limits<std::uint64_t>::max();
    require(chooseProbe({max, max}, 2) && chooseProbe({max, max}, 1) && !chooseProbe({max, max}, .5),
            "alpha cost comparison uint64 overflow boundary");
    mustReject([] { PhysicalKernels::chooseProbe({1,1}, 1.1); }, "unregistered alpha accepted");
    mustReject([] { parseDagPasses("p7"); }, "unauthorized P7 accepted");
    mustReject([] { parseDagPasses("p4,p4"); }, "duplicate physical pass accepted");
    std::vector<int> empty, singleton{7}, several{1,3,7,11};
    std::vector<Edge> edges{{1,8},{3,9},{7,10},{11,11}};
    CandidateReadView projected(edges), integers(several);
    require(projected.size() == integers.size() && projected.edge(2)->temporal_edge_id == 10,
            "Edge projection lost the actual edge identity");
    for (int candidate = -2; candidate < 15; ++candidate) {
        const auto expected = static_cast<std::size_t>(std::lower_bound(several.begin(), several.end(), candidate) - several.begin());
        require(PhysicalKernels::lowerBound(projected, candidate) == expected, "projected lower_bound differs");
    }
    require(PhysicalKernels::intersectViews(CandidateReadView(empty), projected, false, nullptr, 0).empty(), "empty view intersection");
    require(PhysicalKernels::intersectViews(CandidateReadView(singleton), projected, false, nullptr, 0) == singleton,
            "singleton projected intersection");
    const auto zero_cost = PhysicalKernels::stateCosts(0, {CandidateReadView(empty)});
    require(zero_cost.probe == 0 && zero_cost.merge == 0, "empty state costs");
    const auto one_cost = PhysicalKernels::stateCosts(1, {CandidateReadView(singleton)});
    require(one_cost.probe == 0 && one_cost.merge == 2, "singleton state costs");

    // SAME compiled node, two reachable root states: (R,L)=(8,8) chooses
    // merge, (1,8) chooses probe. No outcome/timing-guided threshold tuning.
    const Bits full = fullBits(8);
    std::vector<Label> labels(18, 3);
    labels[0]=labels[1]=0;
    for (int i=2; i<10; ++i) labels[i]=1;
    labels[10]=2;
    std::vector<std::tuple<int,int,Bits>> graph_edges;
    for (int i=2; i<10; ++i) { graph_edges.emplace_back(0,i,full); graph_edges.emplace_back(i,10,full); }
    graph_edges.emplace_back(1,2,full);
    for (int i=11; i<18; ++i) graph_edges.emplace_back(1,i,full);
    const auto f = fixedFixture("physical_state_choice", {{0,1},{1,2}}, {0,1,2}, labels, graph_edges);
    const auto qd = fixedDecomposition(f, {-1,0,1}, {0,1,2});
    const TDTree tree(f.graph, f.query, qd, f.k);
    const auto plan = makeQueryPlan(f.query, qd, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const auto base = compiler.compile(f.query, plan, qd);
    const auto all_off = optimizeCandidateDag(base, f.graph, f.query, plan, tree, {});
    require(dagIdentity(base)==dagIdentity(all_off), "unit all-off IR identity");
    mustReject([&] { optimizeCandidateDag(base,f.graph,f.query,plan,tree,physicalPasses(1),.5); },
               "alpha without P5 accepted");
    for (int mask : {2,3,6,7}) {
        const auto dag = optimizeCandidateDag(base,f.graph,f.query,plan,tree,physicalPasses(mask));
        OperatorCounters ops;
        SearchStatisticsCollector stats(false);
        DagExecutorOptions options;
        options.memo_mode=MemoMode::None; options.collect_statistics=false; options.ops=&ops;
        DagExecutor executor(f.graph,f.query,plan,dag,tree,f.k,options,stats);
        SearchState a,b;
        a.mapping={0,-1,-1}; b.mapping={1,-1,-1};
        auto parent_batch=executor.physicalCandidates(1,a);
        const auto child_batch=executor.physicalCandidates(1,b);
        require(parent_batch.size()==8 && child_batch.size()==1, "actual state-cost candidate batch differs");
        require(ops.ops_physical_probe_selections==1 && ops.ops_physical_merge_selections==1,
                "same compiled node did not choose different strategies by state");
        require(ops.ops_physical_history_payload_reads==0 && ops.ops_history_lookups_executed==0,
                "candidate phase read temporal payload before injectivity");
        if (mask & 1) {
            const auto row=parent_batch.historyRow(0);
            const auto candidate=parent_batch.candidate(0);
            // A second evaluation has not overwritten parent_batch's handles.
            const auto support=executor.temporalSupportPhysical(1,candidate,a,&row);
            require(support.has_constraints && !support.intervals->empty(), "parent frame handle corrupted by later batch");
            ProbeHistoryRow foreign=row; foreign.candidate+=1;
            mustReject([&] { executor.temporalSupportPhysical(1,candidate,a,&foreign); }, "foreign candidate row accepted");
            foreign=row; foreign.producer=kInvalidDagNode;
            mustReject([&] { executor.temporalSupportPhysical(1,candidate,a,&foreign); }, "foreign producer row accepted");
            require(ops.ops_physical_handle_reprobes==0, "merge repeated handle membership search");
        }
        std::ostringstream evidence; ops.writeJsonMembers(evidence,true);
        writeText(out / ("state_choice_ops_mask"+std::to_string(mask)+".txt"),evidence.str());
    }
    // Necessary Lmax guards do not prove a common k-run. Add deterministic
    // negative cases as well as the positive reciprocal/self-loop parallel set.
    std::vector<Fixture> negative;
    const std::vector<Label> negative_labels{0,1,2,3,3,3,3,3};
    negative.push_back(fixedFixture("disjoint_cumulative_support", {{0,1},{1,2}}, {0,1,2},
        negative_labels, {{0,1,Bits{0x07}},{1,2,Bits{0x1c}}}));
    negative.push_back(fixedFixture("missing_nonroot_selfloop", {{0,1},{1,1},{1,2}}, {0,1,2},
        negative_labels, {{0,1,full},{1,2,full}}));
    negative.push_back(fixedFixture("short_nonroot_selfloop", {{0,1},{1,1},{1,2}}, {0,1,2},
        negative_labels, {{0,1,full},{1,1,Bits{0x03}},{1,2,full}}));
    negative.push_back(fixedFixture("missing_root_selfloop", {{0,0},{0,1},{1,2}}, {0,1,2},
        negative_labels, {{0,1,full},{1,2,full}}));
    negative.push_back(fixedFixture("short_root_selfloop", {{0,0},{0,1},{1,2}}, {0,1,2},
        negative_labels, {{0,0,Bits{0x03}},{0,1,full},{1,2,full}}));
    std::uint64_t negative_checks=0;
    for (const auto& fixture:negative) {
        const auto expected=oracle(fixture);
        require(expected.empty(),"negative independent witness is not empty");
        const auto decomposition=fixedDecomposition(fixture,{-1,0,1},{0,1,2});
        const TDTree relation(fixture.graph,fixture.query,decomposition,fixture.k);
        const auto fixed_plan=makeQueryPlan(fixture.query,decomposition,relation,OrderMode::Paper);
        CandidateDagCompiler negative_compiler;
        const auto unmodified=negative_compiler.compile(fixture.query,fixed_plan,decomposition);
        comparePhysical(out,fixture,"direct_negative",expected,physicalNativeRun(fixture,fixed_plan,relation,nullptr,1));
        for (int legacy=0;legacy<8;++legacy) for (int physical=0;physical<8;++physical) {
            const auto passes=physicalPasses(physical,legacy);
            for (double alpha:{.5,1.0,2.0}) {
                if (!passes.p5 && alpha!=1) continue;
                const auto rewritten=optimizeCandidateDag(unmodified,fixture.graph,fixture.query,fixed_plan,relation,passes,alpha);
                physicalIrChecks(rewritten,fixed_plan,fixture,passes,alpha);
                comparePhysical(out,fixture,dagPassesName(passes)+"_negative",expected,
                    physicalNativeRun(fixture,fixed_plan,relation,&rewritten,1));
                ++negative_checks;
            }
        }
    }
    require(negative_checks==640,"negative fixture configuration coverage");
    writeText(out / "physical_unit_receipt.json",
        "{\"status\":\"PASS\",\"alpha_boundaries\":true,\"alpha_comparisons\":240000,\"empty\":true,\"singleton\":true,\"same_node_both_strategies\":true,\"frame_owned_history\":true,\"foreign_rows_rejected\":true,\"payload_after_injectivity_boundary\":true,\"merge_no_reprobe\":true,\"negative_fixed_fixtures\":5,\"negative_rewrite_checks\":640,\"explicit_cumulative_k_rejection\":true,\"root_and_nonroot_missing_short_loops\":true}\n");
}

void physicalDifferential(const fs::path& out) {
    std::mt19937_64 rng(kGraphSeed);
    std::uint64_t comparisons=0, off_counter_checks=0;
    std::map<std::pair<int,std::string>,Stratum> strata;
    std::ofstream log(out / "physical_differential_cases.csv");
    log << "k,shape,instance,vertices,edges,horizon,answers,direct_extensions,physical_subsets,p123_subsets,all_exact_sets_pass\n";
    for (int k : {2,3,4}) for (const auto& shape : kAdaptiveShapes) {
        for (int instance=0; instance<kInstancesPerStratum; ++instance) {
            const auto f=makeAdaptiveFixture(rng,k,shape,instance);
            Answers expected;
            try {
                expected=oracle(f);
                require(!f.planted || !expected.empty(), "independent planted witness missing");
                const auto qd=decomposition(f);
                const TDTree tree(f.graph,f.query,qd,k);
                const auto plan=makeQueryPlan(f.query,qd,tree,OrderMode::Paper);
                CandidateDagCompiler compiler;
                const auto base=compiler.compile(f.query,plan,qd);
                const auto direct=physicalNativeRun(f,plan,tree,nullptr,1);
                comparePhysical(out,f,"direct",expected,direct); ++comparisons;
                for (int legacy=0; legacy<8; ++legacy) for (int physical=0; physical<8; ++physical) {
                    const auto passes=physicalPasses(physical,legacy);
                    for (double alpha : {.5,1.0,2.0}) {
                        if (!passes.p5 && alpha!=1) continue;
                        const auto dag=optimizeCandidateDag(base,f.graph,f.query,plan,tree,passes,alpha);
                        if (!passes.any()) require(dagIdentity(base)==dagIdentity(dag), "all-off IR changed");
                        physicalIrChecks(dag,plan,f,passes,alpha);
                        const std::string name=dagPassesName(passes)+"_alpha"+std::to_string(alpha);
                        const auto actual=physicalNativeRun(f,plan,tree,&dag,1);
                        comparePhysical(out,f,name,expected,actual); ++comparisons;
                        require(actual.summary.result_digest==direct.summary.result_digest,
                                "equal answer sets have different mapping digest");
                        require(actual.extensions==direct.extensions, "static physical rewrite changed accepted logical extensions");
                        if (instance==0 && legacy==0 && alpha==1) {
                            const auto uncounted=physicalNativeRun(f,plan,tree,&dag,1,false);
                            comparePhysical(out,f,name+"_ops_off",expected,uncounted); ++comparisons; ++off_counter_checks;
                            require(uncounted.summary.result_digest==actual.summary.result_digest, "ops observer changed digest");
                        }
                    }
                }
                auto& counts=strata[{k,shape}]; ++counts.instances;
                counts.positive+=!expected.empty(); counts.zero+=expected.empty();
                counts.planted+=f.planted; counts.answer_total+=expected.size();
                log << k << ',' << shape << ',' << instance << ',' << f.graph.num_vertices << ',' << f.graph.temporal_edges.size()
                    << ',' << f.horizon << ',' << expected.size() << ',' << direct.extensions << ",8,8,true\n";
                log.flush();
            } catch (const std::exception& error) {
                if (!fs::exists(out / "counterexample.json")) counterexample(out,f,"physical_invariant",error.what(),expected,{});
                throw;
            }
        }
        std::cout << "PHYSICAL DIFFERENTIAL PASS k=" << k << " shape=" << shape << " instances=200" << std::endl;
    }
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"instances\":4200,\"seed\":" << kGraphSeed
            << ",\"native_exact_set_comparisons\":" << comparisons << ",\"ops_off_checks\":" << off_counter_checks
            << ",\"physical_subsets\":8,\"p123_subsets\":8,\"alpha_values\":[0.5,1,2],\"strata\":[";
    bool comma=false;
    for (const auto& entry:strata) {
        const auto& s=entry.second;
        require(s.instances==200 && s.positive>0 && s.zero>0,"missing positive/zero stratum coverage");
        if (comma) receipt << ','; comma=true;
        receipt << "{\"k\":" << entry.first.first << ",\"shape\":" << quote(entry.first.second)
                << ",\"instances\":" << s.instances << ",\"positive\":" << s.positive << ",\"zero\":" << s.zero
                << ",\"planted\":" << s.planted << ",\"answer_total\":" << s.answer_total << '}';
    }
    receipt << "]}\n";
    writeText(out / "physical_differential_receipt.json",receipt.str());
}

void physicalParallel(const fs::path& out) {
    std::vector<std::string> shapes(kAdaptiveShapes.begin(),kAdaptiveShapes.end());
    shapes.push_back("reciprocal_selfloop");
    std::ofstream log(out / "physical_parallel_checks.csv");
    log << "shape,passes,alpha,requested_threads,threads_used,callback_threads,answers,exact_sets_equal,count_equal,digest_equal,extensions_equal\n";
    std::uint64_t pairs=0,max_callbacks=0;
    for (const auto& shape:shapes) {
        const auto arcs=shape=="reciprocal_selfloop"
            ? std::vector<std::pair<int,int>>{{0,0},{0,1},{1,0},{1,2},{2,1},{2,2}} : adaptiveArcs(shape);
        int qn=0; for (auto [a,b]:arcs) qn=std::max(qn,std::max(a,b)+1);
        std::vector<std::tuple<int,int,Bits>> edges;
        for (int a=0;a<8;++a) for (int b=0;b<8;++b)
            if (a!=b || shape=="reciprocal_selfloop") edges.emplace_back(a,b,Bits{0xe7});
        const auto f=fixedFixture("physical_parallel_"+shape,arcs,std::vector<Label>(qn,0),std::vector<Label>(8,0),edges);
        Answers expected;
        try {
            expected=oracle(f); require(!expected.empty(),"parallel witness empty");
            const auto qd=decomposition(f);
            const TDTree tree(f.graph,f.query,qd,f.k);
            const auto plan=makeQueryPlan(f.query,qd,tree,OrderMode::Paper);
            CandidateDagCompiler compiler; const auto base=compiler.compile(f.query,plan,qd);
            for (int legacy=0;legacy<8;++legacy) for (int physical=0;physical<8;++physical) {
                const auto passes=physicalPasses(physical,legacy);
                for (double alpha:{.5,1.0,2.0}) {
                    if (!passes.p5 && alpha!=1) continue;
                    const auto dag=optimizeCandidateDag(base,f.graph,f.query,plan,tree,passes,alpha);
                    physicalIrChecks(dag,plan,f,passes,alpha);
                    const auto serial=physicalNativeRun(f,plan,tree,&dag,1);
                    const auto parallel=physicalNativeRun(f,plan,tree,&dag,16);
                    const std::string name=dagPassesName(passes);
                    comparePhysical(out,f,name+"_1t",expected,serial);
                    comparePhysical(out,f,name+"_16t",expected,parallel);
                    require(serial.summary.match_count==parallel.summary.match_count &&
                            serial.summary.result_digest==parallel.summary.result_digest && serial.extensions==parallel.extensions,
                            "parallel count/digest/logical extensions differ");
                    require(parallel.summary.threads_used>1,"parallel fixture took serial-only path");
                    max_callbacks=std::max<std::uint64_t>(max_callbacks,parallel.callback_threads); ++pairs;
                    log << shape << ',' << quote(name) << ',' << alpha << ",16," << parallel.summary.threads_used
                        << ',' << parallel.callback_threads << ',' << expected.size() << ",true,true,true,true\n";
                    if (legacy==0 && alpha==1 && (shape=="path2" || shape=="cycle3"))
                        dumpCandidateDag(dag,plan,(out/"dag_dumps"/(shape+"_mask"+std::to_string(physical))).string(),name);
                }
            }
        } catch (const std::exception& error) {
            if (!fs::exists(out / "counterexample.json")) counterexample(out,f,"physical_parallel_invariant",error.what(),expected,{});
            throw;
        }
    }
    std::ostringstream receipt;
    receipt << "{\"status\":\"PASS\",\"fixtures\":8,\"serial_parallel_pairs\":" << pairs
            << ",\"requested_parallel_threads\":16,\"workers_capped_by_roots\":true,\"maximum_callback_threads_observed\":"
            << max_callbacks << ",\"mapping_and_active_interval_sets_equal\":true,\"mapping_digest_equal\":true}\n";
    writeText(out / "physical_parallel_receipt.json",receipt.str());
}
} // namespace

int main(int argc,char** argv) {
    if (argc!=2) { std::cerr << "Usage: test_adaptive_physical NEW_OUTPUT_DIRECTORY\n"; return 2; }
    const fs::path out(argv[1]);
    if (fs::exists(out)) { std::cerr << "Refusing existing correctness output " << out << '\n'; return 2; }
    fs::create_directories(out);
    try {
        physicalUnits(out);
        physicalDifferential(out);
        physicalParallel(out);
        writeText(out/"physical_native_receipt.json",
            "{\"status\":\"PASS\",\"random_graph_instances\":4200,\"independent_answer_set_oracle\":true,\"active_intervals_compared\":true,\"all_off_ir_unchanged\":true,\"parallel_count_digest_equal\":true,\"parallel_exact_interval_sets_equal\":true,\"P123_compatibility\":true,\"alpha_boundaries\":true,\"physical_subsets\":8,\"p123_subsets\":8,\"alpha_values\":[0.5,1,2],\"failures\":0}\n");
        std::cout << "PHYSICAL CORRECTNESS PASS" << std::endl; return 0;
    } catch (const std::exception& error) {
        const std::string receipt="{\"status\":\"STOPPED\",\"reason\":"+quote(error.what())+",\"no_further_test_instances\":true}\n";
        if (!fs::exists(out/"counterexample.json")) writeText(out/"counterexample.json",receipt);
        writeText(out/"STOP.json",receipt); writeText(out/"physical_native_receipt.json",receipt);
        std::cerr << "STOP: " << error.what() << '\n'; return 1;
    }
}
