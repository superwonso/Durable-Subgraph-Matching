#ifndef DSM_PHYSICAL_EXECUTION_PROFILE_H
#define DSM_PHYSICAL_EXECUTION_PROFILE_H

#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <ostream>

// Optional census only. Saturation is explicit; no ratios may be derived from
// an overflowed profile. Decision costs are per STATE, never binary-pair costs.
class PhysicalExecutionProfile {
public:
    void record(std::uint32_t node, bool probe, std::uint64_t input,
                std::uint64_t output, std::uint64_t probe_cost,
                std::uint64_t merge_cost) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (Stats* s : {&totals_, &nodes_[node]}) {
            add(s->calls, 1); add(probe ? s->probe : s->merge, 1);
            add(s->input, input); add(s->output, output);
            add(s->probe_cost, probe_cost); add(s->merge_cost, merge_cost);
        }
    }
    void writeJson(std::ostream& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        out << "{\"schema_version\":1,\"overflow\":" << (overflow_ ? "true" : "false")
            << ",\"definitions\":{\"probe_cost\":\"R*sum ceilLog2(Li), sizes 0/1 contribute 0\","
            << "\"merge_cost\":\"R+sum Li; state proxy, not measured CPU work\","
            << "\"strategy\":\"compiled StateCost: probe iff probe_cost<=alpha*merge_cost; otherwise compiled fixed strategy\","
            << "\"zero_costs\":\"P6-only legacy intersection records no state cost comparison and stores zero cost fields\","
            << "\"copy_bytes\":\"logical copied payload, not allocation/RSS/hardware traffic\"},\"totals\":";
        writeStats(out, totals_);
        out << ",\"nodes\":[";
        bool comma = false;
        for (const auto& entry : nodes_) {
            if (comma) out << ',';
            comma = true;
            out << "{\"node_id\":" << entry.first << ",\"stats\":";
            writeStats(out, entry.second); out << '}';
        }
        out << "]}";
    }
private:
    struct Stats {
        std::uint64_t calls=0, probe=0, merge=0, input=0, output=0, probe_cost=0, merge_cost=0;
    };
    mutable std::mutex mutex_;
    bool overflow_ = false;
    Stats totals_;
    std::map<std::uint32_t, Stats> nodes_;
    void add(std::uint64_t& target, std::uint64_t amount) {
        if (amount > std::numeric_limits<std::uint64_t>::max() - target) {
            target = std::numeric_limits<std::uint64_t>::max(); overflow_ = true;
        } else target += amount;
    }
    static void writeStats(std::ostream& out, const Stats& s) {
        out << "{\"calls\":" << s.calls << ",\"probe_selections\":" << s.probe
            << ",\"merge_selections\":" << s.merge << ",\"input_candidates\":" << s.input
            << ",\"output_candidates\":" << s.output << ",\"probe_cost_sum\":" << s.probe_cost
            << ",\"merge_cost_sum\":" << s.merge_cost << '}';
    }
};

#endif
