#include "WindowStore.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using Clock = std::chrono::steady_clock;
std::uint64_t elapsed(Clock::time_point start) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
}
constexpr std::size_t missing = std::numeric_limits<std::size_t>::max();
}

WindowStore::WindowStore(const Graph& graph, const std::string& store_path,
                         const std::string& actual_graph_file_sha256, int k)
    : graph_(graph), lazy_(false), loaded_(std::make_unique<wstore_file::LoadedSection>(
          wstore_file::load(store_path, actual_graph_file_sha256,
                           graph.temporal_edges.size(), graph.num_vertices, k))) {
    // No graph scan, candidate-union construction, k transformation, once-state
    // allocation or query-specific W construction takes place in this mode.
    domain_.emplace(loaded_->domain);
}

WindowStore::WindowStore(const Graph& graph, const Graph& query,
                         const TDTree& tree, int k, bool lazy)
    : graph_(graph), lazy_(lazy) {
    // Include domain discovery, candidate-union construction, eligibility,
    // allocation and once-state initialization in query-specific preparation.
    const auto start = Clock::now();
    std::vector<unsigned char> in_domain(static_cast<std::size_t>(graph.num_vertices), 0);
    for (int q = 0; q < query.num_vertices; ++q) {
        for (int v : tree.queryCandidateDomain(q))
            in_domain.at(static_cast<std::size_t>(v)) = 1;
    }
    slots_.assign(graph.temporal_edges.size(), missing);
    std::int64_t minimum = std::numeric_limits<std::int64_t>::max();
    std::int64_t maximum = std::numeric_limits<std::int64_t>::min();
    for (std::size_t id = 0; id < graph.temporal_edges.size(); ++id) {
        const auto& edge = graph.temporal_edges[id];
        const auto& runs = edge.activeIntervals();
        if (!runs.empty()) {
            minimum = std::min(minimum, static_cast<std::int64_t>(runs.front().start));
            maximum = std::max(maximum, static_cast<std::int64_t>(runs.back().end));
        }
        if (in_domain.at(static_cast<std::size_t>(edge.u)) &&
            in_domain.at(static_cast<std::size_t>(edge.v))) {
            slots_[id] = relevant_ids_.size();
            relevant_ids_.push_back(static_cast<int>(id));
            maximum_edge_intervals_ = std::max(maximum_edge_intervals_, runs.size());
        }
    }
    if (minimum > maximum) { minimum = 0; maximum = -1; }
    domain_.emplace(window_algebra::Domain::make(minimum, maximum, k));
    forced_intervals_ = window_algebra::densePayloadBytes(*domain_, relevant_ids_.size()) >
                       (4ULL << 30);
    if (forced_intervals_) domain_.emplace(window_algebra::Domain::make(
        minimum, maximum, k, window_algebra::RepresentationPolicy::ForceIntervals));
    once_ = std::make_unique<std::once_flag[]>(relevant_ids_.size());
    if (domain_->representation() == window_algebra::Representation::Dense) {
        const auto words = static_cast<std::size_t>(domain_->wordCount());
        if (words && relevant_ids_.size() > dense_.max_size() / words)
            throw std::length_error("W dense payload size overflow");
        dense_.resize(relevant_ids_.size() * words);
    } else {
        intervals_.resize(relevant_ids_.size());
    }
    if (!lazy_) {
        std::atomic<std::size_t> next{0};
        std::atomic<bool> cancel{false};
        std::exception_ptr failure;
        std::mutex failure_mutex;
        auto work = [&] {
            try {
                while (!cancel.load(std::memory_order_relaxed)) {
                    const auto slot = next.fetch_add(1, std::memory_order_relaxed);
                    if (slot >= relevant_ids_.size()) break;
                    std::call_once(once_[slot], [&] { build(slot); });
                }
            } catch (...) {
                std::lock_guard<std::mutex> lock(failure_mutex);
                if (!failure) failure = std::current_exception();
                cancel.store(true);
            }
        };
        // Fixed protocol: build uses 16 workers even for a 1T enumeration.
        std::vector<std::thread> workers;
        try {
            for (unsigned i = 1; i < 16; ++i) workers.emplace_back(work);
            work();
        } catch (...) {
            cancel.store(true);
            for (auto& worker : workers) worker.join();
            throw;
        }
        for (auto& worker : workers) worker.join();
        if (failure) std::rethrow_exception(failure);
    }
    preparation_ns_ = elapsed(start);
}

void WindowStore::build(std::size_t slot) const {
    const auto& runs = graph_.temporal_edges.at(
        static_cast<std::size_t>(relevant_ids_.at(slot))).activeIntervals();
    window_algebra::WindowBuffer buffer;
    buffer.reserveIntervals(window_algebra::buildIntervalCapacity(*domain_, runs.size()));
    window_algebra::buildInto(*domain_, runs.data(), runs.size(), buffer);
    const auto view = buffer.view();
    if (domain_->representation() == window_algebra::Representation::Dense) {
        const auto words = static_cast<std::size_t>(domain_->wordCount());
        if (!view.isEmpty())
            std::copy_n(view.words(), words, dense_.data() + slot * words);
    } else if (!view.isEmpty()) {
        const auto starts = view.starts();
        intervals_[slot].assign(starts.data, starts.data + starts.size);
        interval_payload_bytes_.fetch_add(
            intervals_[slot].capacity() * sizeof(window_algebra::Interval),
            std::memory_order_relaxed);
    }
    built_edges_.fetch_add(1, std::memory_order_relaxed);
}

bool WindowStore::eligible(int id) const {
    if (loaded_) return id >= 0 && static_cast<std::size_t>(id) < graph_.temporal_edges.size();
    return id >= 0 && static_cast<std::size_t>(id) < slots_.size() &&
           slots_[static_cast<std::size_t>(id)] != missing;
}

window_algebra::WindowView WindowStore::get(int id) const {
    if (!eligible(id)) throw std::logic_error("W lookup outside frozen candidate-union eligibility");
    if (loaded_) {
        const auto words = static_cast<std::size_t>(loaded_->section.words);
        if (!words) return window_algebra::WindowView::empty(*domain_);
        return window_algebra::WindowView::dense(*domain_,
            loaded_->payload.data() + static_cast<std::size_t>(id) * words, words);
    }
    const auto slot = slots_[static_cast<std::size_t>(id)];
    if (lazy_) {
        std::call_once(once_[slot], [&] {
            const auto start = Clock::now();
            build(slot);
            lazy_build_ns_.fetch_add(elapsed(start), std::memory_order_relaxed);
        });
    }
    if (domain_->representation() == window_algebra::Representation::Dense) {
        const auto words = static_cast<std::size_t>(domain_->wordCount());
        if (!words) return window_algebra::WindowView::empty(*domain_);
        return window_algebra::WindowView::dense(*domain_, dense_.data() + slot * words, words);
    }
    const auto& runs = intervals_[slot];
    return window_algebra::WindowView::intervals(*domain_, {runs.data(), runs.size()});
}

std::size_t WindowStore::payloadBytes() const {
    if (loaded_) return loaded_->payload.capacity() * sizeof(std::uint64_t);
    return dense_.capacity() * sizeof(std::uint64_t) + interval_payload_bytes_.load();
}
std::size_t WindowStore::storageBytes() const {
    if (loaded_) {
        // Account resident payload and heap-owned metadata, not on-disk size.
        const auto& h = loaded_->header;
        std::size_t metadata = sizeof(wstore_file::LoadedSection) + h.dataset_id.capacity() +
            h.graph_file_sha256.capacity() + h.graph_semantic_sha256.capacity() +
            h.header_sha256.capacity() + h.sections.capacity() * sizeof(wstore_file::Section) +
            loaded_->section.sha256.capacity();
        for (const auto& section : h.sections) metadata += section.sha256.capacity();
        return payloadBytes() + metadata;
    }
    return payloadBytes() + slots_.capacity() * sizeof(std::size_t) +
        relevant_ids_.capacity() * sizeof(int) +
        relevant_ids_.size() * sizeof(std::once_flag) +
        intervals_.capacity() * sizeof(std::vector<window_algebra::Interval>);
}
std::size_t WindowStore::scratchIntervalBound(std::size_t query_edges) const {
    if (domain_->representation() == window_algebra::Representation::Dense) return 0;
    if (maximum_edge_intervals_ &&
        query_edges > std::numeric_limits<std::size_t>::max() / maximum_edge_intervals_)
        throw std::length_error("W scratch bound overflow");
    // Intersection endpoints come from some input edge; summing all query-edge
    // interval counts bounds every cumulative support without truncation.
    return std::max<std::size_t>(1, query_edges) * maximum_edge_intervals_;
}

void WindowStore::writeLoadJson(std::ostream& out) const {
    if (loaded_) wstore_file::writeLoadMetricsJson(out, *loaded_);
    else out << "null";
}
