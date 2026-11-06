#ifndef DSM_V8_RAW_BITSET_H
#define DSM_V8_RAW_BITSET_H

#include <array>
#include <cstdint>
#include "WindowAlgebra.h"
#include "WStoreFile.h"

namespace raw_bitset {
struct RunWork {
    std::uint64_t shift_and_steps = 0;
    std::uint64_t shift_word_ops = 0;
};
using Scratch = std::array<std::uint64_t, window_algebra::kDenseWords>;

// The input is NEVER modified. Bit i denotes original timestamp min+i.
// No interval normalization and no gap/rank compression occur during search.
bool hasRun(window_algebra::WindowView input, int query_k,
            Scratch& scratch, RunWork* counted_work = nullptr);

struct BitVerification {
    std::uint64_t edges = 0;
    std::uint64_t logical_bits = 0;
    std::uint64_t padding_bits = 0;
};
// Offline independent membership oracle: do not use WindowAlgebra::buildInto.
// Includes every timestamp and every padding bit of every temporal-edge row.
BitVerification verifyEveryBit(const Graph& graph,
                              const wstore_file::LoadedSection& loaded);
}
#endif
