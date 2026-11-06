#include "../TDTree.h"
#include "../Utils.h"
#include "../query_decomposition.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void addTemporalEdge(
    Graph& graph,
    int source,
    int target,
    std::vector<TimeInterval> intervals) {
    std::int64_t active_snapshot_count = 0;
    for (const TimeInterval& interval : intervals) {
        active_snapshot_count += interval.length();
    }
    const int temporal_edge_id =
        static_cast<int>(graph.temporal_edges.size());
    graph.temporal_edges.emplace_back(
        source,
        target,
        std::move(intervals),
        active_snapshot_count);
    graph.adj[static_cast<std::size_t>(source)].push_back(
        {target, temporal_edge_id});
    graph.in_adj[static_cast<std::size_t>(target)].push_back(
        {source, temporal_edge_id});
}

Graph makeData(bool mutate_relation) {
    Graph graph;
    graph.num_vertices = mutate_relation ? 5 : 4;
    graph.external_ids = mutate_relation
        ? std::vector<int>{10, 20, 30, 40, 50}
        : std::vector<int>{10, 20, 30, 40};
    graph.vertex_labels = mutate_relation
        ? std::vector<Label>{
              labelFromString("A"), labelFromString("B"),
              labelFromString("A"), labelFromString("B"),
              labelFromString("B")}
        : std::vector<Label>{
              labelFromString("A"), labelFromString("B"),
              labelFromString("A"), labelFromString("B")};
    graph.adj.resize(static_cast<std::size_t>(graph.num_vertices));
    graph.in_adj.resize(static_cast<std::size_t>(graph.num_vertices));
    addTemporalEdge(graph, 0, 1, {{1, 4}});
    addTemporalEdge(graph, 2, 3, {{1, 4}});
    if (mutate_relation) addTemporalEdge(graph, 0, 4, {{1, 4}});

    graph.vertex_active_durations.assign(
        static_cast<std::size_t>(graph.num_vertices), 4);
    graph.neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    graph.incoming_neighbor_label_counts.assign(
        static_cast<std::size_t>(graph.num_vertices), {});
    for (const TemporalEdge& edge : graph.temporal_edges) {
        const Label target_label =
            graph.vertex_labels[static_cast<std::size_t>(edge.v)];
        const Label source_label =
            graph.vertex_labels[static_cast<std::size_t>(edge.u)];
        ++graph.neighbor_label_counts[static_cast<std::size_t>(edge.u)]
            [target_label];
        ++graph.incoming_neighbor_label_counts[
            static_cast<std::size_t>(edge.v)][source_label];
    }
    return graph;
}

Graph makeQuery() {
    Graph query;
    query.num_vertices = 2;
    query.external_ids = {0, 1};
    query.vertex_labels = {
        labelFromString("A"), labelFromString("B")};
    query.adj.resize(2);
    query.in_adj.resize(2);
    query.adj[0].push_back({1, -1});
    query.in_adj[1].push_back({0, -1});
    return query;
}

QueryDecomposition makePlan(const Graph& query) {
    std::array<std::size_t, kLabelCount> counts{};
    std::array<double, kLabelCount> activity{};
    counts[labelFromString("A")] = 2;
    counts[labelFromString("B")] = 2;
    activity[labelFromString("A")] = 4.0;
    activity[labelFromString("B")] = 4.0;
    return decomposeQuery(
        query, counts, activity, SelectivityMode::Paper);
}

} // namespace

int main() {
    try {
        const Graph data = makeData(false);
        const Graph query = makeQuery();
        const QueryDecomposition plan = makePlan(query);
        TDTree tree(data, query, plan, 3);
        TDTree identical(data, query, plan, 3);
        TDTree stricter(data, query, plan, 4);
        const Graph changed_data = makeData(true);
        TDTree changed(changed_data, query, plan, 3);

        const TDTreeCapacityReceipt capacity = tree.capacityReceipt();
        const TDTreeCapacityReceipt identical_capacity =
            identical.capacityReceipt();
        require(
            capacity.complete && !capacity.overflowed &&
                capacity.invariantHolds() &&
                capacity.total_bytes > sizeof(TDTree) &&
                capacity.total_bytes == tree.getMemoryUsage() &&
                capacity.query_out_requirements_count <=
                    capacity.query_out_requirements_capacity &&
                capacity.query_in_requirements_count <=
                    capacity.query_in_requirements_capacity &&
                capacity.decomposition_spanning_outer_count <=
                    capacity.decomposition_spanning_outer_capacity &&
                capacity.decomposition_spanning_nested_count <=
                    capacity.decomposition_spanning_nested_capacity &&
                capacity.decomposition_parent_count <=
                    capacity.decomposition_parent_capacity &&
                tree.hasOnlySortedFlatBlocks(),
            "normalized TD-tree has one complete exact flat capacity receipt");

        TDTreeCapacityReceipt tampered_requirements = capacity;
        const std::uint64_t requirement_width =
            sizeof(std::array<int, kLabelCount>);
        tampered_requirements.query_out_requirements_bytes +=
            requirement_width;
        tampered_requirements.dynamic_bytes += requirement_width;
        tampered_requirements.total_bytes += requirement_width;
        require(
            !tampered_requirements.invariantHolds(),
            "coordinated requirement-byte/subtotal tamper is rejected");

        TDTreeCapacityReceipt tampered_parent = capacity;
        const std::uint64_t parent_width = sizeof(int);
        tampered_parent.decomposition_parent_bytes += parent_width;
        tampered_parent.query_decomposition_bytes += parent_width;
        tampered_parent.dynamic_bytes += parent_width;
        tampered_parent.total_bytes += parent_width;
        require(
            !tampered_parent.invariantHolds(),
            "coordinated decomposition-byte/subtotal tamper is rejected");

        TDTreeCapacityReceipt tampered_nested = capacity;
        tampered_nested.decomposition_spanning_nested_capacity += 1;
        require(
            !tampered_nested.invariantHolds(),
            "nested spanning capacity mutation is independently rejected");
        require(
            identical_capacity.complete &&
                identical_capacity.invariantHolds() &&
                identical_capacity.total_bytes == capacity.total_bytes &&
                identical_capacity.dynamic_bytes == capacity.dynamic_bytes &&
                identical_capacity.nodes_bytes == capacity.nodes_bytes &&
                identical_capacity.root_candidates_bytes ==
                    capacity.root_candidates_bytes &&
                identical_capacity.blocks_bytes == capacity.blocks_bytes &&
                identical_capacity.block_candidates_bytes ==
                    capacity.block_candidates_bytes &&
                identical_capacity.query_decomposition_bytes ==
                    capacity.query_decomposition_bytes &&
                identical_capacity.node_capacity == capacity.node_capacity &&
                identical_capacity.root_candidate_capacity ==
                    capacity.root_candidate_capacity &&
                identical_capacity.block_capacity == capacity.block_capacity &&
                identical_capacity.block_candidate_capacity ==
                    capacity.block_candidate_capacity,
            "identical TD-tree builds have exact deterministic capacity totals");

        const std::filesystem::path one_path =
            std::filesystem::temp_directory_path() /
            "dsm_tdtree_digest_1.txt";
        const std::filesystem::path many_path =
            std::filesystem::temp_directory_path() /
            "dsm_tdtree_digest_16.txt";
        const MatchSummary one = tree.save_res(
            one_path.string(), MatchOutputMode::CountOnly, 1, true);
        const MatchSummary many = tree.save_res(
            many_path.string(), MatchOutputMode::CountOnly, 16, true);
        require(
            one.output_written && many.output_written &&
                !one.count_overflow && !many.count_overflow &&
                one.match_count == 2 && many.match_count == 2 &&
                one.result_digest_collected &&
                many.result_digest_collected &&
                one.mapping_digest_xor == many.mapping_digest_xor &&
                one.mapping_digest_sum == many.mapping_digest_sum,
            "mapping digest is deterministic and thread-order independent");

        const std::uint64_t content =
            tree.candidateContentFingerprint();
        require(
            content != 0 &&
                identical.candidateContentFingerprint() == content &&
                stricter.candidateContentFingerprint() != content &&
                changed.candidateContentFingerprint() != content,
            "candidate content witness binds k and relation contents");
        std::filesystem::remove(one_path);
        std::filesystem::remove(many_path);
        std::cout << "TDTree exactness receipt tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
