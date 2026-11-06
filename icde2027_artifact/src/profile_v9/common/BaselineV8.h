#ifndef DSM_BASELINE_V8_H
#define DSM_BASELINE_V8_H
#include "LabelProjection.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include <limits>

namespace baseline_v8 {
using Clock = std::chrono::steady_clock;
inline std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-start).count());
}
struct Options {
    std::string graph, query, map, projection, output_dir=".";
    unsigned seed=42;
    int k=0;
    std::size_t threads=1;
    bool count_only=false, prefilter=false;
    std::string label_mode="stable-id", duration_mode="consecutive", index="tinla";
};
inline unsigned long long integer(const std::string& value, const char* role) {
    if (value.empty() || value.front()=='-') throw std::runtime_error(std::string("Invalid ")+role);
    std::size_t n=0; auto number=std::stoull(value,&n);
    if(n!=value.size()) throw std::runtime_error(std::string("Invalid ")+role);
    return number;
}
inline Options parse(int argc,char** argv,bool tinla) {
    if(argc<4) throw std::runtime_error("Usage: BASELINE DATA QUERY K [seed] --count-only --threads 16 --prefilter on|off --output-dir DIR [--global-label-map MAP --query-label-projection PROJECTION]");
    Options o; o.graph=argv[1];o.query=argv[2];
    auto k=integer(argv[3],"k"); if(k<1||k>std::numeric_limits<int>::max()) throw std::runtime_error("k out of range");o.k=static_cast<int>(k);
    std::map<std::string,bool> seen;
    for(int i=4;i<argc;++i) {
        std::string key=argv[i], value;
        if(key=="--count-only") {if(seen[key])throw std::runtime_error("duplicate count-only");seen[key]=true;o.count_only=true;continue;}
        if(key.rfind("--",0)!=0) {if(seen["seed"])throw std::runtime_error("duplicate seed");seen["seed"]=true;auto seed=integer(key,"seed");if(seed>std::numeric_limits<unsigned>::max())throw std::runtime_error("seed overflow");o.seed=static_cast<unsigned>(seed);continue;}
        const auto equal=key.find('=');
        if(equal!=std::string::npos) {value=key.substr(equal+1);key.resize(equal);} else {if(i+1>=argc)throw std::runtime_error("missing option value");value=argv[++i];}
        if(value.empty()||seen[key])throw std::runtime_error("empty/duplicate option "+key);
        seen[key]=true;
        if(key=="--threads") {auto n=integer(value,"threads");if(n<1||n>1024)throw std::runtime_error("threads out of range");o.threads=n;}
        else if(key=="--prefilter") {if(value!="on"&&value!="off")throw std::runtime_error("invalid prefilter");o.prefilter=value=="on";}
        else if(key=="--label-mode") {if(value!="stable-id"&&value!="sequential-rng")throw std::runtime_error("invalid label mode");o.label_mode=value;}
        else if(key=="--duration-mode") {if(value!="consecutive"&&value!="cardinality")throw std::runtime_error("invalid duration mode");o.duration_mode=value;}
        else if(key=="--output-dir")o.output_dir=value;
        else if(key=="--global-label-map")o.map=value;
        else if(key=="--query-label-projection")o.projection=value;
        else if(key=="--index"&&tinla) {if(value!="tinla"&&value!="ctinla")throw std::runtime_error("invalid index");o.index=value;}
        else throw std::runtime_error("unknown option "+key);
    }
    if(o.map.empty()!=o.projection.empty())throw std::runtime_error("explicit map and projection are required together");
    if(!o.map.empty()&&o.label_mode!="stable-id")throw std::runtime_error("explicit labels cannot combine with sequential RNG");
    if(o.prefilter&&(o.k<2||o.duration_mode!="consecutive"))throw std::runtime_error("F2 requires consecutive k>=2");
    std::filesystem::create_directories(o.output_dir);
    return o;
}
inline std::uint64_t loadProjection(const Options& o,ExplicitRuntimeLabelMap& labels) {
    const auto start=Clock::now();
    if(!o.map.empty()) {std::string error;if(!loadExplicitRuntimeLabelMap(o.map,o.projection,o.query,labels,error))throw std::runtime_error(error);}
    return elapsed(start);
}
inline void numbers(std::ostream& out,const std::map<std::string,std::uint64_t>& values) {
    out<<'{';bool first=true;for(const auto& [key,value]:values){if(!first)out<<',';first=false;out<<'"'<<key<<"\":"<<value;}out<<'}';
}
inline void writeMetrics(const Options& o,const std::string& system,std::uint64_t count,bool overflow,std::size_t used,
                         const std::map<std::string,std::uint64_t>& phases,const std::map<std::string,std::uint64_t>& counters,
                         const std::map<std::string,std::uint64_t>& graph,const ExplicitRuntimeLabelMap& labels,
                         const std::vector<int>& plan={},int root=-1) {
    std::ofstream out(std::filesystem::path(o.output_dir)/"baseline_metrics.json");
    if(!out)throw std::runtime_error("cannot write baseline_metrics.json");
    out<<"{\"schema\":\"v8_baseline_metrics_v1\",\"system\":\""<<system<<"\",\"match_count\":"<<count
       <<",\"count_overflow\":"<<(overflow?"true":"false")<<",\"threads_requested\":"<<o.threads<<",\"threads_used\":"<<used
       <<",\"count_only\":"<<(o.count_only?"true":"false")<<",\"prefilter\":\""<<(o.prefilter?"on":"off")
       <<"\",\"label_mode\":\""<<(o.map.empty()?o.label_mode:"explicit-projection")<<"\",\"duration_mode\":\""<<o.duration_mode
       <<"\",\"minimum_duration_k\":"<<o.k<<",\"filter_scope\":\""<<(o.prefilter?"edge_history_materialization_and_f2":"not_applied")
       <<"\",\"primary_wall_ns\":"<<phases.at("enumeration")<<",\"phase_ns\":";numbers(out,phases);
    out<<",\"counters\":";numbers(out,counters);out<<",\"graph\":";numbers(out,graph);
    out<<",\"query_root\":"<<root<<",\"plan_order\":[";
    for(std::size_t i=0;i<plan.size();++i){if(i)out<<',';out<<plan[i];}out<<']';
    out<<",\"label_projection\":{\"global_label_map_file_sha256\":\""<<labels.global_label_map_file_sha256
       <<"\",\"query_label_projection_file_sha256\":\""<<labels.query_label_projection_file_sha256
       <<"\",\"query_file_sha256\":\""<<labels.query_file_sha256<<"\",\"invalid_vertex_count_input\":"<<labels.invalid_vertex_count<<"}}\n";
    out.close();if(!out)throw std::runtime_error("failed writing baseline_metrics.json");
}
} // namespace baseline_v8
#endif
