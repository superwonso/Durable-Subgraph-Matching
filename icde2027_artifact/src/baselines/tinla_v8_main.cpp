#include "common/BaselineV8.h"
#include "paper_baselines/tinla/tinla.h"
#include <iomanip>

int main(int argc,char** argv) {
    try {
        using namespace baseline_v8;
        const Options o=parse(argc,argv,true);std::string error;
        ExplicitRuntimeLabelMap labels;auto projection_ns=loadProjection(o,labels);
        auto start=Clock::now();std::vector<tinla::ValidatedRuntimeLabel> native_labels;
        for(const auto& [id,label]:labels.labels_by_external_id)native_labels.push_back({id,label});
        projection_ns+=elapsed(start);
        tinla::Graph graph;tinla::V8LoadTimings load;double read_ms=0,preprocess_ms=0;
        if(!tinla::readTemporalGraph(o.graph,o.seed,o.label_mode=="stable-id"?tinla::LabelMode::StableId:tinla::LabelMode::SequentialRng,
             graph,&read_ms,&preprocess_ms,error,o.map.empty()?nullptr:&native_labels,o.prefilter,&load))throw std::runtime_error(error);
        start=Clock::now();tinla::Query query;if(!tinla::readQuery(o.query,query,error))throw std::runtime_error(error);
        if(!o.map.empty()) for(auto label:query.labels)
            if(label>=labels.selected_global_classes.size())throw std::runtime_error("query label has no selected global class");
        const auto query_ns=elapsed(start);
        tinla::RunOptions opts;opts.minimum_duration=o.k;opts.label_seed=o.seed;opts.threads=o.threads;opts.count_only=o.count_only;
        opts.label_mode=o.label_mode=="stable-id"?tinla::LabelMode::StableId:tinla::LabelMode::SequentialRng;
        opts.duration_mode=o.duration_mode=="consecutive"?tinla::DurationMode::Consecutive:tinla::DurationMode::Cardinality;
        opts.index_mode=o.index=="tinla"?tinla::IndexMode::Tinla:tinla::IndexMode::Ctinla;
        tinla::RunResult result;
        if(graph.labels.empty()&&o.prefilter) {
            // Empty F2 input has no edge-query mapping. This adapter handles
            // the valid empty filtered graph before the native nonempty API.
            result.threads_requested=o.threads;result.threads_used=1;
        } else if(!tinla::runMatcher(graph,query,opts,result,error))throw std::runtime_error(error);
        const auto& c=result.counters;
        std::map<std::string,std::uint64_t> counters{{"initial_candidates",c.initial_candidates},{"post_index_candidates",c.post_index_candidates},
            {"post_refine_candidates",c.post_refine_candidates},{"index_pruned",c.index_pruned},{"refine_pruned",c.refine_pruned},
            {"refine_rounds",c.refine_rounds},{"root_candidates",c.root_candidates},{"dfs_states",c.dfs_states},
            {"raw_extension_candidates",c.raw_extension_candidates},{"injectivity_pruned",c.injectivity_pruned},
            {"structural_pruned",c.structural_pruned},{"temporal_pruned",c.temporal_pruned},{"dynamic_refine_calls",c.dynamic_refine_calls},
            {"dynamic_refine_pruned",c.dynamic_refine_pruned},{"dynamic_refine_rounds",c.dynamic_refine_rounds}};
        const auto& t=result.timings;
        std::map<std::string,std::uint64_t> phases{{"read_parse",load.read_parse_ns},{"label_assignment",projection_ns+load.label_ns},
            {"filter",load.filter_ns},{"graph_construction",load.graph_build_ns},{"index_relation",t.build_index_ns+t.filter_candidates_ns+t.refine_candidates_ns},
            {"planning",query_ns+t.order_ns},{"enumeration",result.primary_metrics.wall_ns},{"output",0}};
        const auto stem=std::filesystem::path(o.graph).stem().string();start=Clock::now();
        std::ofstream matches(std::filesystem::path(o.output_dir)/("matching_results_"+stem+".txt"));
        matches<<"Count: "<<result.match_count<<'\n';
        if(!o.count_only) for(std::size_t row=0;row<result.matches.size();++row) {
            const auto& m=result.matches[row];matches<<"Match "<<row+1<<": ";
            for(std::size_t q=0;q<m.mapping.size();++q){if(q)matches<<", ";matches<<query.names[q]<<'('<<tinla::labelToString(query.labels[q])<<")->"<<graph.externalId(m.mapping[q]);}
            matches<<" | active=[";for(std::size_t j=0;j<m.support.size();++j){if(j)matches<<", ";matches<<m.support[j].start;if(m.support[j].end!=m.support[j].start)matches<<'-'<<m.support[j].end;}matches<<"]\n";
        }
        matches.close();if(!matches)throw std::runtime_error("matching output failed");
        std::ofstream timing(std::filesystem::path(o.output_dir)/("timing_results_"+stem+".txt"));
        timing<<"algorithm: tinla_reimpl_v8_input_adapter\nmatches: "<<result.match_count<<"\nenumerate_ns: "<<t.enumerate_ns<<'\n';
        dsm::pure_online_v2::writePrimaryMetrics(timing,result.primary_metrics);
        for(const auto& [key,value]:counters)timing<<key<<": "<<value<<'\n';
        timing.close();if(!timing)throw std::runtime_error("timing output failed");phases["output"]=elapsed(start);
        std::map<std::string,std::uint64_t> sizes{{"vertices",graph.labels.size()},{"edges",graph.edges.size()},
            {"input_occurrences",graph.input_occurrences},{"unique_occurrences",graph.unique_occurrences},
            {"graph_capacity_bytes",graph.memoryBytes()},{"index_capacity_bytes",result.index_memory_bytes},
            {"index_cells",result.index_cells},{"index_segments",result.index_segments}};
        writeMetrics(o,o.index,result.match_count,result.count_overflow,result.threads_used,phases,counters,sizes,labels);
        std::cout<<"Final durable matches: "<<result.match_count<<'\n';return result.count_overflow?5:0;
    } catch(const std::exception& e) {std::cerr<<"Error: "<<e.what()<<'\n';return 2;}
}
