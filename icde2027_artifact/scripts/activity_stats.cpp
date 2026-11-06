// E7 exact directed-edge activity statistics and E4 calendar rebucketing.
// New outputs only. Input external IDs, directions, self-loops are retained.
// Deduplication is exactly by (source, destination, snapshot). UTC calendar
// months are consecutive year*12+month ordinals; missing months are not removed.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

using Clock=std::chrono::steady_clock;
struct Triple { uint32_t u,v; int32_t t; };
struct Reader {
    FILE* f; std::array<unsigned char,1<<20> b; size_t pos=0,n=0;
    explicit Reader(const char* p):f(std::fopen(p,"rb")){if(!f)throw std::runtime_error("input open");}
    ~Reader(){if(f)std::fclose(f);}
    int ch(){if(pos==n){n=std::fread(b.data(),1,b.size(),f);pos=0;if(n==0)return EOF;}return b[pos++];}
    bool number(int64_t& out){int c; do{c=ch();if(c==EOF)return false;}while(c==' '||c=='\t'||c=='\n'||c=='\r');
        const bool negative=c=='-';if(negative)c=ch();
        if(c<'0'||c>'9')throw std::runtime_error("integer input required");
        int64_t v=0;do{v=v*10+(c-'0');if(v>INT64_C(2147483648))throw std::runtime_error("int32 input overflow");c=ch();}while(c>='0'&&c<='9');
        if(c!=EOF && c!=' '&&c!='\t'&&c!='\n'&&c!='\r')throw std::runtime_error("invalid delimiter");out=negative?-v:v;return true;}
};
// Howard Hinnant civil-from-days arithmetic, Gregorian calendar in UTC.
static uint32_t month_ordinal(uint32_t timestamp){
    int64_t z=timestamp/86400; z+=719468;
    const int64_t era=(z>=0?z:z-146096)/146097;
    const unsigned doe=static_cast<unsigned>(z-era*146097);
    const unsigned yoe=(doe-doe/1460+doe/36524-doe/146096)/365;
    int64_t y=static_cast<int64_t>(yoe)+era*400;
    const unsigned doy=doe-(365*yoe+yoe/4-yoe/100);
    const unsigned mp=(5*doy+2)/153;
    const unsigned m=mp+(mp<10?3:-9); y+=(m<=2);
    return static_cast<uint32_t>(y*12+(m-1));
}
struct Writer{
    FILE* f; std::array<char,1<<20> b; size_t n=0;
    explicit Writer(const std::string& p):f(std::fopen(p.c_str(),"wb")){if(!f)throw std::runtime_error("output open");}
    void flush(){if(n && std::fwrite(b.data(),1,n,f)!=n)throw std::runtime_error("output write");n=0;}
    void row(uint32_t u,uint32_t v,int32_t t){if(n+40>=b.size())flush();n+=std::snprintf(b.data()+n,b.size()-n,"%u %u %d\n",u,v,t);}
    ~Writer(){if(f){try{flush();}catch(...){std::terminate();}std::fclose(f);}}
};
struct Bin {uint64_t edges=0,occ=0;};
static unsigned activity_bin(uint32_t a){if(a<=2)return a-1;if(a<=5)return 2;if(a<=10)return 3;if(a<=50)return 4;return 5;}
static uint64_t unique_ids(std::vector<uint32_t>& ids){std::sort(ids.begin(),ids.end());ids.erase(std::unique(ids.begin(),ids.end()),ids.end());return ids.size();}
static double elapsed(Clock::time_point start){return std::chrono::duration<double>(Clock::now()-start).count();}

int main(int argc,char** argv){try{
    if(argc!=6)throw std::runtime_error("usage: activity_stats INPUT OUTDIR GRAPH weekly|month|day emit|stats");
    const std::string input=argv[1],out=argv[2],graph=argv[3],mode=argv[4];const bool emit=std::string(argv[5])=="emit";
    if(mode!="weekly"&&mode!="month"&&mode!="day")throw std::runtime_error("mode");
    std::filesystem::create_directories(out);
    if(std::filesystem::exists(out+"/summary.csv"))throw std::runtime_error("refuse completed output overwrite");
    const auto start=Clock::now(); std::vector<Triple> rows;
    rows.reserve(std::filesystem::file_size(input)/(mode=="weekly"?18:24)+1024);
    int32_t min_bucket=INT32_MAX,max_bucket=INT32_MIN; uint64_t input_rows=0;
    { Reader reader(input.c_str());Triple r;int64_t u,v,t;while(reader.number(u)){
        if(!reader.number(v)||!reader.number(t))throw std::runtime_error("incomplete triple");
        if(u<0||v<0||u>INT32_MAX||v>INT32_MAX||t<INT32_MIN||t>INT32_MAX)throw std::runtime_error("v8 integer input range");
        if(mode!="weekly"&&t<0)throw std::runtime_error("pre-epoch UNIX time unsupported");
        r={static_cast<uint32_t>(u),static_cast<uint32_t>(v),static_cast<int32_t>(t)};
        if(mode=="month")r.t=month_ordinal(r.t);else if(mode=="day")r.t/=86400;
        min_bucket=std::min(min_bucket,r.t);max_bucket=std::max(max_bucket,r.t); rows.push_back(r);++input_rows;
    }}
    if(rows.empty())throw std::runtime_error("empty input");
    const int32_t origin=mode=="weekly"?0:min_bucket-1;
    if(origin)for(auto& r:rows)r.t-=origin;
    const double read_s=elapsed(start);std::cerr<<graph<<' '<<mode<<" read_rows="<<rows.size()<<" read_s="<<read_s<<"\n";
    std::sort(rows.begin(),rows.end(),[](const Triple&a,const Triple&b){return std::tie(a.u,a.v,a.t)<std::tie(b.u,b.v,b.t);});
    rows.erase(std::unique(rows.begin(),rows.end(),[](const Triple&a,const Triple&b){return a.u==b.u&&a.v==b.v&&a.t==b.t;}),rows.end());
    const double sorted_s=elapsed(start);std::cerr<<graph<<' '<<mode<<" unique_rows="<<rows.size()<<" elapsed_s="<<sorted_s<<"\n";
    std::vector<uint32_t> vertices,filtered_vertices;std::map<uint32_t,Bin> exact;
    std::array<Bin,6> abin{},lbin{};std::set<int32_t> snapshots,fsnapshots;
    uint64_t edges=0,filtered_edges=0,filtered_occ=0,a1=0,gapped=0;
    int32_t tmin=INT32_MAX,tmax=INT32_MIN,fmin=INT32_MAX,fmax=INT32_MIN;
    Writer* raw=emit?new Writer(out+"/raw.txt"):nullptr;Writer* filtered=emit?new Writer(out+"/f2.txt"):nullptr;
    for(size_t begin=0;begin<rows.size();){
        size_t end=begin+1;while(end<rows.size()&&rows[end].u==rows[begin].u&&rows[end].v==rows[begin].v)++end;
        const auto a=static_cast<uint32_t>(end-begin);uint32_t run=1,lmax=1;
        for(size_t j=begin+1;j<end;++j){run=rows[j].t==rows[j-1].t+1?run+1:1;lmax=std::max(lmax,run);}
        ++edges;vertices.push_back(rows[begin].u);vertices.push_back(rows[begin].v);
        ++exact[a].edges;exact[a].occ+=a;++abin[activity_bin(a)].edges;abin[activity_bin(a)].occ+=a;
        ++lbin[std::min(lmax,6U)-1].edges;lbin[std::min(lmax,6U)-1].occ+=a;
        if(a==1)++a1;else if(lmax==1)++gapped;
        if(lmax>=2){++filtered_edges;filtered_occ+=a;filtered_vertices.push_back(rows[begin].u);filtered_vertices.push_back(rows[begin].v);}
        for(size_t j=begin;j<end;++j){const auto&r=rows[j];snapshots.insert(r.t);tmin=std::min(tmin,r.t);tmax=std::max(tmax,r.t);if(raw)raw->row(r.u,r.v,r.t);
            if(lmax>=2){fsnapshots.insert(r.t);fmin=std::min(fmin,r.t);fmax=std::max(fmax,r.t);if(filtered)filtered->row(r.u,r.v,r.t);}}
        begin=end;
    }
    delete raw;delete filtered;
    const auto nv=unique_ids(vertices),nfv=unique_ids(filtered_vertices);
    if(emit){std::ofstream v(out+"/raw_external_ids.txt"),fv(out+"/f2_external_ids.txt");for(auto id:vertices)v<<id<<'\n';for(auto id:filtered_vertices)fv<<id<<'\n';}
    const uint64_t occ=rows.size();const uint32_t span=int64_t(tmax)-tmin+1,fspan=filtered_edges?int64_t(fmax)-fmin+1:0;
    auto share=[](uint64_t x,uint64_t n){return n?double(x)/n:0.0;};
    std::ofstream summary(out+"/summary.csv");summary<<std::setprecision(17);
    summary<<"graph,granularity,source,input_rows,unique_occurrences,duplicate_rows_removed,vertices,edges,snapshot_count,time_min,time_max,time_span,f2_vertices,f2_edges,f2_occurrences,f2_snapshot_count,f2_time_min,f2_time_max,f2_time_span,edge_removal_ratio,occurrence_removal_ratio,a1_edges,a1_edge_ratio,a_ge2_lmax1_edges,a_ge2_lmax1_edge_ratio,f2_raw_history_words,f2_eroded_words_k2,f2_eroded_words_k3,f2_eroded_words_k4,f2_eroded_words_k5,source_bucket_origin,read_s,sort_s,total_s\n";
    summary<<graph<<','<<mode<<','<<input<<','<<input_rows<<','<<occ<<','<<input_rows-occ<<','<<nv<<','<<edges<<','<<snapshots.size()<<','<<tmin<<','<<tmax<<','<<span<<','<<nfv<<','<<filtered_edges<<','<<filtered_occ<<','<<fsnapshots.size()<<','<<(filtered_edges?fmin:0)<<','<<(filtered_edges?fmax:0)<<','<<fspan<<','<<share(edges-filtered_edges,edges)<<','<<share(occ-filtered_occ,occ)<<','<<a1<<','<<share(a1,edges)<<','<<gapped<<','<<share(gapped,edges)<<','<<(fspan+63)/64;
    for(uint32_t k=2;k<=5;++k)summary<<','<<(fspan>=k?(fspan-k+1+63)/64:0);
    summary<<','<<origin<<','<<read_s<<','<<sorted_s-read_s<<','<<elapsed(start)<<'\n';
    std::ofstream activity(out+"/activity_bins.csv"),runs(out+"/lmax_bins.csv");
    activity<<std::setprecision(17)<<"graph,granularity,a_bin,edges,edge_share,occurrences,occurrence_share\n";
    runs<<std::setprecision(17)<<"graph,granularity,lmax_bin,edges,edge_share,occurrences,occurrence_share\n";
    const char* alabels[]={"1","2","3-5","6-10","11-50",">50"};const char* llabels[]={"1","2","3","4","5",">=6"};
    for(size_t j=0;j<6;++j){activity<<graph<<','<<mode<<','<<alabels[j]<<','<<abin[j].edges<<','<<share(abin[j].edges,edges)<<','<<abin[j].occ<<','<<share(abin[j].occ,occ)<<'\n';runs<<graph<<','<<mode<<','<<llabels[j]<<','<<lbin[j].edges<<','<<share(lbin[j].edges,edges)<<','<<lbin[j].occ<<','<<share(lbin[j].occ,occ)<<'\n';}
    std::ofstream ccdf(out+"/ccdf.csv");ccdf<<std::setprecision(17)<<"graph,granularity,a_threshold,edges_ge_a,ccdf_ge_a\n";uint64_t remaining=edges;
    for(uint32_t a=1;a<=exact.rbegin()->first;++a){ccdf<<graph<<','<<mode<<','<<a<<','<<remaining<<','<<share(remaining,edges)<<'\n';auto it=exact.find(a);if(it!=exact.end())remaining-=it->second.edges;}
    std::ofstream top(out+"/top_share.csv");top<<std::setprecision(17)<<"graph,granularity,top_edge_fraction,selected_edges,occurrences,occurrence_share,rounding\n";
    for(unsigned percent:{1U,5U,20U}){const uint64_t selected=(edges*percent+99)/100;uint64_t left=selected,covered=0;for(auto it=exact.rbegin();it!=exact.rend()&&left;++it){const auto take=std::min(left,it->second.edges);covered+=take*it->first;left-=take;}top<<graph<<','<<mode<<','<<percent/100.0<<','<<selected<<','<<covered<<','<<share(covered,occ)<<",ceil_fraction_times_edges\n";}
    if(a1+gapped!=edges-filtered_edges)throw std::runtime_error("F2 partition mismatch");
    std::cerr<<graph<<' '<<mode<<" DONE edges="<<edges<<" F2="<<filtered_edges<<" seconds="<<elapsed(start)<<'\n';
    return 0;
}catch(const std::exception&e){std::cerr<<"ERROR "<<e.what()<<'\n';return 1;}}

