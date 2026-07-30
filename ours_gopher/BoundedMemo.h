#ifndef BOUNDED_MEMO_H
#define BOUNDED_MEMO_H

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

struct MemoOptions {
    std::size_t budget_bytes = 256ULL << 20;
    std::size_t max_entry_bytes = 8ULL << 20;
    std::size_t shard_count = 64;
    std::size_t minimum_estimated_work = 128;
    bool cost_aware_admission = true;
    bool collect_statistics = true;
};

struct MemoSnapshot {
    std::uint64_t hits = 0;
    std::uint64_t misses = 0;
    std::uint64_t admitted = 0;
    std::uint64_t bypassed = 0;
    std::uint64_t duplicate_inserts = 0;
    std::uint64_t entries = 0;
    std::uint64_t bytes = 0;
    std::uint64_t rejected_budget = 0;
    std::uint64_t rejected_entry_size = 0;
    std::uint64_t rejected_cost = 0;
    std::uint64_t hit_value_elements = 0;
    std::uint64_t hit_saved_work = 0;
    std::uint64_t max_hit_value_elements = 0;
    std::uint64_t admitted_value_elements = 0;
    std::uint64_t admitted_estimated_work = 0;
};

template <typename Key, typename Value, typename Hash>
class BoundedMemo {
public:
    explicit BoundedMemo(MemoOptions options = {})
        : options_(std::move(options)) {
        const std::size_t shard_count = std::max<std::size_t>(1, options_.shard_count);
        shards_.reserve(shard_count);
        for (std::size_t i = 0; i < shard_count; ++i) {
            shards_.push_back(std::make_unique<Shard>());
        }
    }

    std::shared_ptr<const Value> find(const Key& key) {
        const auto start_hash = hasher_(key);
        Shard& shard = *shards_[start_hash % shards_.size()];
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.values.find(key);
        if (found == shard.values.end()) {
            if (options_.collect_statistics) {
                misses_.fetch_add(1, std::memory_order_relaxed);
            }
            return {};
        }
        if (options_.collect_statistics) {
            hits_.fetch_add(1, std::memory_order_relaxed);
            hit_value_elements_.fetch_add(
                found->second.value_elements,
                std::memory_order_relaxed);
            hit_saved_work_.fetch_add(
                found->second.estimated_work,
                std::memory_order_relaxed);
            std::uint64_t current_max =
                max_hit_value_elements_.load(
                    std::memory_order_relaxed);
            while (current_max <
                       found->second.value_elements &&
                   !max_hit_value_elements_.compare_exchange_weak(
                       current_max,
                       found->second.value_elements,
                       std::memory_order_relaxed,
                       std::memory_order_relaxed)) {
            }
        }
        return found->second.value;
    }

    std::shared_ptr<const Value> insertOrGet(
        const Key& key,
        std::shared_ptr<const Value> value,
        std::size_t value_bytes,
        std::size_t estimated_work,
        std::size_t dynamic_key_bytes = 0,
        std::size_t value_elements = 0) {
        if (!value) {
            if (options_.collect_statistics) {
                bypassed_.fetch_add(1, std::memory_order_relaxed);
            }
            return value;
        }
        if (options_.budget_bytes == 0) {
            if (options_.collect_statistics) {
                bypassed_.fetch_add(1, std::memory_order_relaxed);
                rejected_budget_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return value;
        }
        if (value_bytes > options_.max_entry_bytes) {
            if (options_.collect_statistics) {
                bypassed_.fetch_add(1, std::memory_order_relaxed);
                rejected_entry_size_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return value;
        }
        if (options_.cost_aware_admission &&
            estimated_work < options_.minimum_estimated_work) {
            recordCostBypass();
            return value;
        }

        // Include the map node, shared ownership metadata, the value object,
        // and caller-reported heap storage owned by dynamic keys/values. This
        // is deliberately conservative so the configured cache limit remains
        // a hard upper bound on admitted materializations.
        const std::size_t fixed_overhead =
            sizeof(Key) + sizeof(Entry) + sizeof(Value) + sizeof(void*) * 4;
        if (dynamic_key_bytes > options_.budget_bytes ||
            value_bytes > options_.budget_bytes ||
            fixed_overhead > options_.budget_bytes ||
            dynamic_key_bytes > options_.budget_bytes - fixed_overhead ||
            value_bytes >
                options_.budget_bytes - fixed_overhead - dynamic_key_bytes) {
            if (options_.collect_statistics) {
                bypassed_.fetch_add(1, std::memory_order_relaxed);
                rejected_entry_size_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return value;
        }
        const std::size_t entry_bytes =
            fixed_overhead + dynamic_key_bytes + value_bytes;
        if (entry_bytes > options_.max_entry_bytes ||
            entry_bytes > options_.budget_bytes) {
            if (options_.collect_statistics) {
                bypassed_.fetch_add(1, std::memory_order_relaxed);
                rejected_entry_size_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return value;
        }

        const auto key_hash = hasher_(key);
        Shard& shard = *shards_[key_hash % shards_.size()];
        std::lock_guard<std::mutex> lock(shard.mutex);
        const auto found = shard.values.find(key);
        if (found != shard.values.end()) {
            if (options_.collect_statistics) {
                duplicate_inserts_.fetch_add(
                    1, std::memory_order_relaxed);
            }
            return found->second.value;
        }

        std::size_t current = bytes_.load(std::memory_order_relaxed);
        while (true) {
            if (current > options_.budget_bytes - entry_bytes) {
                if (options_.collect_statistics) {
                    bypassed_.fetch_add(
                        1, std::memory_order_relaxed);
                    rejected_budget_.fetch_add(
                        1, std::memory_order_relaxed);
                }
                return value;
            }
            if (bytes_.compare_exchange_weak(
                    current,
                    current + entry_bytes,
                    std::memory_order_relaxed,
                    std::memory_order_relaxed)) {
                break;
            }
        }

        try {
            shard.values.emplace(
                key,
                Entry{
                    std::move(value),
                    entry_bytes,
                    value_elements,
                    estimated_work});
        } catch (...) {
            bytes_.fetch_sub(entry_bytes, std::memory_order_relaxed);
            throw;
        }
        if (options_.collect_statistics) {
            admitted_.fetch_add(1, std::memory_order_relaxed);
            entries_.fetch_add(1, std::memory_order_relaxed);
            admitted_value_elements_.fetch_add(
                value_elements, std::memory_order_relaxed);
            admitted_estimated_work_.fetch_add(
                estimated_work, std::memory_order_relaxed);
        }
        return shard.values.find(key)->second.value;
    }

    MemoSnapshot snapshot() const {
        MemoSnapshot result;
        result.hits = hits_.load(std::memory_order_relaxed);
        result.misses = misses_.load(std::memory_order_relaxed);
        result.admitted = admitted_.load(std::memory_order_relaxed);
        result.bypassed = bypassed_.load(std::memory_order_relaxed);
        result.duplicate_inserts =
            duplicate_inserts_.load(std::memory_order_relaxed);
        result.entries = entries_.load(std::memory_order_relaxed);
        result.bytes = bytes_.load(std::memory_order_relaxed);
        result.rejected_budget =
            rejected_budget_.load(std::memory_order_relaxed);
        result.rejected_entry_size =
            rejected_entry_size_.load(std::memory_order_relaxed);
        result.rejected_cost =
            rejected_cost_.load(std::memory_order_relaxed);
        result.hit_value_elements =
            hit_value_elements_.load(std::memory_order_relaxed);
        result.hit_saved_work =
            hit_saved_work_.load(std::memory_order_relaxed);
        result.max_hit_value_elements =
            max_hit_value_elements_.load(std::memory_order_relaxed);
        result.admitted_value_elements =
            admitted_value_elements_.load(std::memory_order_relaxed);
        result.admitted_estimated_work =
            admitted_estimated_work_.load(
                std::memory_order_relaxed);
        return result;
    }

    void recordCostBypass() {
        if (options_.collect_statistics) {
            bypassed_.fetch_add(1, std::memory_order_relaxed);
            rejected_cost_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void clear() {
        for (const auto& shard_ptr : shards_) {
            std::lock_guard<std::mutex> lock(shard_ptr->mutex);
            shard_ptr->values.clear();
        }
        hits_.store(0, std::memory_order_relaxed);
        misses_.store(0, std::memory_order_relaxed);
        admitted_.store(0, std::memory_order_relaxed);
        bypassed_.store(0, std::memory_order_relaxed);
        duplicate_inserts_.store(0, std::memory_order_relaxed);
        entries_.store(0, std::memory_order_relaxed);
        bytes_.store(0, std::memory_order_relaxed);
        rejected_budget_.store(0, std::memory_order_relaxed);
        rejected_entry_size_.store(0, std::memory_order_relaxed);
        rejected_cost_.store(0, std::memory_order_relaxed);
        hit_value_elements_.store(0, std::memory_order_relaxed);
        hit_saved_work_.store(0, std::memory_order_relaxed);
        max_hit_value_elements_.store(0, std::memory_order_relaxed);
        admitted_value_elements_.store(0, std::memory_order_relaxed);
        admitted_estimated_work_.store(0, std::memory_order_relaxed);
    }

private:
    struct Entry {
        std::shared_ptr<const Value> value;
        std::size_t bytes = 0;
        std::size_t value_elements = 0;
        std::size_t estimated_work = 0;
    };

    struct Shard {
        std::mutex mutex;
        std::unordered_map<Key, Entry, Hash> values;
    };

    MemoOptions options_;
    Hash hasher_;
    std::vector<std::unique_ptr<Shard>> shards_;
    std::atomic<std::uint64_t> hits_{0};
    std::atomic<std::uint64_t> misses_{0};
    std::atomic<std::uint64_t> admitted_{0};
    std::atomic<std::uint64_t> bypassed_{0};
    std::atomic<std::uint64_t> duplicate_inserts_{0};
    std::atomic<std::uint64_t> entries_{0};
    std::atomic<std::size_t> bytes_{0};
    std::atomic<std::uint64_t> rejected_budget_{0};
    std::atomic<std::uint64_t> rejected_entry_size_{0};
    std::atomic<std::uint64_t> rejected_cost_{0};
    std::atomic<std::uint64_t> hit_value_elements_{0};
    std::atomic<std::uint64_t> hit_saved_work_{0};
    std::atomic<std::uint64_t> max_hit_value_elements_{0};
    std::atomic<std::uint64_t> admitted_value_elements_{0};
    std::atomic<std::uint64_t> admitted_estimated_work_{0};
};

#endif
