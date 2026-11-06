#ifndef DSM_V5_JINDEX_EXECUTION_H
#define DSM_V5_JINDEX_EXECUTION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <ostream>
#include <vector>

#include "CandidateReadView.h"
#include "WindowAlgebra.h"

class WindowStore;

// Production P8 storage/selection only. No CandidateDag objects or dispatch are
// required by this class; the caller registers a fixed compiled/recipe driver.
// A selected subset is only a necessary condition. Callers MUST still check
// remaining directed constraints, injectivity and exact cumulative WindowAnd.
class JIndexExecution {
public:
    enum class Mode { Adaptive, Always };
    enum class DriverOrigin { CompiledDag, DirectRecipe };
    struct Selection {
        bool used = false;
        // Sorted, unique IDs already checked for membership in the ORIGINAL TD
        // seed. Parallel pointers borrow immutable Graph adjacency entries and
        // remain valid for the graph's lifetime, including nested DFS frames.
        std::vector<int> candidates;
        std::vector<const Edge*> driver_entries;
        std::uint64_t c_idx = 0;
        std::uint64_t c_scan = 0;
        bool costs_evaluated = false;
    };

    JIndexExecution(const Graph& graph, const window_algebra::Domain& domain,
                    int k, std::size_t B, Mode mode, double alpha,
                    std::size_t query_vertices, bool counters_enabled = false,
                    bool verify_superset = false,
                    const WindowStore* window_source = nullptr);
    ~JIndexExecution();
    JIndexExecution(const JIndexExecution&) = delete;
    JIndexExecution& operator=(const JIndexExecution&) = delete;

    // Preparation only, before worker creation. Duplicate identical metadata
    // is allowed; differing metadata or registration after freeze is an error.
    void registerDriver(int query_vertex, std::uint32_t node_id,
                        int anchor_query_vertex, bool outgoing, DriverOrigin origin);
    void freezeDrivers();
    Selection select(int query_vertex, std::uint32_t node_id,
                     int anchor_data_vertex, bool outgoing,
                     CandidateReadView original_td_seed,
                     const std::vector<CandidateReadView>& all_neighbors,
                     window_algebra::WindowView prior) const;

    std::uint64_t setupNanoseconds() const;
    std::uint64_t lazyBuildThreadNanoseconds() const;
    std::uint64_t persistentIndexBytes() const;
    std::uint64_t peakIndexBytes() const;
    bool memoryUnavailable() const;
    // Must be called after every worker has joined. OFF counters are null;
    // static plans, memory accounting and lazy build telemetry remain visible.
    void writeJson(std::ostream& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

#endif
