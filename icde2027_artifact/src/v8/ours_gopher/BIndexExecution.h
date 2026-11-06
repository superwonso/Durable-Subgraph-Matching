#ifndef DSM_V6_BINDEX_EXECUTION_H
#define DSM_V6_BINDEX_EXECUTION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <ostream>
#include <vector>

#include "WindowAlgebra.h"

struct Edge;
struct Graph;
struct QueryPlan;
class TDTree;
class WindowStore;

// Candidate-block index. A CSR entry is a position in one immutable TD block,
// never an adjacency position or an unverified data vertex. No DAG is built.
class BIndexExecution {
public:
    enum class Mode { Adaptive, Always };
    struct WorkerScratch {
        // Grow when an index is actually selected. Scratch belongs to one worker, not the
        // shared index. No returned selection borrows this mutable storage.
        void reserve(std::size_t maximum_block_size);
        std::size_t storageBytes() const;
        std::vector<std::uint64_t> bitmap;
        std::vector<std::size_t> touched_words;
    };
    struct Selection {
        bool used = false;
        int parent_query = -1;
        bool parent_outgoing = false; // mapped parent -> candidate
        std::uint64_t n_idx = 0;
        // Sorted in the ORIGINAL R order. Pointers borrow immutable Graph
        // adjacency and remain valid across recursion and other workers.
        std::vector<int> candidates;
        std::vector<const Edge*> parent_entries;
    };

    BIndexExecution(const Graph& graph, const Graph& query, const QueryPlan& plan,
                    const TDTree& tree, const WindowStore& windows,
                    int k, std::size_t B, std::size_t n_min, std::uint64_t c,
                    double alpha, Mode mode, bool counters_enabled = false,
                    bool verify_superset = false,
                    std::optional<std::uint64_t> finalized_capacity_bytes = std::nullopt);
    ~BIndexExecution();
    BIndexExecution(const BIndexExecution&) = delete;
    BIndexExecution& operator=(const BIndexExecution&) = delete;

    Selection select(int query_vertex, int parent_data_vertex,
                     const std::vector<int>& original_td_block,
                     window_algebra::WindowView prior, WorkerScratch& scratch) const;
    // All actual candidates after injectivity / after successful exact W,
    // respectively, including scan states. Never counts index-build probes.
    void recordTimeCheck(int query_vertex, std::uint64_t count = 1) const;
    void recordTimePass(int query_vertex, std::uint64_t count = 1) const;
    std::size_t maximumBlockSize() const;
    std::uint64_t setupNanoseconds() const;
    std::uint64_t buildThreadNanoseconds() const;
    std::uint64_t buildWallNanoseconds() const;
    // Preserve v6's virtual full-registry cap debit. These conservative payload
    // bounds are not allocator/RSS measurements; actual lazy payload is in JSON.
    std::uint64_t storageBytes() const;
    std::uint64_t peakIndexBytes() const;
    void writeJson(std::ostream& out) const; // after all workers join

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#endif
