#include "RawBitset.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace raw_bitset {
namespace W = window_algebra;

bool hasRun(W::WindowView input, int query_k, Scratch& scratch, RunWork* work) {
    if (work) *work = RunWork{};
    if (query_k <= 0 || input.domain().k() != 1 ||
        input.domain().representation() != W::Representation::Dense)
        throw std::invalid_argument("raw run requires positive query k and dense k=1 domain");
    if (input.isUniversal())
        throw std::invalid_argument("raw run cannot test an unconstrained Universal sentinel");
    if (input.isEmpty() || static_cast<std::uint64_t>(query_k) > input.domain().span()) return false;
    const auto words = input.wordCount();
    if (words > scratch.size()) throw std::length_error("raw run scratch width exceeded");
    std::copy_n(input.words(), words, scratch.begin());
    // Invariant: bit i in r means [i,i+len) consists entirely of ones in J.
    // r & (r >> s), s<=len, extends the witnessed interval to len+s.
    // Ascending word order is safe in-place: a right shift reads only the
    // current/higher source words, which have not yet been overwritten.
    std::uint64_t len = 1;
    while (len < static_cast<std::uint64_t>(query_k)) {
        const auto shift = std::min(len, static_cast<std::uint64_t>(query_k) - len);
        const auto whole = static_cast<std::size_t>(shift / 64);
        const auto part = static_cast<unsigned>(shift % 64);
        for (std::size_t i = 0; i < words; ++i) {
            std::uint64_t shifted = 0;
            if (whole < words - i) {
                const auto source = i + whole;
                shifted = scratch[source] >> part;
                if (part && source + 1 < words)
                    shifted |= scratch[source + 1] << (64U - part);
            }
            scratch[i] &= shifted;
        }
        if (work) { ++work->shift_and_steps; work->shift_word_ops += words; }
        len += shift;
    }
    std::uint64_t any = 0;
    for (std::size_t i = 0; i < words; ++i) any |= scratch[i];
    return any != 0;
}

BitVerification verifyEveryBit(const Graph& graph, const wstore_file::LoadedSection& loaded) {
    if (loaded.section.k != 1 || loaded.domain.k() != 1 ||
        loaded.header.sections.size() != 1 || loaded.header.sections.front().k != 1)
        throw std::runtime_error("raw bitset requires exactly one k=1 section");
    if (loaded.header.edges != graph.temporal_edges.size() ||
        loaded.header.vertices != static_cast<std::uint64_t>(graph.num_vertices) ||
        loaded.header.graph_semantic_sha256 != wstore_file::graphSemanticSha256(graph))
        throw std::runtime_error("raw-bitset graph identity mismatch");
    const auto span = loaded.domain.span();
    const auto words = static_cast<std::uint64_t>(loaded.section.words);
    BitVerification checked;
    for (std::size_t id = 0; id < graph.temporal_edges.size(); ++id) {
        const auto& intervals = graph.temporal_edges[id].activeIntervals();
        std::size_t cursor = 0;
        for (std::uint64_t offset = 0; offset < words * 64; ++offset) {
            bool expected = false;
            if (offset < span) {
                const auto timestamp = loaded.domain.timestampAt(offset);
                while (cursor < intervals.size() && intervals[cursor].end < timestamp) ++cursor;
                expected = cursor < intervals.size() && intervals[cursor].start <= timestamp;
                ++checked.logical_bits;
            } else ++checked.padding_bits;
            const auto actual = ((loaded.payload.at(id * words + offset / 64) >> (offset % 64)) & 1U) != 0;
            if (actual != expected)
                throw std::runtime_error("raw bit mismatch edge_id=" + std::to_string(id) +
                                         " offset=" + std::to_string(offset));
        }
        ++checked.edges;
    }
    return checked;
}
}
