#ifndef DSM_STRUCTURAL_PROFILE_H
#define DSM_STRUCTURAL_PROFILE_H

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <ostream>
#include <tuple>

// Stage-1 observation only: no strategy selection, reordered operands, or
// simulated intersections. Callers report the actual binary operation after
// it has finished. All methods are called only when --ops-counters is on;
// census timings must not be mixed with uninstrumented performance runs.
class StructuralProfile {
public:
    enum class Backend { LegacyDag, RewrittenDag, Direct, DirectOpt, Kernel };
    enum class Operand { TdBlock, OutNeighbor, InNeighbor };
    enum class Copy { NeighborMaterialization, StructuralSeed, TdBlock };
    struct Context {
        Backend backend = Backend::Kernel;
        std::int64_t node_id = -1;
        int query_vertex = -1;
        bool operator<(const Context& other) const {
            return std::tie(backend, node_id, query_vertex) <
                   std::tie(other.backend, other.node_id, other.query_vertex);
        }
    };

    // Exact requested formula, including ceil(log2(1)) == 0. Empty operands
    // have probe cost zero by a separately reported convention; log2(0) is
    // never evaluated. There is deliberately no max(1, logarithm) adjustment.
    static std::uint64_t ceilLog2(std::uint64_t value) {
        if (value <= 1) return 0;
        --value;
        std::uint64_t bits = 0;
        while (value != 0) { ++bits; value >>= 1; }
        return bits;
    }

    void recordBinary(Context context, std::size_t left, std::size_t right,
                      std::size_t output, bool binary_search) {
        std::lock_guard<std::mutex> lock(mutex_);
        binary(totals_.binary, left, right, output, binary_search);
        binary(nodes_[context].binary, left, right, output, binary_search);
    }

    void recordOperand(Context context, Operand kind, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto index = static_cast<std::size_t>(kind);
        observe(totals_.operands[index], size);
        observe(nodes_[context].operands[index], size);
    }

    void recordCopy(Context context, Copy kind, std::size_t elements,
                    std::size_t element_bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto index = static_cast<std::size_t>(kind);
        copy(totals_.copies[index], elements, element_bytes);
        copy(nodes_[context].copies[index], elements, element_bytes);
    }

    void recordDirectMembership(Context context) {
        std::lock_guard<std::mutex> lock(mutex_);
        add(totals_.direct_membership_probes, 1);
        add(nodes_[context].direct_membership_probes, 1);
    }

    void recordDirectOutput(Context context, std::size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        observe(totals_.direct_output, size);
        observe(nodes_[context].direct_output, size);
    }

    void writeJson(std::ostream& out) const {
        std::lock_guard<std::mutex> lock(mutex_);
        out << "{\"schema_version\":1,\"overflow\":" << (overflow_ ? "true" : "false")
            << ",\"costs_valid\":" << (overflow_ ? "false" : "true")
            << ",\"definitions\":{"
            << "\"size_histogram\":\"index 0: size=0; index i=1..64: [2^(i-1),2^i)\","
            << "\"ratio_histogram\":\"nonempty pairs only: [1,2),[2,8),[8,32),[32,128),[128,infinity)\","
            << "\"merge_cost\":\"sum(a+b) for actual binary calls, including empty operands\","
            << "\"probe_cost\":\"sum(min(a,b)*ceil(log2(max(a,b)))); empty=0; max=1 gives 0\","
            << "\"zero_probe_denominator\":\"gate ratio is unavailable, never a substituted finite value\","
            << "\"operand_sizes\":\"physical lookup executions only; cache-hit returns are excluded; binary sizes include any cached operands actually consumed\","
            << "\"context_query_vertex\":\"lookup anchor for DAG nodes; extension vertex for Direct; -1 for DAG intersections; not a universal query-stage index\","
            << "\"copy_bytes\":\"logical payload written/copied, not RSS, allocation, or hardware memory traffic; move/swap/cache-hit/borrowed-view=0\","
            << "\"overflow\":\"uint64 saturation marks all costs invalid; do not compute a gate ratio\"},"
            << "\"totals\":";
        writeStats(out, totals_);
        out << ",\"contexts\":[";
        bool first = true;
        for (const auto& item : nodes_) {
            if (!first) out << ',';
            first = false;
            out << "{\"backend\":\"" << backendName(item.first.backend)
                << "\",\"node_id\":" << item.first.node_id
                << ",\"query_vertex\":" << item.first.query_vertex << ",\"stats\":";
            writeStats(out, item.second);
            out << '}';
        }
        out << "]}";
    }

private:
    using U64 = std::uint64_t;
    struct Sizes {
        U64 count = 0, sum = 0, minimum = std::numeric_limits<U64>::max(), maximum = 0;
        std::array<U64, 65> histogram{};
    };
    struct Copies { U64 calls = 0, elements = 0, bytes = 0; };
    struct Binary {
        U64 calls = 0, merge_calls = 0, binary_search_calls = 0;
        U64 empty_left_only = 0, empty_right_only = 0, empty_both = 0, nonempty_calls = 0;
        U64 zero_probe_cost_calls = 0;
        U64 merge_cost_sum = 0, probe_cost_sum = 0;
        U64 nonempty_merge_cost_sum = 0, nonempty_probe_cost_sum = 0;
        std::array<U64, 5> ratio_histogram{};
        Sizes left, right, output;
    };
    struct Stats {
        Binary binary;
        std::array<Sizes, 3> operands{};
        std::array<Copies, 3> copies{};
        U64 direct_membership_probes = 0;
        Sizes direct_output;
    };
    mutable std::mutex mutex_;
    bool overflow_ = false;
    Stats totals_;
    std::map<Context, Stats> nodes_;

    void add(U64& target, U64 value) {
        if (value > std::numeric_limits<U64>::max() - target) {
            overflow_ = true;
            target = std::numeric_limits<U64>::max();
        } else target += value;
    }
    U64 product(U64 first, U64 second) {
        if (second != 0 && first > std::numeric_limits<U64>::max() / second) {
            overflow_ = true;
            return std::numeric_limits<U64>::max();
        }
        return first * second;
    }
    void observe(Sizes& target, U64 value) {
        add(target.count, 1);
        add(target.sum, value);
        target.minimum = std::min(target.minimum, value);
        target.maximum = std::max(target.maximum, value);
        std::size_t index = 0;
        for (U64 remaining = value; remaining != 0; remaining >>= 1) ++index;
        add(target.histogram[index], 1);
    }
    void copy(Copies& target, U64 elements, U64 bytes) {
        add(target.calls, 1);
        add(target.elements, elements);
        add(target.bytes, product(elements, bytes));
    }
    void binary(Binary& target, U64 left, U64 right, U64 output, bool probe) {
        add(target.calls, 1);
        add(probe ? target.binary_search_calls : target.merge_calls, 1);
        observe(target.left, left);
        observe(target.right, right);
        observe(target.output, output);
        const U64 small = std::min(left, right), large = std::max(left, right);
        U64 merge_cost = left;
        add(merge_cost, right);
        const U64 probe_cost = small == 0 ? 0 : product(small, ceilLog2(large));
        add(target.merge_cost_sum, merge_cost);
        add(target.probe_cost_sum, probe_cost);
        if (probe_cost == 0) add(target.zero_probe_cost_calls, 1);
        if (small == 0) {
            if (large == 0) add(target.empty_both, 1);
            else if (left == 0) add(target.empty_left_only, 1);
            else add(target.empty_right_only, 1);
            return;
        }
        add(target.nonempty_calls, 1);
        add(target.nonempty_merge_cost_sum, merge_cost);
        add(target.nonempty_probe_cost_sum, probe_cost);
        // Integer division has exactly the same classification at these
        // integer boundaries; multiplication/floating rounding is avoided.
        const U64 ratio = large / small;
        const std::size_t bucket = ratio < 2 ? 0 : ratio < 8 ? 1 : ratio < 32 ? 2 : ratio < 128 ? 3 : 4;
        add(target.ratio_histogram[bucket], 1);
    }
    static const char* backendName(Backend backend) {
        switch (backend) {
            case Backend::LegacyDag: return "dag_legacy";
            case Backend::RewrittenDag: return "dag_rewritten";
            case Backend::Direct: return "direct_membership";
            case Backend::DirectOpt: return "direct_opt";
            case Backend::Kernel: return "kernel";
        }
        return "invalid";
    }
    template <std::size_t N>
    static void writeArray(std::ostream& out, const std::array<U64, N>& values) {
        out << '[';
        for (std::size_t i = 0; i < N; ++i) { if (i != 0) out << ','; out << values[i]; }
        out << ']';
    }
    static void writeSizes(std::ostream& out, const Sizes& value) {
        out << "{\"count\":" << value.count << ",\"sum\":" << value.sum << ",\"min\":";
        if (value.count == 0) out << "null"; else out << value.minimum;
        out << ",\"max\":";
        if (value.count == 0) out << "null"; else out << value.maximum;
        out << ",\"histogram_log2\":";
        writeArray(out, value.histogram);
        out << '}';
    }
    static void writeCopies(std::ostream& out, const Copies& value) {
        out << "{\"calls\":" << value.calls << ",\"elements\":" << value.elements << ",\"bytes\":" << value.bytes << '}';
    }
    static void writeStats(std::ostream& out, const Stats& value) {
        const auto& b = value.binary;
        out << "{\"binary\":{\"calls\":" << b.calls
            << ",\"merge_strategy_calls\":" << b.merge_calls
            << ",\"binary_search_strategy_calls\":" << b.binary_search_calls
            << ",\"empty_left_only\":" << b.empty_left_only
            << ",\"empty_right_only\":" << b.empty_right_only
            << ",\"empty_both\":" << b.empty_both
            << ",\"nonempty_calls\":" << b.nonempty_calls
            << ",\"zero_probe_cost_calls\":" << b.zero_probe_cost_calls
            << ",\"merge_cost_sum\":" << b.merge_cost_sum
            << ",\"probe_cost_sum\":" << b.probe_cost_sum
            << ",\"nonempty_merge_cost_sum\":" << b.nonempty_merge_cost_sum
            << ",\"nonempty_probe_cost_sum\":" << b.nonempty_probe_cost_sum
            << ",\"ratio_histogram\":";
        writeArray(out, b.ratio_histogram);
        out << ",\"left_sizes\":"; writeSizes(out, b.left);
        out << ",\"right_sizes\":"; writeSizes(out, b.right);
        out << ",\"output_sizes\":"; writeSizes(out, b.output);
        out << "},\"operands\":{\"td_block\":"; writeSizes(out, value.operands[0]);
        out << ",\"out_neighbor\":"; writeSizes(out, value.operands[1]);
        out << ",\"in_neighbor\":"; writeSizes(out, value.operands[2]);
        out << "},\"copies\":{\"neighbor_materialization\":"; writeCopies(out, value.copies[0]);
        out << ",\"structural_seed\":"; writeCopies(out, value.copies[1]);
        out << ",\"td_block\":"; writeCopies(out, value.copies[2]);
        out << "},\"direct_membership_probes\":" << value.direct_membership_probes;
        out << ",\"direct_output_sizes\":"; writeSizes(out, value.direct_output);
        out << '}';
    }
};

#endif
