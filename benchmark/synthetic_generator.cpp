#include "../ours/Utils.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

struct Options {
    std::string data_path;
    std::string query_path;
    int center_count = 500;
    int leaves_per_label = 20;
    int neighbors_per_label = 8;
    int transient_edges = 10000;
    std::uint32_t label_seed = kDefaultLabelSeed;
};

int parseNonNegative(const std::string& text, const char* name) {
    std::size_t consumed = 0;
    const long long value = std::stoll(text, &consumed);
    if (consumed != text.size() || value < 0 || value > 100000000) {
        throw std::invalid_argument(name);
    }
    return static_cast<int>(value);
}

std::vector<Label> assignLabels(int vertex_count, std::uint32_t seed) {
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
    int required_center_count,
    int required_leaf_count,
    std::uint32_t seed) {
    int universe_size =
        std::max(4096, (required_center_count + required_leaf_count * 4) * 8);
    for (;;) {
        const auto labels = assignLabels(universe_size, seed);
        std::array<std::vector<int>, kLabelCount> pools;
        for (int vertex = 0; vertex < universe_size; ++vertex) {
            pools[labels[static_cast<std::size_t>(vertex)]].push_back(vertex);
        }
        const bool enough_centers =
            pools[labelFromString("B")].size() >=
            static_cast<std::size_t>(required_center_count);
        bool enough_leaves = true;
        for (const std::string label_text : {"A", "C", "D", "E"}) {
            enough_leaves &=
                pools[labelFromString(label_text)].size() >=
                static_cast<std::size_t>(required_leaf_count);
        }
        if (enough_centers && enough_leaves) return pools;
        universe_size *= 2;
    }
}

void writeRun(std::ofstream& output, int source, int target, int first, int last) {
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

void generate(const Options& options) {
    if (options.center_count <= 0 || options.leaves_per_label <= 0 ||
        options.neighbors_per_label <= 0 ||
        options.neighbors_per_label > options.leaves_per_label) {
        throw std::invalid_argument("invalid pool sizes");
    }

    const auto pools = makePools(
        options.center_count, options.leaves_per_label, options.label_seed);
    const std::array<Label, 4> leaf_labels{{
        labelFromString("A"),
        labelFromString("C"),
        labelFromString("D"),
        labelFromString("E")}};
    const Label center_label = labelFromString("B");

    std::ofstream output(options.data_path);
    if (!output.is_open()) {
        throw std::runtime_error("cannot open data output");
    }

    // Include the complete sorted ID universe so the loader's seeded label
    // assignment is exactly the one used above. Two-snapshot self-loops survive
    // Algorithm 3.1 but are too short to be candidates when k >= 3.
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

    for (int center_index = 0; center_index < options.center_count; ++center_index) {
        const int center =
            pools[center_label][static_cast<std::size_t>(center_index)];
        for (std::size_t group = 0; group < leaf_labels.size(); ++group) {
            const auto& leaves = pools[leaf_labels[group]];
            for (int offset = 0; offset < options.neighbors_per_label; ++offset) {
                const int leaf_index =
                    (center_index + offset * 3) % options.leaves_per_label;
                const int leaf = leaves[static_cast<std::size_t>(leaf_index)];

                // Most choices share [3,5]. A minority remain individually
                // durable but have a shifted run, producing safe temporal
                // pruning opportunities for multi-leaf partial mappings.
                const bool shifted =
                    ((center_index + offset + static_cast<int>(group)) % 11) == 0;
                if (shifted && group >= 2) {
                    writeRun(output, center, leaf, 40, 50);
                } else {
                    const int first = group == 0 ? 1 : (group == 1 ? 2 : 3);
                    writeRun(output, center, leaf, first, first + 29);
                }
            }
        }
    }

    // One-snapshot noise exercises Algorithm 3.1 without changing the active
    // label universe after filtering. Endpoints are selected from active pools.
    std::mt19937 generator(options.label_seed + 991U);
    std::uniform_int_distribution<int> center_pick(0, options.center_count - 1);
    std::uniform_int_distribution<int> label_pick(0, 3);
    std::uniform_int_distribution<int> leaf_pick(0, options.leaves_per_label - 1);
    for (int i = 0; i < options.transient_edges; ++i) {
        const int source =
            pools[center_label][static_cast<std::size_t>(center_pick(generator))];
        const Label target_label =
            leaf_labels[static_cast<std::size_t>(label_pick(generator))];
        const int target =
            pools[target_label][static_cast<std::size_t>(leaf_pick(generator))];
        output << target << ' ' << source << ' ' << (100 + i % 17) << '\n';
    }
    if (!output.good()) throw std::runtime_error("failed writing data output");
    writeQuery(options.query_path);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3 || argc > 8) {
        std::cerr
            << "Usage: " << argv[0]
            << " <data-output> <query-output> [centers] [leaves-per-label]"
               " [neighbors-per-label] [transient-edges] [label-seed]\n";
        return 1;
    }

    Options options;
    options.data_path = argv[1];
    options.query_path = argv[2];
    try {
        if (argc >= 4) options.center_count = parseNonNegative(argv[3], "centers");
        if (argc >= 5) {
            options.leaves_per_label =
                parseNonNegative(argv[4], "leaves per label");
        }
        if (argc >= 6) {
            options.neighbors_per_label =
                parseNonNegative(argv[5], "neighbors per label");
        }
        if (argc >= 7) {
            options.transient_edges =
                parseNonNegative(argv[6], "transient edges");
        }
        if (argc >= 8) {
            options.label_seed =
                static_cast<std::uint32_t>(parseNonNegative(argv[7], "label seed"));
        }
        generate(options);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 2;
    }

    std::cout << "Generated reuse-star temporal graph at " << options.data_path
              << " and query at " << options.query_path << '\n';
    return 0;
}
