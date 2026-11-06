#include "../CandidateDag.h"
#include "../GopherMatcher.h"
#include "../LabelProjection.h"
#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using MappingSet = std::set<std::vector<int>>;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string readFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "open fixture " + path.string());
    std::ostringstream buffer;
    buffer << input.rdbuf();
    require(!input.bad(), "read fixture " + path.string());
    return buffer.str();
}

void writeFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    require(output.is_open(), "open temporary artifact " + path.string());
    output << text;
    output.flush();
    require(output.good(), "write temporary artifact " + path.string());
}

MappingSet bruteForceExternalMappings(
    const Graph& graph,
    const Graph& query,
    int minimum_duration) {
    std::vector<int> mapping(
        static_cast<std::size_t>(query.num_vertices), -1);
    std::vector<std::uint8_t> used(
        static_cast<std::size_t>(graph.num_vertices), 0U);
    MappingSet result;
    std::function<void(int)> enumerate = [&](int query_vertex) {
        if (query_vertex < query.num_vertices) {
            const Label required = query.vertex_labels[
                static_cast<std::size_t>(query_vertex)];
            for (int data_vertex = 0;
                 data_vertex < graph.num_vertices;
                 ++data_vertex) {
                if (used[static_cast<std::size_t>(data_vertex)] != 0U ||
                    graph.vertex_labels[
                        static_cast<std::size_t>(data_vertex)] != required) {
                    continue;
                }
                mapping[static_cast<std::size_t>(query_vertex)] = data_vertex;
                used[static_cast<std::size_t>(data_vertex)] = 1U;
                enumerate(query_vertex + 1);
                used[static_cast<std::size_t>(data_vertex)] = 0U;
                mapping[static_cast<std::size_t>(query_vertex)] = -1;
            }
            return;
        }

        std::vector<TimeInterval> support;
        bool initialized = false;
        for (int source = 0; source < query.num_vertices; ++source) {
            for (const Edge& query_edge :
                 query.adj[static_cast<std::size_t>(source)]) {
                const TemporalEdge* data_edge = graph.findTemporalEdge(
                    mapping[static_cast<std::size_t>(source)],
                    mapping[static_cast<std::size_t>(query_edge.to)]);
                if (data_edge == nullptr) return;
                if (!initialized) {
                    for (const TimeInterval& interval :
                         data_edge->activeIntervals()) {
                        if (interval.length() >= minimum_duration) {
                            support.push_back(interval);
                        }
                    }
                    initialized = true;
                } else {
                    support = intersectTimeIntervals(
                        support,
                        data_edge->activeIntervals(),
                        minimum_duration);
                }
                if (support.empty()) return;
            }
        }
        if (!initialized || support.empty()) return;
        std::vector<int> external_mapping = mapping;
        for (int& data_vertex : external_mapping) {
            data_vertex = graph.externalId(data_vertex);
        }
        require(
            result.insert(std::move(external_mapping)).second,
            "independent oracle duplicate");
    };
    enumerate(0);
    return result;
}

MappingSet parseFullRows(const std::string& text, int query_vertices) {
    MappingSet result;
    std::istringstream input(text);
    std::string row;
    while (std::getline(input, row)) {
        if (row.rfind("Match ", 0) != 0U) continue;
        std::vector<int> mapping;
        std::size_t cursor = 0U;
        for (int query_vertex = 0;
             query_vertex < query_vertices;
             ++query_vertex) {
            const std::string marker =
                "q" + std::to_string(query_vertex) + "(";
            const std::size_t marker_position = row.find(marker, cursor);
            require(marker_position != std::string::npos, "missing query vertex in row");
            const std::size_t arrow = row.find(")->", marker_position);
            require(arrow != std::string::npos, "missing mapping arrow in row");
            const std::size_t value_start = arrow + 3U;
            const std::size_t value_end =
                row.find_first_of(", ", value_start);
            require(value_end != std::string::npos, "missing mapped vertex in row");
            const std::string value =
                row.substr(value_start, value_end - value_start);
            std::size_t parsed = 0U;
            const int external_id = std::stoi(value, &parsed);
            require(parsed == value.size(), "malformed mapped vertex in row");
            mapping.push_back(external_id);
            cursor = value_end;
        }
        require(result.insert(std::move(mapping)).second, "duplicate full-output row");
    }
    return result;
}

QueryDecomposition decompose(const Graph& graph, const Graph& query) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<long long, kLabelCount> duration_sums{};
    for (std::size_t vertex = 0; vertex < graph.vertex_labels.size(); ++vertex) {
        const Label label = graph.vertex_labels[vertex];
        if (label >= kLabelCount) continue;
        ++counts[label];
        duration_sums[label] += graph.vertex_active_durations[vertex];
    }
    std::array<double, kLabelCount> mean_activity{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        if (counts[label] != 0U) {
            mean_activity[label] =
                static_cast<double>(duration_sums[label]) /
                static_cast<double>(counts[label]);
        }
    }
    return decomposeQuery(
        query, counts, mean_activity, SelectivityMode::Paper);
}

void testProjectionAndExecutorParity(const std::filesystem::path& fixture_dir) {
    const std::filesystem::path data =
        fixture_dir / "label_projection_generic_c.txt";
    const std::filesystem::path query_file =
        fixture_dir / "label_projection_generic_c.qry";
    const std::filesystem::path global_map =
        fixture_dir / "label_projection_global20.json";
    const std::filesystem::path projection =
        fixture_dir / "label_projection_generic_c.json";

    ExplicitRuntimeLabelMap labels;
    std::string error;
    require(
        loadExplicitRuntimeLabelMap(
            global_map.string(),
            projection.string(),
            query_file.string(),
            labels,
            error),
        "load projection: " + error);
    require(labels.global_label_cardinality == 20U, "global cardinality receipt");
    require(
        labels.selected_global_classes == std::vector<std::uint32_t>({1U, 2U, 3U}),
        "selected global classes receipt");
    require(
        labels.runtime_class_counts ==
            std::array<std::size_t, kLabelCount>({2U, 2U, 2U, 0U, 0U}),
        "projected runtime counts receipt");
    require(labels.invalid_vertex_count == 2U, "invalid projection count receipt");

    Graph graph;
    require(
        readTemporalGraph(
            data.string(),
            graph,
            kDefaultLabelSeed,
            nullptr,
            LabelMode::ExplicitProjection,
            &labels),
        "load graph with explicit projection");
    Graph query;
    require(readQueryGraph(query_file.string(), query), "load projected query");

    const auto internal = [&](int external_id) {
        const auto found = std::lower_bound(
            graph.external_ids.begin(), graph.external_ids.end(), external_id);
        require(found != graph.external_ids.end() && *found == external_id,
                "expected external vertex");
        return static_cast<std::size_t>(found - graph.external_ids.begin());
    };
    require(graph.vertex_labels[internal(10)] == labelFromString("A"), "class 1 -> A");
    require(graph.vertex_labels[internal(15)] == labelFromString("B"), "class 2 -> B");
    require(graph.vertex_labels[internal(12)] == labelFromString("C"), "class 3 -> C");
    require(graph.vertex_labels[internal(11)] == kInvalidLabel, "unselected class -> invalid");

    constexpr int minimum_duration = 3;
    const MappingSet oracle =
        bruteForceExternalMappings(graph, query, minimum_duration);
    require(oracle.size() == 6U, "independent known oracle count");
    const QueryDecomposition decomposition = decompose(graph, query);
    require(decomposition.connected, "projected query connected");
    const TDTree tree(graph, query, decomposition, minimum_duration);
    const QueryPlan plan = makeQueryPlan(
        query, decomposition, tree, OrderMode::Paper);
    CandidateDagCompiler compiler;
    const CandidateDag dag = compiler.compile(query, plan, decomposition);

    GopherMatcherOptions options;
    options.threads = 1U;
    options.parallel_mode = ParallelMode::Roots;
    options.order_mode = OrderMode::Paper;
    options.executor.memo_mode = MemoMode::None;
    options.executor.collect_statistics = false;
    std::ostringstream direct_rows;
    std::ostringstream dag_rows;
    GopherMatcher direct(
        graph, query, plan, tree, minimum_duration, options);
    GopherMatcher dag_no_memo(
        graph, query, plan, dag, tree, minimum_duration, options);
    const GopherMatchSummary direct_summary =
        direct.run(MatchOutputMode::Full, &direct_rows);
    const GopherMatchSummary dag_summary =
        dag_no_memo.run(MatchOutputMode::Full, &dag_rows);

    require(direct_summary.match_count == oracle.size(), "DirectDFS/oracle count");
    require(dag_summary.match_count == oracle.size(), "DAG-NoMemo/oracle count");
    require(!direct_summary.count_overflow && !dag_summary.count_overflow,
            "fixture counts do not overflow");
    require(parseFullRows(direct_rows.str(), query.num_vertices) == oracle,
            "DirectDFS full embedding set equals independent oracle");
    require(parseFullRows(dag_rows.str(), query.num_vertices) == oracle,
            "DAG-NoMemo full embedding set equals independent oracle");
    require(direct_rows.str() == dag_rows.str(),
            "DirectDFS and DAG-NoMemo deterministic rows are byte-identical");

    ExplicitRuntimeLabelMap missing = labels;
    missing.labels_by_external_id.pop_back();
    Graph rejected_graph;
    std::ostringstream rejected_diagnostic;
    std::streambuf* const previous_error_buffer =
        std::cerr.rdbuf(rejected_diagnostic.rdbuf());
    const bool coverage_accepted = readTemporalGraph(
        data.string(),
        rejected_graph,
        kDefaultLabelSeed,
        nullptr,
        LabelMode::ExplicitProjection,
        &missing);
    std::cerr.rdbuf(previous_error_buffer);
    require(
        !coverage_accepted,
        "raw graph coverage mismatch is rejected");
    require(
        rejected_diagnostic.str().find("does not cover") != std::string::npos,
        "raw graph coverage mismatch diagnostic");
}

void testStrictProjectionIdentity(const std::filesystem::path& fixture_dir) {
    const std::filesystem::path query_file =
        fixture_dir / "label_projection_generic_c.qry";
    const std::filesystem::path global_map =
        fixture_dir / "label_projection_global20.json";
    const std::filesystem::path projection =
        fixture_dir / "label_projection_generic_c.json";
    std::string malformed = readFile(projection);
    malformed.insert(malformed.find('{') + 1U, "\n  \"unknown\": 1,");
    const std::filesystem::path temporary =
        std::filesystem::temp_directory_path() /
        "dsm_label_projection_unknown_key.json";
    writeFile(temporary, malformed);

    ExplicitRuntimeLabelMap labels;
    std::string error;
    require(
        !loadExplicitRuntimeLabelMap(
            global_map.string(),
            temporary.string(),
            query_file.string(),
            labels,
            error),
        "unknown projection field is rejected");
    require(error.find("unknown query-label-projection key") != std::string::npos,
            "unknown projection field diagnostic");
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);

    require(
        !loadExplicitRuntimeLabelMap(
            global_map.string(),
            projection.string(),
            (fixture_dir / "label_projection_generic_c.txt").string(),
            labels,
            error),
        "query identity mismatch is rejected");
    require(error.find("different query file sha256") != std::string::npos,
            "query identity diagnostic");
}

} // namespace

int main(int argc, char* argv[]) {
    try {
        require(argc == 2, "usage: test_label_projection <fixture-dir>");
        const std::filesystem::path fixture_dir = argv[1];
        testProjectionAndExecutorParity(fixture_dir);
        testStrictProjectionIdentity(fixture_dir);
        std::cout << "Label-projection focused tests passed.\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Test failure: " << exception.what() << '\n';
        return 1;
    }
}
