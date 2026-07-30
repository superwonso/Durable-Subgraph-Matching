#include "../ours/Utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string data_path;
    std::string query_path;
    int center_count = 5000;
    int cold_neighbors_per_label = 2;
    int hot_neighbors_per_label = 64;
    int durable_run_length = 3;
    std::uint32_t label_seed = kDefaultLabelSeed;
};

int parsePositive(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size() || value <= 0 || value > 100000000) {
        throw std::invalid_argument(name);
    }
    return static_cast<int>(value);
}

std::vector<Label> assignLabels(
    int vertex_count,
    std::uint32_t seed) {
    std::vector<Label> labels(static_cast<std::size_t>(vertex_count));
    std::mt19937 generator(seed);
    std::uniform_int_distribution<int> distribution(
        0, static_cast<int>(kLabelCount) - 1);
    for (auto& label : labels) {
        label = static_cast<Label>(distribution(generator));
    }
    return labels;
}

std::array<std::vector<int>, kLabelCount> makePools(
    int required_centers,
    int required_leaves,
    std::uint32_t seed) {
    int universe_size =
        std::max(4096, (required_centers + required_leaves * 4) * 8);
    for (;;) {
        const auto labels = assignLabels(universe_size, seed);
        std::array<std::vector<int>, kLabelCount> pools;
        for (int vertex = 0; vertex < universe_size; ++vertex) {
            pools[labels[static_cast<std::size_t>(vertex)]].push_back(vertex);
        }
        bool enough =
            pools[labelFromString("B")].size() >=
            static_cast<std::size_t>(required_centers);
        for (const std::string label_text : {"A", "C", "D", "E"}) {
            enough &=
                pools[labelFromString(label_text)].size() >=
                static_cast<std::size_t>(required_leaves);
        }
        if (enough) return pools;
        if (universe_size >
            std::numeric_limits<int>::max() / 2) {
            throw std::overflow_error("label universe is too large");
        }
        universe_size *= 2;
    }
}

void writeRun(
    std::ofstream& output,
    int source,
    int target,
    int first,
    int last) {
    for (int snapshot = first; snapshot <= last; ++snapshot) {
        output << source << ' ' << target << ' ' << snapshot << '\n';
    }
}

void writeQuery(const std::string& path) {
    std::ofstream output(path);
    if (!output.is_open()) {
        throw std::runtime_error("cannot open query output");
    }
    output << "center:B leaf_a:A\n"
           << "center:B leaf_c:C\n"
           << "center:B leaf_d:D\n"
           << "center:B leaf_e:E\n";
}

std::uint64_t fourthPower(std::uint64_t value) {
    if (value != 0 &&
        value > std::numeric_limits<std::uint64_t>::max() / value) {
        throw std::overflow_error("expected match count overflow");
    }
    const std::uint64_t square = value * value;
    if (square != 0 &&
        square > std::numeric_limits<std::uint64_t>::max() / square) {
        throw std::overflow_error("expected match count overflow");
    }
    return square * square;
}

void generate(const Options& options) {
    if (options.center_count < 2 ||
        options.cold_neighbors_per_label >
            options.hot_neighbors_per_label ||
        options.durable_run_length < 3) {
        throw std::invalid_argument("invalid skewed workload dimensions");
    }

    const auto pools = makePools(
        options.center_count,
        options.hot_neighbors_per_label,
        options.label_seed);
    const std::array<Label, 4> leaf_labels{{
        labelFromString("A"),
        labelFromString("C"),
        labelFromString("D"),
        labelFromString("E")}};
    const auto& centers = pools[labelFromString("B")];

    std::ofstream output(options.data_path);
    if (!output.is_open()) {
        throw std::runtime_error("cannot open data output");
    }

    // Keep the loader's seeded label universe deterministic. These two-snapshot
    // self-loops are removed by the k >= 3 temporal prefilter.
    std::size_t universe_size = 0;
    for (const auto& pool : pools) universe_size += pool.size();
    for (std::size_t vertex = 0; vertex < universe_size; ++vertex) {
        writeRun(
            output,
            static_cast<int>(vertex),
            static_cast<int>(vertex),
            60,
            61);
    }

    for (int center_index = 0;
         center_index < options.center_count;
         ++center_index) {
        const int neighbor_count =
            center_index == 0
            ? options.hot_neighbors_per_label
            : options.cold_neighbors_per_label;
        const int center =
            centers[static_cast<std::size_t>(center_index)];
        for (std::size_t group = 0;
             group < leaf_labels.size();
             ++group) {
            const auto& leaves = pools[leaf_labels[group]];
            for (int offset = 0; offset < neighbor_count; ++offset) {
                const int leaf_index =
                    (center_index * 7 +
                     offset +
                     static_cast<int>(group) * 5) %
                    options.hot_neighbors_per_label;
                writeRun(
                    output,
                    center,
                    leaves[static_cast<std::size_t>(leaf_index)],
                    1,
                    options.durable_run_length);
            }
        }
    }
    if (!output.good()) {
        throw std::runtime_error("failed writing data output");
    }
    writeQuery(options.query_path);

    const std::uint64_t hot_matches =
        fourthPower(
            static_cast<std::uint64_t>(
                options.hot_neighbors_per_label));
    const std::uint64_t cold_matches =
        fourthPower(
            static_cast<std::uint64_t>(
                options.cold_neighbors_per_label));
    const std::uint64_t expected =
        hot_matches +
        static_cast<std::uint64_t>(options.center_count - 1) *
            cold_matches;
    std::cout
        << "Generated skewed reuse-star graph at "
        << options.data_path << '\n'
        << "hot_roots=1 cold_roots="
        << (options.center_count - 1)
        << " expected_matches=" << expected << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 8) {
        std::cerr
            << "Usage: " << argv[0]
            << " <data-output> <query-output> [centers]"
               " [cold-neighbors] [hot-neighbors]"
               " [durable-run-length] [label-seed]\n";
        return 1;
    }

    Options options;
    options.data_path = argv[1];
    options.query_path = argv[2];
    try {
        if (argc >= 4) {
            options.center_count =
                parsePositive(argv[3], "centers");
        }
        if (argc >= 5) {
            options.cold_neighbors_per_label =
                parsePositive(argv[4], "cold neighbors");
        }
        if (argc >= 6) {
            options.hot_neighbors_per_label =
                parsePositive(argv[5], "hot neighbors");
        }
        if (argc >= 7) {
            options.durable_run_length =
                parsePositive(argv[6], "durable run length");
        }
        if (argc >= 8) {
            options.label_seed = static_cast<std::uint32_t>(
                parsePositive(argv[7], "label seed"));
        }
        generate(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    }
    return 0;
}
