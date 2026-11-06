#include "common/BaselineV8.h"
#include "original/TDTree.h"
#include "original/E5BuildProfile.h"
#include <memory>

int main(int argc,char** argv) {
    try {
        using namespace baseline_v8;
        const auto process_start=Clock::now();
        const Options o=parse(argc,argv,false);
        ExplicitRuntimeLabelMap labels;const auto projection_ns=loadProjection(o,labels);
        Graph graph;TemporalGraphLoadTimings load;
        if(!readTemporalGraph(o.graph,graph,o.seed,&load,
            o.label_mode=="stable-id"?LabelMode::StableId:LabelMode::SequentialRng,o.prefilter,
            o.map.empty()?nullptr:&labels.labels_by_external_id))return 2;
        auto start=Clock::now();Graph query;if(!readQueryGraph(o.query,query))return 2;
        if(!o.map.empty()) for(Label label:query.vertex_labels)
            if(label>=labels.selected_global_classes.size())throw std::runtime_error("query label has no selected global class");
        std::array<std::size_t,kLabelCount> label_counts{};
        for(Label label:graph.vertex_labels)if(label<kLabelCount)++label_counts[label];
        const auto decomposition=decomposeQuery(query,label_counts,QueryTreeMode::Dfs);
        if(!decomposition.connected||decomposition.root<0)throw std::runtime_error("query not connected");
        const auto plan_ns=elapsed(start);
        start=Clock::now();
        TDTree tree(graph,query,decomposition,o.k,o.duration_mode=="consecutive"?DurationMode::Consecutive:DurationMode::Cardinality,
                    PathOrderMode::Sorted,2022,true);
        const auto index_ns=elapsed(start);
        const auto build_profile=e5_build_profile::totals;
        tree.print_res();
        const auto stem=std::filesystem::path(o.graph).stem().string();
        const auto matching=(std::filesystem::path(o.output_dir)/("matching_results_"+stem+".txt")).string();
        start=Clock::now();
        const auto result=tree.save_res(matching,o.count_only?MatchOutputMode::CountOnly:MatchOutputMode::Full,o.threads);
        const auto save_ns=elapsed(start);
        if(!result.output_written)throw std::runtime_error("matching output failed");
        std::map<std::string,std::uint64_t> phases{
            {"read_parse",load.read_parse_ns},{"label_assignment",projection_ns+load.label_ns},{"filter",load.filter_ns},
            {"graph_construction",load.graph_build_ns},{"index_relation",index_ns},
            {"planning",plan_ns+result.matching_plan_nanoseconds},{"enumeration",result.primary_metrics.wall_ns},
            {"output",save_ns-result.matching_plan_nanoseconds-result.primary_metrics.wall_ns}};
        std::map<std::string,std::uint64_t> counters{
            {"dfs_states",result.dfs_states},{"generated_candidates",result.generated_candidates},
            {"accepted_candidates",result.accepted_candidates},{"completed_matches",result.completed_matches},
            {"pruned_injectivity",result.pruned_injectivity},{"pruned_temporal_support",result.pruned_temporal_support},
            {"leaf_block_precheck_attempts",result.leaf_block_precheck_attempts},{"leaf_blocks_pruned",result.leaf_blocks_pruned},
            {"leaf_block_candidates_pruned",result.leaf_block_candidates_pruned},{"root_candidate_count",result.root_candidate_count},
            {"candidate_relation_entries",tree.candidateRelationCount()},{"root_tasks_claimed",result.root_tasks_claimed}};
        start=Clock::now();
        std::ofstream timing(std::filesystem::path(o.output_dir)/("timing_results_"+stem+".txt"));
        timing<<"algorithm: tdtree_repo_corrected_v8_input_adapter\nmatch_count: "<<result.match_count
              <<"\nbuildTDTree_ns: "<<index_ns<<"\nmatching_plan_ns: "<<result.matching_plan_nanoseconds
              <<"\nenumerateMatches_ns: "<<result.enumeration_nanoseconds<<'\n';
        timing<<"e5_profile_kind: diagnostic_only_not_performance\n"
              <<"e5_build_vertex_candidate_ns: "<<build_profile.vertex_ns<<'\n'
              <<"e5_build_temporal_ns: "<<build_profile.temporal_ns<<'\n'
              <<"e5_build_leaf_compaction_ns: "<<build_profile.leaf_ns<<'\n'
              <<"e5_build_vertex_candidate_calls: "<<build_profile.vertex_calls<<'\n'
              <<"e5_build_temporal_calls: "<<build_profile.temporal_calls<<'\n'
              <<"e5_build_leaf_compaction_calls: "<<build_profile.leaf_calls<<'\n';
        dsm::pure_online_v2::writePrimaryMetrics(timing,result.primary_metrics);
        for(const auto& [key,value]:counters)timing<<key<<": "<<value<<'\n';
        timing.close();if(!timing)throw std::runtime_error("timing output failed");
        phases["output"]+=elapsed(start);
        std::map<std::string,std::uint64_t> sizes{{"vertices",graph.external_ids.size()},{"edges",graph.temporal_edges.size()},
            {"input_occurrences",graph.input_occurrence_count},{"graph_capacity_bytes",graph.getMemoryUsage()},
            {"index_capacity_bytes",tree.getMemoryUsage()},{"index_logical_bytes",tree.getLogicalMemoryUsageBytes()},
            {"content_fingerprint_fnv1a64",temporalGraphContentFingerprint(graph)}};
        writeMetrics(o,"tdtree",result.match_count,result.count_overflow,result.threads_used,phases,counters,sizes,labels,
                     result.generated_matching_order,decomposition.root);
        std::cout<<"Final durable matches: "<<result.match_count<<"\nprocess_observed_until_metrics_ns: "<<elapsed(process_start)<<'\n';
        return result.count_overflow?5:0;
    } catch(const std::exception& e) {std::cerr<<"Error: "<<e.what()<<'\n';return 2;}
}
