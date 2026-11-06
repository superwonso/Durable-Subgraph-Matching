#include "Utils.h"
#include "v3_canonical_json.h"
#include "v3_execution.h"
#include "v3_run_contract.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dsm::v3::Json;

Json s(const std::string& value) { return Json(value); }
Json u(std::uint64_t value) { return Json(value); }
Json b(bool value) { return Json(value); }

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void writeBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "cannot create synthetic fixture file");
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    require(output.good(), "cannot finish synthetic fixture file");
}

Json graphCaps() {
    Json::Object value;
    for (const char* field : {
            "data_vertices", "query_vertices",
            "combined_outgoing_adjacency_entries",
            "combined_incoming_adjacency_entries", "temporal_edges",
            "interval_records", "derived_incident_interval_visits",
            "derived_neighbor_label_updates", "snapshot_storage_bytes",
            "temporary_validation_bytes", "seal_peak_live_bytes"}) {
        value.emplace(field, u(1'000'000));
    }
    return Json::object(std::move(value));
}

Json candidateCaps() {
    Json::Object value;
    for (const char* field : {
            "adjacency_probes", "reverse_edge_lookups", "arc_checks",
            "relation_entries", "candidate_value_occurrences", "blocks",
            "semijoin_entry_visits", "index_entries",
            "payload_memory_bytes"}) {
        value.emplace(field, u(1'000'000));
    }
    value.emplace("fixed_memory_headroom_bytes", u(0));
    value.emplace("candidate_tree_tokens", u(1));
    return Json::object(std::move(value));
}

Json lazyCaps() {
    Json::Object value;
    for (const char* field : {
            "adjacency_probes", "reverse_edge_lookups", "arc_checks",
            "emitted_entries", "cache_capacity_bytes", "extension_probes",
            "interval_intersections"}) {
        value.emplace(field, u(1'000'000));
    }
    return Json::object(std::move(value));
}

Json makeContract(
    const std::string& arm_id,
    std::uint64_t ordinal,
    const std::string& data_path,
    const std::string& data_bytes,
    const std::string& query_path,
    const std::string& query_bytes,
    const std::vector<std::string>& labels,
    const std::string& run_path,
    const std::string& receipt_path) {
    const bool incumbent = arm_id == "v3dev_incumbent_16t";
    const bool gate_on = arm_id == "v3dev_cover_lazy_gate_on_16t";
    Json::Object root;
    root.emplace("schema", s("icde2027_v3_run_contract"));
    root.emplace("schema_version", u(1));

    Json::Object identity;
    identity.emplace("arm_id", s(arm_id));
    identity.emplace("arm_position", u(ordinal));
    identity.emplace("attempt", u(1));
    identity.emplace("block_id", s("synthetic_block"));
    identity.emplace("case_id", s("synthetic_case"));
    identity.emplace("ordinal", u(ordinal));
    identity.emplace("protocol_file_sha256", s(std::string(64, 'a')));
    identity.emplace("protocol_id", s("synthetic-v3-execution"));
    identity.emplace("protocol_payload_sha256", s(std::string(64, 'b')));
    identity.emplace("repeat", u(1));
    identity.emplace("run_contract_sha256", s(std::string(64, 'c')));
    identity.emplace("run_signature_sha256", s(std::string(64, 'd')));
    root.emplace("identity", Json::object(std::move(identity)));

    Json::Object authorization;
    authorization.emplace("authorization_path", s("synthetic/authorization.json"));
    authorization.emplace("authorization_receipt_sha256", s(std::string(64, 'e')));
    authorization.emplace("execution_authorized_observed", b(true));
    authorization.emplace("headline_authorized", b(false));
    root.emplace("authorization", Json::object(std::move(authorization)));

    Json::Object algorithm;
    algorithm.emplace("formula_id", s(dsm::v3::kV3SelectivityFormulaId));
    algorithm.emplace("internal_filter", s("consecutive_pair_on"));
    algorithm.emplace("label_mode", s("stable_id"));
    algorithm.emplace("label_seed", u(42));
    algorithm.emplace("match_output", s("count_only"));
    algorithm.emplace("minimum_duration", u(2));
    algorithm.emplace("planner_policy", s(
        incumbent ? "validated_incumbent_only" : "cover_lazy_v3"));
    algorithm.emplace("profitability_gate", s(
        incumbent ? "not_applicable" :
        gate_on ? "enabled_exact_nine_tenths" :
                  "disabled_attribution_only"));
    algorithm.emplace("profitability_ratio_denominator", u(10));
    algorithm.emplace("profitability_ratio_numerator", u(9));
    root.emplace("algorithm", Json::object(std::move(algorithm)));

    Json::Object input;
    input.emplace("data_bytes", u(data_bytes.size()));
    input.emplace("data_kind", s("full_raw"));
    input.emplace("data_path", s(data_path));
    input.emplace("data_sha256", s(dsm::v3::sha256Hex(data_bytes)));
    input.emplace("query_bytes", u(query_bytes.size()));
    input.emplace("query_edges", u(2));
    Json::Array edges;
    edges.push_back(Json::array({u(0), u(1)}));
    edges.push_back(Json::array({u(1), u(2)}));
    input.emplace("query_edges_directed", Json::array(std::move(edges)));
    Json::Array label_array;
    for (const std::string& label : labels) label_array.push_back(s(label));
    input.emplace("query_labels", Json::array(std::move(label_array)));
    input.emplace("query_path", s(query_path));
    input.emplace("query_sha256", s(dsm::v3::sha256Hex(query_bytes)));
    input.emplace("query_vertices", u(3));
    root.emplace("input", Json::object(std::move(input)));

    Json::Object resources;
    resources.emplace("affinity_cpu_set", Json::array({u(0), u(1)}));
    resources.emplace("memory_max_bytes", u(1'000'000'000));
    resources.emplace("memory_swap_max_bytes", u(0));
    resources.emplace("smt_active_required", u(0));
    resources.emplace("threads_requested", u(2));
    resources.emplace("threads_used_required", u(2));
    resources.emplace("timeout_seconds", u(60));
    resources.emplace("worker_count_semantics", s("executor_pool_size"));
    root.emplace("resources", Json::object(std::move(resources)));

    Json::Object planner_caps;
    planner_caps.emplace("complete_proposal_cap", u(64));
    planner_caps.emplace("prefix_samples_per_plan", u(16));
    planner_caps.emplace("proposal_state_expansions", u(256));
    planner_caps.emplace("quote_cover_value_visits", u(10000));
    Json::Object metadata_caps;
    metadata_caps.emplace("payload_bytes", u(1'000'000));
    metadata_caps.emplace("receipt_tokens", u(1));
    Json::Object caps;
    caps.emplace("candidate_build", candidateCaps());
    caps.emplace("graph_validation", graphCaps());
    caps.emplace("lazy", lazyCaps());
    caps.emplace("metadata", Json::object(std::move(metadata_caps)));
    caps.emplace("planner", Json::object(std::move(planner_caps)));
    root.emplace("numeric_caps", Json::object(std::move(caps)));

    Json::Object build;
    for (const char* field : {
            "compiler_identity_sha256", "executable_sha256",
            "parameter_bundle_sha256", "runner_sha256",
            "source_bundle_sha256", "validator_sha256"}) {
        build.emplace(field, s(std::string(64, 'f')));
    }
    root.emplace("build", Json::object(std::move(build)));

    Json::Object output;
    output.emplace("matcher_receipt_path", s(receipt_path));
    output.emplace("run_contract_path", s(run_path));
    output.emplace("run_directory", s(
        std::filesystem::path(receipt_path).parent_path().generic_string()));
    output.emplace("runner_receipt_path", s(
        (std::filesystem::path(receipt_path).parent_path() /
         "runner_receipt.json").generic_string()));
    root.emplace("output", Json::object(std::move(output)));
    return Json::object(std::move(root));
}

} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2, "usage: test_v3_execution RELATIVE_FIXTURE_ROOT");
        const std::filesystem::path root(argv[1]);
        require(!root.is_absolute(), "fixture root must be repository-relative");
        std::filesystem::create_directories(root);
        const std::string data_bytes =
            "10 11 1\n10 11 2\n10 11 3\n"
            "11 12 1\n11 12 2\n11 12 3\n"
            "20 21 1\n20 21 2\n21 22 1\n21 22 2\n";
        const std::vector<std::string> labels{
            labelToString(stableIdLabel(10, 42)),
            labelToString(stableIdLabel(11, 42)),
            labelToString(stableIdLabel(12, 42))};
        const std::string query_bytes =
            "q0:" + labels[0] + " q1:" + labels[1] + "\n" +
            "q1:" + labels[1] + " q2:" + labels[2] + "\n";
        const std::string data_path = (root / "data.txt").generic_string();
        const std::string query_path = (root / "query.qry").generic_string();
        writeBytes(data_path, data_bytes);
        writeBytes(query_path, query_bytes);

        const std::vector<std::string> arms{
            "v3dev_incumbent_16t",
            "v3dev_cover_lazy_gate_on_16t",
            "v3dev_cover_lazy_gate_off_16t"};
        for (std::size_t index = 0; index < arms.size(); ++index) {
            const std::filesystem::path run_dir =
                root / ("run_" + std::to_string(index + 1));
            std::filesystem::create_directories(run_dir);
            const std::string contract_path =
                (run_dir / "run_contract.json").generic_string();
            const std::string receipt_path =
                (run_dir / "matcher_receipt.json").generic_string();
            Json contract = makeContract(
                arms[index], index + 1, data_path, data_bytes,
                query_path, query_bytes, labels,
                contract_path, receipt_path);
            writeBytes(contract_path, dsm::v3::canonicalJsonBytes(contract));
            dsm::v3::ValidatedV3RunContract authority;
            authority.contract.value = contract;
            authority.ordinal = index + 1;
            authority.matcher_receipt_path = receipt_path;
            const dsm::v3::V3ExecutionSpec spec =
                dsm::v3::projectV3ExecutionSpec(authority);
            dsm::v3::V3EnvironmentObservation environment;
            environment.affinity_cpu_set = {0, 1};
            environment.smt_active = 0;
            environment.cgroup_path = "/synthetic-v3";
            environment.cgroup_id = "0:0:1";
            environment.memory_max_bytes = 1'000'000'000;
            environment.memory_swap_max_bytes = 0;
            const int code = dsm::v3::executeProjectedV3RunForTesting(
                authority, spec, environment);
            require(code == 0,
                "synthetic arm failed with exit " + std::to_string(code));
            const dsm::v3::CanonicalArtifact receipt =
                dsm::v3::loadCanonicalArtifact(receipt_path);
            require(receipt.value.at("schema").string() ==
                    "icde2027_v3_matcher_receipt",
                "synthetic receipt schema drift");
            // Exact no-overwrite behavior is part of the public executor: a
            // second process attempt cannot replace the first artifact.
            require(dsm::v3::executeProjectedV3RunForTesting(
                    authority, spec, environment) ==
                    dsm::v3::kV3ReceiptIoError,
                "second synthetic execution overwrote an existing receipt");
        }
        std::cout << "All synthetic V3 execution tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "V3 execution test failure: " << error.what() << '\n';
        return 1;
    }
}
