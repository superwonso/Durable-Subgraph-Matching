#ifndef DSM_V5_WINDOW_STORE_H
#define DSM_V5_WINDOW_STORE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>
#include "TDTree.h"
#include "WindowAlgebra.h"
#include "WStoreFile.h"

// Query-k-dependent storage, indexed by the original temporal edge ID.
// It never rewrites the Graph or compresses gaps in its integer time axis.
class WindowStore {
public:
    WindowStore(const Graph& graph, const Graph& query, const TDTree& tree,
                int k, bool lazy);
    // Offline, graph-wide fixed-ID store: selected k only. The digest must be
    // computed from the actual parsed graph input file by the caller.
    WindowStore(const Graph& graph, const std::string& store_path,
                const std::string& actual_graph_file_sha256, int k);
    WindowStore(const WindowStore&) = delete;
    WindowStore& operator=(const WindowStore&) = delete;
    window_algebra::WindowView get(int temporal_edge_id) const;
    const window_algebra::Domain& domain() const { return *domain_; }
    bool eligible(int id) const;
    // E1 implementation (a): zero-padded copy on load, no store file rewrite.
    void padLoadedToRawWords();
    std::uint64_t paddingNanoseconds() const { return padding_ns_; }
    std::uint64_t preparationNanoseconds() const { return preparation_ns_; }
    std::uint64_t lazyBuildThreadNanoseconds() const {
        return lazy_build_ns_.load(std::memory_order_relaxed);
    }
    std::uint64_t builtEdges() const { return built_edges_.load(); }
    std::size_t relevantEdges() const {
        return loaded_ ? graph_.temporal_edges.size() : relevant_ids_.size();
    }
    std::size_t payloadBytes() const;
    std::size_t storageBytes() const;
    std::size_t scratchIntervalBound(std::size_t query_edges) const;
    bool lazy() const { return lazy_; }
    bool forcedIntervals() const { return forced_intervals_; }
    bool loaded() const { return static_cast<bool>(loaded_); }
    bool rawOnlySection() const {
        return loaded_ && loaded_->header.sections.size() == 1 && loaded_->section.k == 1;
    }
    std::uint64_t loadNanoseconds() const { return loaded_ ? loaded_->metrics.nanoseconds : 0; }
    std::uint64_t loadedBytes() const { return loaded_ ? loaded_->section.bytes : 0; }
    // Null for the unchanged v5 query-built mode, a complete load receipt for v6.
    void writeLoadJson(std::ostream& out) const;
private:
    void build(std::size_t slot) const;
    const Graph& graph_;
    bool lazy_;
    bool forced_intervals_ = false;
    std::optional<window_algebra::Domain> domain_;
    std::unique_ptr<wstore_file::LoadedSection> loaded_;
    std::vector<std::size_t> slots_;
    std::vector<int> relevant_ids_;
    std::unique_ptr<std::once_flag[]> once_;
    mutable std::vector<std::uint64_t> dense_;
    mutable std::vector<std::vector<window_algebra::Interval>> intervals_;
    std::size_t maximum_edge_intervals_ = 0;
    std::uint64_t preparation_ns_ = 0;
    std::uint64_t padding_ns_ = 0;
    mutable std::atomic<std::uint64_t> lazy_build_ns_{0};
    mutable std::atomic<std::uint64_t> built_edges_{0};
    mutable std::atomic<std::size_t> interval_payload_bytes_{0};
};
#endif
