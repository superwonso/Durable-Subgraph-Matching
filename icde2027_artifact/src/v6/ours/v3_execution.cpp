#include "v3_execution.h"

#include <v3_build_identity_generated.h>

#include "TDTree.h"
#include "query_decomposition.h"
#include "v3_canonical_json.h"
#include "v3_plan_handoff.h"
#include "v3_receipt.h"
#include "v3_run_contract.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#if !defined(_WIN32)
#include <sched.h>
#endif
#include <sstream>
#include <string_view>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif
#include <utility>

namespace dsm::v3 {
#if defined(_WIN32)

ClassifiedV3Error::ClassifiedV3Error(
    int exit_code,
    std::string message)
    : std::runtime_error(std::move(message)), exit_code_(exit_code) {}

int ClassifiedV3Error::exitCode() const noexcept { return exit_code_; }

V3ExecutionSpec projectV3ExecutionSpec(
    const ValidatedV3RunContract&) {
    throw ClassifiedV3Error(
        kV3EnvironmentError,
        "closed V3 execution requires the Linux cgroup-v2 adapter");
}

V3EnvironmentObservation observeV3Environment() {
    throw ClassifiedV3Error(
        kV3EnvironmentError,
        "closed V3 execution requires the Linux cgroup-v2 adapter");
}

void requireV3Environment(
    const V3EnvironmentObservation&,
    const V3ResourceSpec&) {
    throw ClassifiedV3Error(
        kV3EnvironmentError,
        "closed V3 execution requires the Linux cgroup-v2 adapter");
}

int executeValidatedV3Run(
    const ValidatedV3RunContract&,
    const std::string&) noexcept {
    std::cerr
        << "V3 environment error: closed execution requires Linux cgroup v2.\n";
    return kV3EnvironmentError;
}

#else
namespace {

using Clock = std::chrono::steady_clock;
constexpr const char* kImplementationGeneration =
    "tdtree_cpu_snapshot_v3_v1";
constexpr const char* kPlannerGeneration = "cover_lazy_v3_v1";
constexpr const char* kAccountingConvention =
    "symmetric_complete_tree_rescore_v1";

const char* numericCapBindingModeName(
    V3NumericCapBindingMode mode) noexcept {
    switch (mode) {
        case V3NumericCapBindingMode::
                ContractMaterializedWithoutFormulaDerivation:
            return "contract_materialized_caps_v1_without_formula_derivation";
        case V3NumericCapBindingMode::
                PostFilterFormulaDerivedCheckedU64V1:
            return "post_filter_formula_derived_checked_u64_v1";
    }
    return "unknown";
}

std::uint64_t uintField(
    const Json& object,
    std::string_view field,
    std::string_view location) {
    const Json& value = object.at(field);
    if (!value.isUnsignedInteger()) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError,
            std::string(location) + "." + std::string(field) +
                " must be uint64");
    }
    return value.unsignedInteger();
}

std::string stringField(
    const Json& object,
    std::string_view field,
    std::string_view location) {
    const Json& value = object.at(field);
    if (!value.isString()) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError,
            std::string(location) + "." + std::string(field) +
                " must be a string");
    }
    return value.string();
}

const Json::Array& arrayField(
    const Json& object,
    std::string_view field,
    std::string_view location) {
    const Json& value = object.at(field);
    if (!value.isArray()) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError,
            std::string(location) + "." + std::string(field) +
                " must be an array");
    }
    return value.arrayValue();
}

std::size_t checkedSize(
    std::uint64_t value,
    std::string_view location,
    int exit_code = kV3CliOrSchemaError) {
    if (value > std::numeric_limits<std::size_t>::max()) {
        throw ClassifiedV3Error(
            exit_code, std::string(location) + " exceeds size_t");
    }
    return static_cast<std::size_t>(value);
}

int checkedInt(std::uint64_t value, std::string_view location) {
    if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError, std::string(location) + " exceeds int");
    }
    return static_cast<int>(value);
}

std::uint32_t checkedU32(std::uint64_t value, std::string_view location) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError, std::string(location) + " exceeds uint32");
    }
    return static_cast<std::uint32_t>(value);
}

std::uint64_t checkedMultiply(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::string_view location,
    int exit_code = kV3PlannerNotIntegrated) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        throw ClassifiedV3Error(
            exit_code, std::string(location) + " multiplication overflow");
    }
    return lhs * rhs;
}

std::uint64_t checkedAdd(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::string_view location,
    int exit_code) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        throw ClassifiedV3Error(
            exit_code, std::string(location) + " addition overflow");
    }
    return lhs + rhs;
}

bool addNoOverflow(
    std::uint64_t lhs,
    std::uint64_t rhs,
    std::uint64_t& result) noexcept {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) return false;
    result = lhs + rhs;
    return true;
}

bool graphReceiptInvariant(
    const GraphValidationReceipt& r,
    const GraphValidationBudget& b) noexcept {
    std::uint64_t vertices = 0;
    std::uint64_t outgoing = 0;
    std::uint64_t incoming = 0;
    std::uint64_t doubled_vertices = 0;
    std::uint64_t doubled_edges = 0;
    std::uint64_t doubled_intervals = 0;
    std::uint64_t quoted_peak_partial = 0;
    std::uint64_t quoted_peak = 0;
    std::uint64_t actual_peak_partial = 0;
    std::uint64_t actual_peak = 0;
    if (!r.complete() || r.storage_quote_passes != 1 ||
        r.storage_quote_graphs_visited != 2 ||
        r.storage_accounting_passes != 2 ||
        r.storage_accounting_graphs_visited != 4 ||
        !addNoOverflow(r.data_vertices_visited, r.query_vertices_visited, vertices) ||
        !addNoOverflow(
            r.data_outgoing_adjacency_entries_visited,
            r.query_outgoing_adjacency_entries_visited, outgoing) ||
        !addNoOverflow(
            r.data_incoming_adjacency_entries_visited,
            r.query_incoming_adjacency_entries_visited, incoming) ||
        r.storage_quote_vertices_visited != vertices ||
        r.storage_quote_outgoing_adjacency_entries_visited != outgoing ||
        r.storage_quote_incoming_adjacency_entries_visited != incoming ||
        r.storage_quote_incident_outgoing_entries_visited !=
            r.data_outgoing_adjacency_entries_visited ||
        r.storage_quote_incident_incoming_entries_visited !=
            r.data_incoming_adjacency_entries_visited ||
        r.storage_quote_temporal_edges_visited != r.temporal_edges_visited ||
        r.storage_quote_interval_records_visited != r.interval_records_visited ||
        !addNoOverflow(vertices, vertices, doubled_vertices) ||
        !addNoOverflow(
            r.storage_quote_temporal_edges_visited,
            r.storage_quote_temporal_edges_visited, doubled_edges) ||
        !addNoOverflow(
            r.storage_quote_interval_records_visited,
            r.storage_quote_interval_records_visited, doubled_intervals) ||
        r.storage_accounting_vertices_visited != doubled_vertices ||
        r.storage_accounting_temporal_edges_visited != doubled_edges ||
        r.storage_accounting_interval_records_visited != doubled_intervals ||
        !addNoOverflow(
            r.source_live_storage_bytes,
            r.quoted_snapshot_storage_bytes, quoted_peak_partial) ||
        !addNoOverflow(
            quoted_peak_partial,
            r.quoted_validation_temporary_bytes, quoted_peak) ||
        r.quoted_seal_peak_live_bytes != quoted_peak ||
        !addNoOverflow(
            r.source_live_storage_bytes,
            r.snapshot_storage_bytes, actual_peak_partial) ||
        !addNoOverflow(
            actual_peak_partial,
            r.temporary_validation_capacity_bytes, actual_peak) ||
        r.seal_peak_live_bytes != actual_peak) {
        return false;
    }
    std::uint64_t vertex_cap = 0;
    return addNoOverflow(b.data_vertices, b.query_vertices, vertex_cap) &&
        r.data_vertices_visited <= b.data_vertices &&
        r.query_vertices_visited <= b.query_vertices &&
        vertices <= vertex_cap &&
        outgoing <= b.combined_outgoing_adjacency_entries &&
        incoming <= b.combined_incoming_adjacency_entries &&
        r.temporal_edges_visited <= b.temporal_edges &&
        r.interval_records_visited <= b.interval_records &&
        r.derived_incident_interval_visits <=
            b.derived_incident_interval_visits &&
        r.derived_neighbor_label_updates <=
            b.derived_neighbor_label_updates &&
        r.quoted_maximum_incident_intervals <=
            b.derived_incident_interval_visits &&
        r.snapshot_storage_bytes <= r.quoted_snapshot_storage_bytes &&
        r.quoted_snapshot_storage_bytes <= b.snapshot_storage_bytes &&
        r.temporary_validation_capacity_bytes <=
            r.quoted_validation_temporary_bytes &&
        r.quoted_validation_temporary_bytes <=
            b.temporary_validation_bytes &&
        r.seal_peak_live_bytes <= r.quoted_seal_peak_live_bytes &&
        r.quoted_seal_peak_live_bytes <= b.seal_peak_live_bytes;
}

struct ArmExecution;
bool plannerCapsInvariant(
    const ArmExecution& arm,
    const V3ExecutionSpec& spec) noexcept;

std::uint64_t elapsedNs(
    Clock::time_point begin,
    Clock::time_point end,
    std::string_view location) {
    if (end < begin) {
        throw ClassifiedV3Error(
            kV3ExecutionExactnessError,
            std::string(location) + " clock moved backwards");
    }
    const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
        end - begin).count();
    if (value <= 0) {
        throw ClassifiedV3Error(
            kV3ExecutionExactnessError,
            std::string(location) + " must be positive");
    }
    return static_cast<std::uint64_t>(value);
}

Json stringJson(std::string value) { return Json(std::move(value)); }
Json uintJson(std::uint64_t value) { return Json(value); }
Json boolJson(bool value) { return Json(value); }

Json uintArray(const std::vector<std::uint64_t>& values) {
    Json::Array result;
    result.reserve(values.size());
    for (std::uint64_t value : values) result.push_back(uintJson(value));
    return Json::array(std::move(result));
}

std::uint64_t sizeToU64(std::size_t value, std::string_view location) {
    if (value > std::numeric_limits<std::uint64_t>::max()) {
        throw ClassifiedV3Error(
            kV3ExecutionExactnessError,
            std::string(location) + " exceeds uint64");
    }
    return static_cast<std::uint64_t>(value);
}

std::string hex64(std::uint64_t value) {
    std::ostringstream output;
    output << std::hex << std::nouppercase << std::setfill('0')
           << std::setw(16) << value;
    return output.str();
}

std::string readTokenFile(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        throw ClassifiedV3Error(
            kV3EnvironmentError, "cannot open environment authority: " + path);
    }
    std::string value;
    std::string extra;
    if (!(input >> value) || (input >> extra)) {
        throw ClassifiedV3Error(
            kV3EnvironmentError, "malformed environment authority: " + path);
    }
    return value;
}

std::uint64_t parseDecimal(
    const std::string& value,
    std::string_view location) {
    if (value.empty()) {
        throw ClassifiedV3Error(
            kV3EnvironmentError, std::string(location) + " is empty");
    }
    std::uint64_t result = 0;
    for (char character : value) {
        if (character < '0' || character > '9') {
            throw ClassifiedV3Error(
                kV3EnvironmentError,
                std::string(location) + " is not finite decimal uint64");
        }
        const unsigned digit = static_cast<unsigned>(character - '0');
        if (result > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U) {
            throw ClassifiedV3Error(
                kV3EnvironmentError,
                std::string(location) + " overflows uint64");
        }
        result = result * 10U + digit;
    }
    return result;
}

bool canonicalNonrootCgroup(std::string_view path) noexcept {
    if (path.size() < 2U || path.front() != '/' || path.back() == '/' ||
        path.find('\\') != std::string_view::npos ||
        path.find("//") != std::string_view::npos) {
        return false;
    }
    std::size_t begin = 1;
    while (begin < path.size()) {
        const std::size_t end = path.find('/', begin);
        const std::string_view component = path.substr(
            begin,
            end == std::string_view::npos
                ? path.size() - begin
                : end - begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return true;
}

std::string hashOpenFile(const char* path) {
    int descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw ClassifiedV3Error(
            kV3AuthorizationOrHashError,
            std::string("cannot open executable authority: ") + path);
    }
    struct Closer {
        int descriptor;
        ~Closer() {
            if (descriptor >= 0) {
                while (::close(descriptor) != 0 && errno == EINTR) {}
            }
        }
    } closer{descriptor};
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode)) {
        throw ClassifiedV3Error(
            kV3AuthorizationOrHashError,
            "running executable is not a regular-file authority");
    }
    Sha256Accumulator hash;
    std::array<char, 64U * 1024U> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count < 0) {
            if (errno == EINTR) continue;
            throw ClassifiedV3Error(
                kV3AuthorizationOrHashError,
                "running executable read failed");
        }
        if (count == 0) break;
        hash.update(std::string_view(
            buffer.data(), static_cast<std::size_t>(count)));
    }
    return hash.finishHex();
}

std::string parameterBundleSha256(
    const ValidatedV3RunContract& authority) {
    const Json& protocol = authority.protocol.value;
    Json::Object bundle;
    bundle.emplace("execution_contract", protocol.at("execution_contract"));
    bundle.emplace("fixed_policy", protocol.at("fixed_policy"));
    bundle.emplace("numeric_caps", protocol.at("numeric_caps"));
    bundle.emplace("resource_envelope", protocol.at("resource_envelope"));
    bundle.emplace("schema", stringJson("icde2027_v3_parameter_bundle"));
    bundle.emplace("schema_version", uintJson(1));
    return sha256Hex(canonicalJsonBytes(Json::object(std::move(bundle))));
}

void verifyBuildTrustAnchors(
    const ValidatedV3RunContract& authority,
    const V3ExecutionSpec& spec) {
    const std::string source_anchor = DSM_V3_SOURCE_BUNDLE_SHA256;
    const std::string compiler_anchor = DSM_V3_COMPILER_IDENTITY_SHA256;
    const std::string parameter_anchor = DSM_V3_PARAMETER_BUNDLE_SHA256;
    const std::string zeros(64, '0');
    if (source_anchor == zeros || compiler_anchor == zeros ||
        parameter_anchor == zeros) {
        throw ClassifiedV3Error(
            kV3AuthorizationOrHashError,
            "binary was not built with generated source/compiler trust anchors");
    }
    if (hashOpenFile("/proc/self/exe") != spec.executable_sha256 ||
        source_anchor != spec.source_bundle_sha256 ||
        compiler_anchor != spec.compiler_identity_sha256 ||
        parameter_anchor != spec.parameter_bundle_sha256 ||
        parameterBundleSha256(authority) != parameter_anchor) {
        throw ClassifiedV3Error(
            kV3AuthorizationOrHashError,
            "observed executable/source/compiler/parameter trust anchor differs from contract");
    }
    // runner_sha256 and validator_sha256 deliberately remain external trust
    // anchors: the live runner hashes its own and the semantic validator's
    // regular-file bytes before spawn.  The matcher has no authority path for
    // either and must not guess one from cwd/basenames.
}

int inputExitCode(V3InputStatus status) noexcept {
    switch (status) {
        case V3InputStatus::SizeMismatch:
        case V3InputStatus::HashMismatch:
        case V3InputStatus::ParsedAuthorityMismatch:
            return kV3AuthorizationOrHashError;
        default:
            return kV3InputOrFilterError;
    }
}

std::array<std::size_t, kLabelCount> labelCounts(const Graph& graph) {
    std::array<std::size_t, kLabelCount> counts{};
    for (Label label : graph.vertex_labels) {
        if (label < kLabelCount) ++counts[label];
    }
    return counts;
}

std::array<double, kLabelCount> labelMeanActivity(const Graph& graph) {
    std::array<std::uint64_t, kLabelCount> counts{};
    std::array<std::uint64_t, kLabelCount> sums{};
    for (std::size_t vertex = 0; vertex < graph.vertex_labels.size(); ++vertex) {
        const Label label = graph.vertex_labels[vertex];
        if (label >= kLabelCount || vertex >= graph.vertex_active_durations.size()) {
            continue;
        }
        ++counts[label];
        const int duration = graph.vertex_active_durations[vertex];
        if (duration < 0 ||
            static_cast<std::uint64_t>(duration) >
                std::numeric_limits<std::uint64_t>::max() - sums[label]) {
            throw ClassifiedV3Error(
                kV3PlannerNotIntegrated,
                "label activity aggregation overflow");
        }
        sums[label] += static_cast<std::uint64_t>(duration);
    }
    std::array<double, kLabelCount> means{};
    for (std::size_t label = 0; label < kLabelCount; ++label) {
        means[label] = counts[label] == 0
            ? 0.0
            : static_cast<double>(sums[label]) /
                static_cast<double>(counts[label]);
    }
    return means;
}

const char* plannerReason(V3TerminalDecision decision) noexcept {
    switch (decision) {
        case V3TerminalDecision::IncumbentEmptyOrDisconnected:
            return "incumbent domain is empty or query is disconnected";
        case V3TerminalDecision::IncumbentOnly:
            return "validated corrected-Paper incumbent retained";
        case V3TerminalDecision::NoNonincumbentCompleteProposal:
            return "no nonincumbent complete fixed plan was admitted";
        case V3TerminalDecision::NoBuildFeasiblePlan:
            return "no proposed plan fit the sealed candidate-build budget";
        case V3TerminalDecision::LazyTransitionBudget:
            return "lazy transition pilot exhausted a frozen cap";
        case V3TerminalDecision::SampleEmpty:
            return "deterministic prefix sample contained no executable prefix";
        case V3TerminalDecision::CandidateBuildBudget:
            return "candidate materialization was not admitted by the frozen ledger";
        case V3TerminalDecision::CandidateBuildQuoteViolation:
            return "candidate materializer exceeded its sealed quote";
        case V3TerminalDecision::GlobalMemoryAdmission:
            return "exact existing-plus-candidate-plus-headroom admission failed";
        case V3TerminalDecision::CandidateReservationAllocationFailure:
            return "candidate reservation owner allocation failed before debit";
        case V3TerminalDecision::CandidateRescoreBudget:
            return "symmetric complete-tree rescore exhausted a frozen cap";
        case V3TerminalDecision::MetadataBudget:
            return "external selected-plan receipt exceeded metadata budget";
        case V3TerminalDecision::SamePlan:
            return "best proposed plan equals corrected-Paper incumbent";
        case V3TerminalDecision::GainBelowMargin:
            return "candidate did not satisfy exact nine-tenths profitability gate";
        case V3TerminalDecision::CandidateSelected:
            return "complete nonincumbent candidate selected and handed off";
        case V3TerminalDecision::IncumbentOriginMismatch:
            return "incumbent origin does not attest corrected Paper selectivity";
    }
    return "unknown planner terminal decision";
}

struct ArmExecution {
    V3TerminalDecision decision = V3TerminalDecision::IncumbentOnly;
    std::unique_ptr<TDTree> tree;
    std::unique_ptr<SelectedPlanReceipt> receipt;
    V3PlannerTelemetry telemetry;
    V3CandidateBuildEvidence candidate_build;
    std::uint64_t incumbent_plan_fingerprint = 0;
    std::uint64_t selected_plan_fingerprint = 0;
    std::uint64_t predicted_incumbent_total = 0;
    std::uint64_t predicted_candidate_total = 0;
};

bool plannerCapsInvariant(
    const ArmExecution& arm,
    const V3ExecutionSpec& spec) noexcept {
    const V3PlannerTelemetry& t = arm.telemetry;
    if (t.v3_proposal_states_expanded >
            spec.proposal_budget.maximum_states_expanded ||
        t.v3_complete_plan_proposals >
            spec.proposal_budget.maximum_complete_plans ||
        t.v3_quote_cover_value_visits >
            spec.proposal_budget.maximum_quote_cover_value_visits ||
        t.v3_lazy_block_requests > spec.lazy_budget.maximum_block_requests ||
        t.v3_lazy_adjacency_probes >
            spec.lazy_budget.maximum_adjacency_probes ||
        t.v3_lazy_reverse_lookups >
            spec.lazy_budget.maximum_reverse_lookups ||
        t.v3_lazy_arc_checks > spec.lazy_budget.maximum_arc_checks ||
        t.v3_lazy_relation_entries >
            spec.lazy_budget.maximum_relation_entries ||
        t.v3_lazy_cache_capacity_bytes >
            spec.lazy_budget.maximum_cache_capacity_bytes ||
        t.v3_lazy_extension_probes >
            spec.lazy_budget.maximum_extension_probes ||
        t.v3_lazy_interval_intersections >
            spec.lazy_budget.maximum_interval_intersections ||
        t.v3_lazy_maximum_prefix_samples_observed >
            spec.lazy_budget.prefix_sample_cap ||
        t.v3_rescore_maximum_prefix_samples_observed >
            spec.rescore_budget.prefix_sample_cap ||
        t.v3_maximum_prefix_samples_observed >
            spec.lazy_budget.prefix_sample_cap ||
        t.v3_materialized_complete_plans >
            spec.candidate_build_budget.candidate_tree_tokens) {
        return false;
    }
    if (spec.arm == V3Arm::IncumbentI) {
        return t.v3_proposal_states_expanded == 0 &&
            t.v3_complete_plan_proposals == 0 &&
            t.v3_nonincumbent_plan_proposals == 0 &&
            t.v3_scored_plan_proposals == 0 &&
            t.v3_materialized_complete_plans == 0 &&
            t.v3_rescored_complete_plans == 0 &&
            t.v3_quote_cover_value_visits == 0 &&
            t.v3_lazy_block_requests == 0 &&
            t.v3_lazy_adjacency_probes == 0 &&
            t.v3_lazy_reverse_lookups == 0 &&
            t.v3_lazy_arc_checks == 0 &&
            t.v3_lazy_relation_entries == 0 &&
            t.v3_lazy_cache_capacity_bytes == 0 &&
            t.v3_lazy_extension_probes == 0 &&
            t.v3_lazy_interval_intersections == 0 &&
            !arm.candidate_build.quote_available &&
            arm.predicted_candidate_total == 0;
    }
    return true;
}

ArmExecution executeIncumbentArm(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree> tree,
    const FixedPlan& plan,
    const V3ExecutionSpec& spec) {
    ArmExecution result;
    result.tree = std::move(tree);
    result.incumbent_plan_fingerprint = plan.fingerprint;
    result.selected_plan_fingerprint = plan.fingerprint;
    result.predicted_incumbent_total = std::max<std::uint64_t>(
        1, sizeToU64(result.tree->candidateRelationCount(),
                     "incumbent relation volume"));
    SelectedPlanMetadata metadata;
    metadata.reporting_mode = SelectivityMode::Paper;
    metadata.decision = PlannerDecision::IncumbentOnly;
    metadata.diagnostics.graph_validation_passes = 1;
    const V3MetadataQuote quote = quoteV3PlanHandoff(
        snapshot, *result.tree, plan, metadata,
        spec.metadata_payload_bytes);
    V3MetadataBudget budget{
        spec.metadata_payload_bytes, spec.metadata_receipt_tokens};
    if (!quote.complete || !quote.fits(budget) ||
        !quote.isBoundToValidatedSnapshot(snapshot)) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "incumbent external-plan metadata quote failed");
    }
    V3PlanHandoffResult handoff = commitV3PlanHandoff(
        snapshot, result.tree, plan, metadata, quote, budget);
    if (!handoff.complete()) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "incumbent external-plan metadata commit failed");
    }
    result.tree = std::move(handoff.selected_tree);
    result.receipt = std::move(handoff.receipt);
    return result;
}

ArmExecution executePlannerArm(
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    std::unique_ptr<TDTree> incumbent,
    const FixedPlan& plan,
    const V3ExecutionSpec& spec,
    const SourceGraphReleaseReceiptV1& source_data_release,
    const SourceGraphReleaseReceiptV1& source_query_release) {
    CandidateBuildLedger ledger(spec.candidate_build_budget);
    V3MetadataBudget metadata{
        spec.metadata_payload_bytes, spec.metadata_receipt_tokens};
    TemporalCostV3Options options;
    options.proposal_budget = spec.proposal_budget;
    options.lazy_budget = spec.lazy_budget;
    options.rescore_budget = spec.rescore_budget;
    options.candidate_build_accounting = defaultCandidateBuildAccounting();
    options.metadata_payload_cap = spec.metadata_payload_bytes;
    options.maximum_trace_entries = 0;
    options.profitability_gate_enabled = spec.profitability_gate_enabled;
    options.global_memory_admission_enabled =
        spec.numeric_cap_binding_mode == V3NumericCapBindingMode::
            PostFilterFormulaDerivedCheckedU64V1;
    options.process_memory_max_bytes = spec.resources.memory_max_bytes;
    options.existing_capacity_phase_generation =
        options.global_memory_admission_enabled ? 1 : 0;
    options.existing_capacity_accounting_model_sha256 =
        spec.existing_capacity_accounting_model_sha256;
    options.source_data_graph_release = source_data_release;
    options.source_query_graph_release = source_query_release;
    if (options.global_memory_admission_enabled) {
        // The native slice below proves only the closed 11-component
        // pre-reservation state. Post-build rescore/metadata/executor peaks
        // remain outside its typed scope, so this diagnostic path cannot emit
        // a production/timing-eligible handoff until the later full-process
        // workspace proof is integrated.
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "pre-reservation memory admission is diagnostic-only; full-process memory proof is incomplete");
    }
    TemporalCostV3Result planned = planTemporalCostV3(
        snapshot, std::move(incumbent), plan, ledger, metadata, options);
    if (planned.decision ==
            V3TerminalDecision::IncumbentEmptyOrDisconnected &&
        planned.selected_tree && !planned.receipt) {
        // A complete, legal incumbent with an empty root domain is an exact
        // zero-match result, not a semantic failure.  The planner exits before
        // its usual handoff in this branch, so preserve that native decision
        // while constructing the same bounded external incumbent receipt.
        SelectedPlanMetadata fallback_metadata;
        fallback_metadata.reporting_mode = SelectivityMode::Paper;
        fallback_metadata.decision = PlannerDecision::IncumbentOnly;
        fallback_metadata.diagnostics.graph_validation_passes = 1;
        const V3MetadataQuote quote = quoteV3PlanHandoff(
            snapshot, *planned.selected_tree, plan,
            fallback_metadata, spec.metadata_payload_bytes);
        V3PlanHandoffResult handoff;
        if (quote.complete && quote.fits(metadata) &&
            quote.isBoundToValidatedSnapshot(snapshot)) {
            handoff = commitV3PlanHandoff(
                snapshot, planned.selected_tree, plan,
                fallback_metadata, quote, metadata);
        }
        if (handoff.complete()) {
            planned.selected_tree = std::move(handoff.selected_tree);
            planned.receipt = std::move(handoff.receipt);
        }
    }
    if (!planned.invariantHolds() || !planned.selected_tree ||
        !planned.receipt ||
        planned.decision == V3TerminalDecision::IncumbentOriginMismatch ||
        planned.decision == V3TerminalDecision::CandidateBuildQuoteViolation ||
        planned.decision == V3TerminalDecision::GlobalMemoryAdmission ||
        planned.decision ==
            V3TerminalDecision::CandidateReservationAllocationFailure ||
        planned.decision == V3TerminalDecision::MetadataBudget) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "planner did not produce complete schema-eligible handoff evidence");
    }
    ArmExecution result;
    result.decision = planned.decision;
    result.tree = std::move(planned.selected_tree);
    result.receipt = std::move(planned.receipt);
    result.telemetry = planned.telemetry;
    result.candidate_build = planned.candidate_build;
    result.incumbent_plan_fingerprint = planned.incumbent_plan_fingerprint;
    result.selected_plan_fingerprint = planned.selected_plan_fingerprint;
    result.predicted_incumbent_total =
        planned.telemetry.predicted_incumbent_total;
    result.predicted_candidate_total =
        planned.telemetry.predicted_candidate_total;
    if (result.predicted_incumbent_total == 0) {
        result.predicted_incumbent_total = std::max<std::uint64_t>(
            1, sizeToU64(result.tree->candidateRelationCount(),
                         "fallback incumbent relation volume"));
    }
    return result;
}

Json graphValidationReceiptJson(const GraphValidationReceipt& r) {
    Json::Object object;
#define DSM_V3_U64_FIELD(name) object.emplace(#name, uintJson(r.name))
    DSM_V3_U64_FIELD(snapshot_token);
    DSM_V3_U64_FIELD(graph_validation_calls);
    DSM_V3_U64_FIELD(storage_quote_passes);
    DSM_V3_U64_FIELD(storage_quote_graphs_visited);
    DSM_V3_U64_FIELD(storage_quote_vertices_visited);
    DSM_V3_U64_FIELD(storage_quote_outgoing_adjacency_entries_visited);
    DSM_V3_U64_FIELD(storage_quote_incoming_adjacency_entries_visited);
    DSM_V3_U64_FIELD(storage_quote_incident_outgoing_entries_visited);
    DSM_V3_U64_FIELD(storage_quote_incident_incoming_entries_visited);
    DSM_V3_U64_FIELD(storage_quote_temporal_edges_visited);
    DSM_V3_U64_FIELD(storage_quote_interval_records_visited);
    DSM_V3_U64_FIELD(storage_accounting_passes);
    DSM_V3_U64_FIELD(storage_accounting_graphs_visited);
    DSM_V3_U64_FIELD(storage_accounting_vertices_visited);
    DSM_V3_U64_FIELD(storage_accounting_temporal_edges_visited);
    DSM_V3_U64_FIELD(storage_accounting_interval_records_visited);
    DSM_V3_U64_FIELD(data_vertices_visited);
    DSM_V3_U64_FIELD(query_vertices_visited);
    DSM_V3_U64_FIELD(data_outgoing_adjacency_entries_visited);
    DSM_V3_U64_FIELD(data_incoming_adjacency_entries_visited);
    DSM_V3_U64_FIELD(query_outgoing_adjacency_entries_visited);
    DSM_V3_U64_FIELD(query_incoming_adjacency_entries_visited);
    DSM_V3_U64_FIELD(temporal_edges_visited);
    DSM_V3_U64_FIELD(interval_records_visited);
    DSM_V3_U64_FIELD(derived_metadata_rebuild_passes);
    DSM_V3_U64_FIELD(derived_incident_interval_visits);
    DSM_V3_U64_FIELD(derived_neighbor_label_updates);
    DSM_V3_U64_FIELD(data_fingerprint_passes);
    DSM_V3_U64_FIELD(query_fingerprint_passes);
    DSM_V3_U64_FIELD(validation_nanoseconds);
    DSM_V3_U64_FIELD(snapshot_storage_bytes);
    DSM_V3_U64_FIELD(temporary_validation_capacity_bytes);
    DSM_V3_U64_FIELD(source_live_storage_bytes);
    DSM_V3_U64_FIELD(quoted_snapshot_storage_bytes);
    DSM_V3_U64_FIELD(snapshot_owner_control_bytes);
    DSM_V3_U64_FIELD(quoted_validation_temporary_bytes);
    DSM_V3_U64_FIELD(quoted_maximum_incident_intervals);
    DSM_V3_U64_FIELD(quoted_seal_peak_live_bytes);
    DSM_V3_U64_FIELD(seal_peak_live_bytes);
    DSM_V3_U64_FIELD(data_fingerprint);
    DSM_V3_U64_FIELD(query_fingerprint);
#undef DSM_V3_U64_FIELD
    object.emplace("status", stringJson(graphValidationStatusName(r.status)));
    return Json::object(std::move(object));
}

Json workVectorJson(const V3CandidateBuildWorkVector& work) {
    Json::Object object;
    object.emplace("adjacency_probes", uintJson(work.adjacency_probes));
    object.emplace("reverse_edge_lookups", uintJson(work.reverse_edge_lookups));
    object.emplace("arc_checks", uintJson(work.arc_checks));
    object.emplace("relation_entries", uintJson(work.relation_entries));
    object.emplace(
        "candidate_value_occurrences",
        uintJson(work.candidate_value_occurrences));
    object.emplace("blocks", uintJson(work.blocks));
    object.emplace(
        "semijoin_entry_visits", uintJson(work.semijoin_entry_visits));
    object.emplace("index_entries", uintJson(work.index_entries));
    object.emplace(
        "payload_memory_bytes", uintJson(work.payload_memory_bytes));
    return Json::object(std::move(object));
}

Json lazyActualJson(const V3PlannerTelemetry& telemetry) {
    Json::Object object;
    object.emplace("adjacency_probes", uintJson(telemetry.v3_lazy_adjacency_probes));
    object.emplace("reverse_edge_lookups", uintJson(telemetry.v3_lazy_reverse_lookups));
    object.emplace("arc_checks", uintJson(telemetry.v3_lazy_arc_checks));
    object.emplace("emitted_entries", uintJson(telemetry.v3_lazy_relation_entries));
    object.emplace("cache_capacity_bytes", uintJson(telemetry.v3_lazy_cache_capacity_bytes));
    object.emplace("extension_probes", uintJson(telemetry.v3_lazy_extension_probes));
    object.emplace("interval_intersections", uintJson(telemetry.v3_lazy_interval_intersections));
    return Json::object(std::move(object));
}

Json selectedParentJson(const FixedPlan& plan) {
    Json::Array result;
    result.reserve(plan.parent.size());
    for (std::size_t vertex = 0; vertex < plan.parent.size(); ++vertex) {
        if (static_cast<int>(vertex) == plan.root) {
            result.push_back(stringJson("root"));
        } else {
            const int parent = plan.parent[vertex];
            if (parent < 0) {
                throw ClassifiedV3Error(
                    kV3PlannerNotIntegrated,
                    "selected plan contains negative nonroot parent");
            }
            result.push_back(uintJson(static_cast<std::uint64_t>(parent)));
        }
    }
    return Json::array(std::move(result));
}

Json selectedOrderJson(const FixedPlan& plan) {
    Json::Array result;
    result.reserve(plan.order.size());
    for (int vertex : plan.order) {
        if (vertex < 0) {
            throw ClassifiedV3Error(
                kV3PlannerNotIntegrated,
                "selected plan contains negative vertex");
        }
        result.push_back(uintJson(static_cast<std::uint64_t>(vertex)));
    }
    return Json::array(std::move(result));
}

Json executionBinding(
    const V3ExecutionSpec& spec,
    const V3DataInputResult& data,
    const std::shared_ptr<const ValidatedGraphSnapshot>& snapshot,
    const FixedPlan& selected_plan,
    const ArmExecution& arm,
    std::uint64_t selected_tree_fingerprint) {
    Json::Object object;
    object.emplace("active_vertices", uintJson(data.active_vertices));
    object.emplace("backend", stringJson("tdtree_cpu"));
    object.emplace("data_logical_fingerprint", uintJson(snapshot->dataFingerprint()));
    object.emplace("executor_semantics_id", stringJson(
        "tdtree_exact_durable_injective_count_v1"));
    object.emplace("formula_id", stringJson(kV3SelectivityFormulaId));
    object.emplace("mapping_digest_id", stringJson(
        "external_id_query_vertex_order_splitmix64_xor_sum_v1"));
    object.emplace("match_output", stringJson("count_only"));
    object.emplace("minimum_duration", uintJson(
        static_cast<std::uint64_t>(spec.minimum_duration)));
    object.emplace("query_logical_fingerprint", uintJson(snapshot->queryFingerprint()));
    object.emplace("retained_unique_directed_edges", uintJson(
        data.retained_unique_directed_edges));
    object.emplace("selected_order", selectedOrderJson(selected_plan));
    object.emplace("selected_parent", selectedParentJson(selected_plan));
    object.emplace("selected_plan_fingerprint", uintJson(arm.selected_plan_fingerprint));
    object.emplace("selected_root", uintJson(static_cast<std::uint64_t>(selected_plan.root)));
    object.emplace("selected_tree_fingerprint", uintJson(selected_tree_fingerprint));
    object.emplace("schema", stringJson("icde2027_v3_tdtree_execution_binding"));
    object.emplace("schema_version", uintJson(1));
    return Json::object(std::move(object));
}

struct TimerEvidence {
    std::uint64_t raw_read_ns = 0;
    std::uint64_t internal_filter_ns = 0;
    std::uint64_t query_read_ns = 0;
    std::uint64_t graph_validation_ns = 0;
    std::uint64_t planner_ns = 0;
    std::uint64_t execution_ns = 0;
    std::uint64_t controllable_ns = 0;
    std::uint64_t wall_ns = 0;
};

Json buildMatcherPayload(
    const ValidatedV3RunContract& authority,
    const V3ExecutionSpec& spec,
    const V3EnvironmentObservation& environment,
    const V3DataInputResult& data,
    const V3QueryInputResult& query,
    const GraphSnapshotSealResult& sealed,
    const FixedPlan& selected_plan,
    const ArmExecution& arm,
    const MatchSummary& match,
    const TimerEvidence& timers,
    std::uint64_t relation_entries,
    std::uint64_t root_plus_work,
    std::uint64_t selected_tree_fingerprint,
    const std::string& execution_binding_sha256) {
    const Json& contract = authority.contract.value;
    Json::Object root;
    root.emplace("schema", stringJson("icde2027_v3_matcher_receipt"));
    root.emplace("schema_version", uintJson(1));
    root.emplace("identity", contract.at("identity"));
    root.emplace("authorization", contract.at("authorization"));

    Json::Object build;
    build.emplace("formula_id", stringJson(kV3SelectivityFormulaId));
    build.emplace("hashes", contract.at("build"));
    build.emplace("implementation_generation", stringJson(kImplementationGeneration));
    build.emplace("planner_generation", stringJson(kPlannerGeneration));
    root.emplace("build", Json::object(std::move(build)));

    Json::Array argv;
    argv.push_back(stringJson("td_tree"));
    argv.push_back(stringJson("--v3-run-contract"));
    argv.push_back(stringJson(spec.run_contract_path));
    argv.push_back(stringJson("--v3-authorization"));
    argv.push_back(stringJson(spec.authorization_path));
    argv.push_back(stringJson("--v3-receipt"));
    argv.push_back(stringJson(spec.matcher_receipt_path));
    Json::Object command;
    command.emplace("canonical_argv", Json::array(std::move(argv)));
    command.emplace("forbidden_override_count", uintJson(0));
    command.emplace("legacy_mode", boolJson(false));
    root.emplace("command", Json::object(std::move(command)));

    Json::Object input;
    input.emplace("active_vertices", uintJson(data.active_vertices));
    input.emplace("data_expected_bytes", uintJson(spec.data.expected_bytes));
    input.emplace("data_expected_sha256", stringJson(spec.data.expected_sha256));
    input.emplace("data_kind", stringJson("full_raw"));
    input.emplace("data_observed_bytes", uintJson(data.observed_bytes));
    input.emplace("data_observed_sha256", stringJson(data.observed_sha256));
    input.emplace("data_path", stringJson(spec.data.path));
    input.emplace("internal_filter", stringJson("consecutive_pair_on"));
    input.emplace("label_mode", stringJson("stable_id"));
    input.emplace("label_seed", uintJson(spec.label_seed));
    input.emplace("match_output", stringJson("count_only"));
    input.emplace("minimum_duration", uintJson(
        static_cast<std::uint64_t>(spec.minimum_duration)));
    input.emplace("occurrences", uintJson(data.occurrences));
    input.emplace("query_edges", uintJson(spec.query_parsed.edges));
    input.emplace("query_expected_bytes", uintJson(spec.query.expected_bytes));
    input.emplace("query_expected_sha256", stringJson(spec.query.expected_sha256));
    input.emplace("query_observed_bytes", uintJson(query.observed_bytes));
    input.emplace("query_observed_sha256", stringJson(query.observed_sha256));
    input.emplace("query_path", stringJson(spec.query.path));
    input.emplace("query_vertices", uintJson(spec.query_parsed.vertices));
    input.emplace("retained_unique_directed_edges", uintJson(
        data.retained_unique_directed_edges));
    input.emplace("unique_directed_edges", uintJson(
        data.unique_directed_edges));
    root.emplace("input", Json::object(std::move(input)));

    Json::Object env;
    env.emplace("affinity_cpu_set", uintArray(environment.affinity_cpu_set));
    env.emplace("cgroup_id", stringJson(environment.cgroup_id));
    env.emplace("cgroup_path", stringJson(environment.cgroup_path));
    env.emplace("memory_max_bytes", uintJson(environment.memory_max_bytes));
    env.emplace("memory_swap_max_bytes", uintJson(environment.memory_swap_max_bytes));
    env.emplace("smt_active", uintJson(environment.smt_active));
    env.emplace("threads_created", uintJson(sizeToU64(
        match.threads_created, "threads created")));
    env.emplace("threads_requested", uintJson(sizeToU64(
        match.threads_requested, "threads requested")));
    env.emplace("threads_used", uintJson(sizeToU64(
        match.threads_used, "threads used")));
    env.emplace("worker_count_semantics", stringJson("executor_pool_size"));
    env.emplace("workers_with_tasks", uintJson(sizeToU64(
        match.workers_with_tasks, "workers with tasks")));
    root.emplace("environment_observed", Json::object(std::move(env)));

    Json::Object tokens;
    tokens.emplace("cover_snapshot_token", uintJson(sealed.snapshot->snapshotToken()));
    tokens.emplace("incumbent_snapshot_token", uintJson(sealed.snapshot->snapshotToken()));
    tokens.emplace("selected_tree_snapshot_token", uintJson(sealed.snapshot->snapshotToken()));
    Json::Object snapshot;
    snapshot.emplace("authority_tokens", Json::object(std::move(tokens)));
    snapshot.emplace("data_fingerprint_rescans_after_seal", uintJson(0));
    snapshot.emplace("data_logical_fingerprint", uintJson(sealed.snapshot->dataFingerprint()));
    snapshot.emplace("frozen_budget", contract.at("numeric_caps").at("graph_validation"));
    snapshot.emplace("materializer_graph_validation_rescans", uintJson(0));
    snapshot.emplace("query_fingerprint_rescans_after_seal", uintJson(0));
    snapshot.emplace("query_logical_fingerprint", uintJson(sealed.snapshot->queryFingerprint()));
    snapshot.emplace("quote_graph_validation_rescans", uintJson(0));
    snapshot.emplace("snapshot_token", uintJson(sealed.snapshot->snapshotToken()));
    snapshot.emplace("status", stringJson("complete"));
    snapshot.emplace("validation_receipt", graphValidationReceiptJson(sealed.receipt));
    root.emplace("snapshot", Json::object(std::move(snapshot)));

    Json::Object counters;
    counters.emplace("candidate_build_quote_violations", uintJson(
        arm.telemetry.v3_candidate_build_quote_violations));
    counters.emplace("complete_plan_proposals", uintJson(
        arm.telemetry.v3_complete_plan_proposals));
    counters.emplace("extra_full_q_by_n_domain_scans", uintJson(
        arm.telemetry.extra_full_q_by_n_domain_scans));
    counters.emplace("global_arc_consistency_passes", uintJson(
        arm.telemetry.global_arc_consistency_passes));
    counters.emplace("global_query_edge_catalogue_builds", uintJson(
        arm.telemetry.global_query_edge_catalogue_builds));
    counters.emplace("implicit_parent_relation_lookups", uintJson(
        arm.telemetry.v3_implicit_parent_relation_lookups));
    counters.emplace("materialized_complete_plans", uintJson(
        arm.telemetry.v3_materialized_complete_plans));
    counters.emplace("maximum_prefix_samples_observed", uintJson(
        arm.telemetry.v3_maximum_prefix_samples_observed));
    counters.emplace("nonincumbent_plan_proposals", uintJson(
        arm.telemetry.v3_nonincumbent_plan_proposals));
    counters.emplace("nonincumbent_selection_count", uintJson(
        arm.decision == V3TerminalDecision::CandidateSelected ? 1 : 0));
    counters.emplace("partial_candidate_tree_handoffs", uintJson(
        arm.telemetry.v3_partial_candidate_tree_handoffs));
    counters.emplace("proposal_states_expanded", uintJson(
        arm.telemetry.v3_proposal_states_expanded));
    counters.emplace("quote_cover_value_visits", uintJson(
        arm.telemetry.v3_quote_cover_value_visits));
    counters.emplace("rescored_complete_plans", uintJson(
        arm.telemetry.v3_rescored_complete_plans));
    counters.emplace("scored_plan_proposals", uintJson(
        arm.telemetry.v3_scored_plan_proposals));
    Json::Object planner;
    planner.emplace("accounting_convention_id", stringJson(kAccountingConvention));
    planner.emplace("arm_id", stringJson(spec.arm_id));
    planner.emplace("counters", Json::object(std::move(counters)));
    planner.emplace("decision", stringJson(v3TerminalDecisionName(arm.decision)));
    planner.emplace("planner_policy", stringJson(spec.planner_policy));
    planner.emplace("predicted_candidate_total", uintJson(arm.predicted_candidate_total));
    planner.emplace("predicted_incumbent_total", uintJson(arm.predicted_incumbent_total));
    planner.emplace("profitability_gate", stringJson(spec.profitability_gate));
    planner.emplace("profitability_ratio_denominator", uintJson(10));
    planner.emplace("profitability_ratio_numerator", uintJson(9));
    planner.emplace("reason", stringJson(plannerReason(arm.decision)));
    root.emplace("planner", Json::object(std::move(planner)));

    Json::Object plan;
    plan.emplace("external_receipt_plan_fingerprint", uintJson(
        arm.receipt->plan().fingerprint));
    plan.emplace("incumbent_plan_fingerprint", uintJson(
        arm.incumbent_plan_fingerprint));
    plan.emplace("legality", stringJson("complete_legal_fixed_plan"));
    plan.emplace("selected_order", selectedOrderJson(selected_plan));
    plan.emplace("selected_parent", selectedParentJson(selected_plan));
    plan.emplace("selected_plan_fingerprint", uintJson(
        arm.selected_plan_fingerprint));
    plan.emplace("selected_root", uintJson(
        static_cast<std::uint64_t>(selected_plan.root)));
    plan.emplace("selected_tree_fingerprint", uintJson(selected_tree_fingerprint));
    plan.emplace("selected_tree_plan_fingerprint", uintJson(
        arm.tree->executionPlanFingerprint()));
    root.emplace("plan", Json::object(std::move(plan)));

    Json::Object candidate;
    candidate.emplace("actual", workVectorJson(arm.candidate_build.actual));
    candidate.emplace("frozen_budget", contract.at("numeric_caps").at("candidate_build"));
    candidate.emplace("partial_handoffs", uintJson(arm.candidate_build.partial_handoffs));
    candidate.emplace("quote_dominance_verified", boolJson(
        arm.candidate_build.quote_dominance_verified));
    candidate.emplace("sealed_quote", workVectorJson(arm.candidate_build.sealed_quote));
    root.emplace("candidate_build", Json::object(std::move(candidate)));

    Json::Object lazy;
    lazy.emplace("actual", lazyActualJson(arm.telemetry));
    lazy.emplace("frozen_budget", contract.at("numeric_caps").at("lazy"));
    root.emplace("lazy", Json::object(std::move(lazy)));

    Json::Object metadata;
    metadata.emplace("actual_bytes", uintJson(arm.receipt->observedPayloadBytes()));
    metadata.emplace("budget_bytes", uintJson(spec.metadata_payload_bytes));
    metadata.emplace("quoted_bytes", uintJson(arm.receipt->payloadBytes()));
    metadata.emplace("receipt_tokens_actual", uintJson(1));
    metadata.emplace("receipt_tokens_budget", uintJson(spec.metadata_receipt_tokens));
    root.emplace("metadata", Json::object(std::move(metadata)));

    Json::Object execution;
    execution.emplace("backend", stringJson("tdtree_cpu"));
    execution.emplace("count_overflow", boolJson(false));
    execution.emplace("execution_binding_sha256", stringJson(execution_binding_sha256));
    execution.emplace("mapping_digest_sum_hex", stringJson(hex64(match.mapping_digest_sum)));
    execution.emplace("mapping_digest_xor_hex", stringJson(hex64(match.mapping_digest_xor)));
    execution.emplace("match_count", uintJson(match.match_count));
    execution.emplace("relation_entries", uintJson(relation_entries));
    execution.emplace("root_candidates", uintJson(sizeToU64(
        match.root_candidate_count, "root candidates")));
    execution.emplace("root_plus_raw_extension_work", uintJson(root_plus_work));
    root.emplace("execution", Json::object(std::move(execution)));

    Json::Object nested;
    nested.emplace("lazy_prefix_sample_cap", uintJson(
        arm.telemetry.v3_lazy_prefix_sample_cap));
    nested.emplace("lazy_maximum_prefix_samples_observed", uintJson(
        arm.telemetry.v3_lazy_maximum_prefix_samples_observed));
    nested.emplace("rescore_prefix_sample_cap", uintJson(
        arm.telemetry.v3_rescore_prefix_sample_cap));
    nested.emplace("rescore_maximum_prefix_samples_observed", uintJson(
        arm.telemetry.v3_rescore_maximum_prefix_samples_observed));
    nested.emplace("derived_lazy_block_request_cap", uintJson(
        spec.lazy_budget.maximum_block_requests));
    nested.emplace("derived_lazy_cache_entry_cap", uintJson(
        spec.lazy_budget.maximum_cache_entries));
    nested.emplace("derived_rescore_block_request_cap", uintJson(
        spec.rescore_budget.maximum_block_requests));
    nested.emplace("derivation_id", stringJson(
        "contract_q_times_prefix_times_complete_v1"));
    nested.emplace("numeric_cap_binding_mode", stringJson(
        numericCapBindingModeName(spec.numeric_cap_binding_mode)));
    Json::Object timer;
    timer.emplace("controllableQueryProcessing_ns", uintJson(timers.controllable_ns));
    timer.emplace("execution_ns", uintJson(timers.execution_ns));
    timer.emplace("graphValidation_ns", uintJson(timers.graph_validation_ns));
    timer.emplace("internalFilter_ns", uintJson(timers.internal_filter_ns));
    timer.emplace("matcherObservedWall_ns", uintJson(timers.wall_ns));
    timer.emplace("nested_diagnostics", Json::object(std::move(nested)));
    timer.emplace("planner_ns", uintJson(timers.planner_ns));
    timer.emplace("queryRead_ns", uintJson(timers.query_read_ns));
    timer.emplace("rawRead_ns", uintJson(timers.raw_read_ns));
    root.emplace("timers_ns", Json::object(std::move(timer)));

    Json::Object terminal;
    terminal.emplace("phase", stringJson("complete"));
    terminal.emplace("process_exit_code_expected", uintJson(0));
    terminal.emplace("reason", stringJson(
        "completed exact count and canonical receipt publication"));
    terminal.emplace("semantic_or_resource_failure", boolJson(false));
    terminal.emplace("status", stringJson("complete_success"));
    terminal.emplace("timing_eligible", boolJson(true));
    root.emplace("terminal", Json::object(std::move(terminal)));
    return Json::object(std::move(root));
}

int executeCore(
    const ValidatedV3RunContract& authority,
    const V3ExecutionSpec& spec,
    const V3EnvironmentObservation& environment,
    bool verify_executable,
    Clock::time_point matcher_start) {
    if (spec.matcher_receipt_path.empty()) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError, "matcher receipt path is empty");
    }
    requireV3Environment(environment, spec.resources);
    if (verify_executable) verifyBuildTrustAnchors(authority, spec);
    V3DataInputResult data = readV3TemporalData(
        spec.data, spec.label_seed, true);
    if (!data.complete()) {
        throw ClassifiedV3Error(
            inputExitCode(data.status),
            std::string("data input ") + v3InputStatusName(data.status) +
                ": " + data.detail);
    }
    V3QueryInputResult query = readV3Query(spec.query, spec.query_parsed);
    if (!query.complete()) {
        throw ClassifiedV3Error(
            inputExitCode(query.status),
            std::string("query input ") + v3InputStatusName(query.status) +
                ": " + query.detail);
    }

    const Clock::time_point c0 = Clock::now();
    GraphSnapshotSealResult sealed = ValidatedGraphSnapshot::seal(
        data.graph, query.graph, spec.graph_validation_budget);
    const Clock::time_point c1 = Clock::now();
    if (!sealed.complete() || !sealed.snapshot ||
        sealed.snapshot->snapshotToken() == 0 ||
        sealed.snapshot->dataFingerprint() == 0 ||
        sealed.snapshot->queryFingerprint() == 0 ||
        sealed.receipt.graph_validation_calls != 1 ||
        sealed.receipt.storage_quote_passes != 1 ||
        sealed.receipt.storage_accounting_passes != 2 ||
        sealed.receipt.storage_quote_graphs_visited != 2 ||
        sealed.receipt.storage_accounting_graphs_visited != 4 ||
        !graphReceiptInvariant(
            sealed.receipt, spec.graph_validation_budget)) {
        throw ClassifiedV3Error(
            kV3SnapshotValidationError,
            "immutable graph snapshot did not complete exact validation");
    }

    // The seal owns fresh private storage.  Release both mutable parser
    // graphs before incumbent/planner work so the hard seal-peak accounting
    // does not turn into an unreported full-run double-copy footprint.
    data.graph = Graph{};
    query.graph = Graph{};
    const SourceGraphReleaseReceiptV1 source_data_release =
        sourceGraphReleaseReceiptV1(
            data.graph, "source_data_graph");
    const SourceGraphReleaseReceiptV1 source_query_release =
        sourceGraphReleaseReceiptV1(
            query.graph, "source_query_graph");
    if (!source_data_release.invariantHolds() ||
        !source_query_release.invariantHolds()) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "mutable parser graph owners were not exactly released");
    }

    const auto counts = labelCounts(sealed.snapshot->data());
    const auto means = labelMeanActivity(sealed.snapshot->data());
    QueryDecomposition decomposition = decomposeQuery(
        sealed.snapshot->query(), counts, means, SelectivityMode::Paper);
    if (!decomposition.connected || decomposition.root < 0 ||
        decomposition.selectivity_mode != SelectivityMode::Paper ||
        selectivityFormulaId(decomposition.selectivity_mode) !=
            std::string(kV3SelectivityFormulaId)) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "corrected Paper incumbent decomposition is incomplete");
    }
    FixedPlan incumbent_plan = makeFixedPlan(*sealed.snapshot, decomposition);
    if (!validateFixedPlan(*sealed.snapshot, incumbent_plan).valid) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated, "corrected Paper fixed plan is illegal");
    }
    std::unique_ptr<TDTree> incumbent = TDTree::buildValidatedIncumbent(
        sealed.snapshot, decomposition, spec.minimum_duration);
    if (!incumbent ||
        !incumbent->hasCorrectedPaperIncumbentOrigin(incumbent_plan) ||
        !incumbent->isBoundToValidatedSnapshot(sealed.snapshot) ||
        incumbent->executionPlanFingerprint() != incumbent_plan.fingerprint) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "corrected Paper incumbent tree origin/owner mismatch");
    }
    ArmExecution arm = spec.arm == V3Arm::IncumbentI
        ? executeIncumbentArm(
            sealed.snapshot, std::move(incumbent), incumbent_plan, spec)
        : executePlannerArm(
            sealed.snapshot, std::move(incumbent), incumbent_plan, spec,
            source_data_release, source_query_release);
    const Clock::time_point c2 = Clock::now();
    if (!arm.tree || !arm.receipt ||
        !arm.tree->isBoundToValidatedSnapshot(sealed.snapshot) ||
        arm.receipt->snapshotToken() != sealed.snapshot->snapshotToken() ||
        arm.receipt->plan().fingerprint != arm.selected_plan_fingerprint ||
        arm.tree->executionPlanFingerprint() != arm.selected_plan_fingerprint ||
        !arm.candidate_build.invariantHolds() ||
        !plannerCapsInvariant(arm, spec)) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "selected tree/external receipt/snapshot authority mismatch");
    }
    const FixedPlan selected_plan = arm.receipt->plan();
    if (!validateFixedPlan(*sealed.snapshot, selected_plan).valid) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated, "selected external fixed plan is illegal");
    }
    const std::uint64_t selected_tree_fingerprint =
        arm.tree->candidateContentFingerprint();
    if (selected_tree_fingerprint == 0) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "selected executable candidate tree fingerprint is zero");
    }
    const std::size_t threads = checkedSize(
        spec.resources.threads_used_required,
        "resources.threads_used_required",
        kV3ExecutionExactnessError);
    MatchSummary match = arm.tree->countMatchesV3(threads);
    const std::uint64_t relation_entries = sizeToU64(
        arm.tree->candidateRelationCount(), "candidate relation entries");
    const std::uint64_t root_count = sizeToU64(
        arm.tree->rootCandidates().size(), "root candidate count");
    if (match.threads_requested != threads ||
        match.threads_created != threads || match.threads_used != threads ||
        match.workers_with_tasks > threads ||
        !match.result_digest_collected || match.count_overflow ||
        match.completed_matches != match.match_count ||
        match.root_candidate_count != arm.tree->rootCandidates().size()) {
        throw ClassifiedV3Error(
            kV3ExecutionExactnessError,
            "file-free exact matcher summary invariant failed");
    }
    const std::uint64_t root_plus_work = checkedAdd(
        root_count, match.generated_candidates,
        "root plus raw extension work", kV3ExecutionExactnessError);
    Json binding = executionBinding(
        spec, data, sealed.snapshot, selected_plan, arm,
        selected_tree_fingerprint);
    const std::string binding_hash = sha256Hex(canonicalJsonBytes(binding));
    const Clock::time_point c3 = Clock::now();
    const Clock::time_point matcher_end = Clock::now();

    TimerEvidence timers;
    timers.raw_read_ns = data.raw_read_ns;
    timers.internal_filter_ns = data.internal_filter_ns;
    timers.query_read_ns = query.query_read_ns;
    timers.graph_validation_ns = elapsedNs(c0, c1, "graph validation timer");
    timers.planner_ns = elapsedNs(c1, c2, "planner timer");
    timers.execution_ns = elapsedNs(c2, c3, "execution timer");
    timers.controllable_ns = checkedAdd(
        checkedAdd(
            timers.graph_validation_ns, timers.planner_ns,
            "controllable graph+planner timer",
            kV3ExecutionExactnessError),
        timers.execution_ns,
        "controllable timer", kV3ExecutionExactnessError);
    if (timers.controllable_ns != elapsedNs(c0, c3, "controllable boundary")) {
        throw ClassifiedV3Error(
            kV3ExecutionExactnessError,
            "controllable timer telescoping equality failed");
    }
    timers.wall_ns = elapsedNs(
        matcher_start, matcher_end, "matcher observed wall timer");
    const std::uint64_t required_wall = checkedAdd(
        checkedAdd(
            checkedAdd(
                timers.raw_read_ns, timers.internal_filter_ns,
                "input read/filter timer", kV3ExecutionExactnessError),
            timers.query_read_ns,
            "input total timer", kV3ExecutionExactnessError),
        timers.controllable_ns,
        "matcher required wall timer", kV3ExecutionExactnessError);
    if (timers.raw_read_ns == 0 || timers.internal_filter_ns == 0 ||
        timers.query_read_ns == 0 || timers.wall_ns < required_wall) {
        throw ClassifiedV3Error(
            kV3ExecutionExactnessError,
            "top-level timer positivity/lower-bound invariant failed");
    }

    // Budget dominance is rechecked at the assembly boundary.  The payload
    // field is ceiling P, not the P+H reservation envelope.  The immutable H
    // field is consumed only by the separate global E+Q_B+H<=M gate in the
    // formula-cap production path.
    const V3CandidateBuildWorkVector& quote = arm.candidate_build.sealed_quote;
    const CandidateBuildBudget& budget = spec.candidate_build_budget;
    if (quote.adjacency_probes > budget.adjacency_probes ||
        quote.reverse_edge_lookups > budget.reverse_edge_lookups ||
        quote.arc_checks > budget.arc_checks ||
        quote.relation_entries > budget.relation_entries ||
        quote.candidate_value_occurrences > budget.candidate_value_occurrences ||
        quote.blocks > budget.blocks ||
        quote.semijoin_entry_visits > budget.semijoin_entry_visits ||
        quote.index_entries > budget.index_entries ||
        quote.payload_memory_bytes > budget.payload_memory_bytes ||
        arm.telemetry.v3_materialized_complete_plans >
            budget.candidate_tree_tokens) {
        throw ClassifiedV3Error(
            kV3PlannerNotIntegrated,
            "candidate build quote exceeds frozen payload budget");
    }

    Json payload = buildMatcherPayload(
        authority, spec, environment, data, query, sealed, selected_plan,
        arm, match, timers, relation_entries, root_plus_work,
        selected_tree_fingerprint, binding_hash);
    V3SealedArtifact artifact = sealV3CanonicalArtifact(std::move(payload));
    V3PublishResult published = publishV3CanonicalNoReplace(
        spec.matcher_receipt_path, artifact);
    if (!published.complete()) {
        throw ClassifiedV3Error(
            kV3ReceiptIoError,
            std::string("matcher receipt publication ") +
                v3PublishStatusName(published.status) + ": " +
                published.detail);
    }
    return kV3CompleteSuccess;
}

} // namespace

ClassifiedV3Error::ClassifiedV3Error(
    int exit_code,
    std::string message)
    : std::runtime_error(std::move(message)), exit_code_(exit_code) {}

int ClassifiedV3Error::exitCode() const noexcept { return exit_code_; }

V3ExecutionSpec projectV3ExecutionSpec(
    const ValidatedV3RunContract& authority) {
    const Json& contract = authority.contract.value;
    const Json& identity = contract.at("identity");
    const Json& algorithm = contract.at("algorithm");
    const Json& input = contract.at("input");
    const Json& resources = contract.at("resources");
    const Json& caps = contract.at("numeric_caps");
    const Json& output = contract.at("output");
    V3ExecutionSpec spec;
    spec.ordinal = uintField(identity, "ordinal", "identity");
    if (spec.ordinal != authority.ordinal) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError, "typed ordinal differs from authority");
    }
    spec.arm_id = stringField(identity, "arm_id", "identity");
    if (spec.arm_id == "v3dev_incumbent_16t") {
        spec.arm = V3Arm::IncumbentI;
    } else if (spec.arm_id == "v3dev_cover_lazy_gate_on_16t") {
        spec.arm = V3Arm::GateOnO;
    } else if (spec.arm_id == "v3dev_cover_lazy_gate_off_16t") {
        spec.arm = V3Arm::GateOffF;
    } else {
        throw ClassifiedV3Error(kV3CliOrSchemaError, "unknown V3 arm id");
    }
    if (stringField(algorithm, "formula_id", "algorithm") !=
            kV3SelectivityFormulaId ||
        stringField(algorithm, "internal_filter", "algorithm") !=
            "consecutive_pair_on" ||
        stringField(algorithm, "label_mode", "algorithm") != "stable_id" ||
        stringField(algorithm, "match_output", "algorithm") != "count_only" ||
        uintField(algorithm, "profitability_ratio_numerator", "algorithm") != 9 ||
        uintField(algorithm, "profitability_ratio_denominator", "algorithm") != 10) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError, "typed algorithm generation drift");
    }
    spec.minimum_duration = checkedInt(
        uintField(algorithm, "minimum_duration", "algorithm"),
        "algorithm.minimum_duration");
    if (spec.minimum_duration < 1) {
        throw ClassifiedV3Error(
            kV3CliOrSchemaError, "minimum duration must be positive");
    }
    spec.label_seed = checkedU32(
        uintField(algorithm, "label_seed", "algorithm"),
        "algorithm.label_seed");
    spec.planner_policy = stringField(
        algorithm, "planner_policy", "algorithm");
    spec.profitability_gate = stringField(
        algorithm, "profitability_gate", "algorithm");
    if (spec.arm == V3Arm::IncumbentI) {
        if (spec.planner_policy != "validated_incumbent_only" ||
            spec.profitability_gate != "not_applicable") {
            throw ClassifiedV3Error(
                kV3CliOrSchemaError, "incumbent arm policy drift");
        }
    } else {
        if (spec.planner_policy != "cover_lazy_v3" ||
            (spec.arm == V3Arm::GateOnO &&
             spec.profitability_gate != "enabled_exact_nine_tenths") ||
            (spec.arm == V3Arm::GateOffF &&
             spec.profitability_gate != "disabled_attribution_only")) {
            throw ClassifiedV3Error(
                kV3CliOrSchemaError, "planner arm policy/gate drift");
        }
        spec.profitability_gate_enabled = spec.arm == V3Arm::GateOnO;
    }
    spec.data = {
        stringField(input, "data_path", "input"),
        uintField(input, "data_bytes", "input"),
        stringField(input, "data_sha256", "input")};
    spec.query = {
        stringField(input, "query_path", "input"),
        uintField(input, "query_bytes", "input"),
        stringField(input, "query_sha256", "input")};
    spec.query_parsed.vertices = uintField(input, "query_vertices", "input");
    spec.query_parsed.edges = uintField(input, "query_edges", "input");
    for (const Json& label : arrayField(
             input, "query_labels", "input")) {
        if (!label.isString()) {
            throw ClassifiedV3Error(
                kV3CliOrSchemaError, "query label authority is not a string");
        }
        spec.query_parsed.labels.push_back(label.string());
    }
    for (const Json& edge : arrayField(
             input, "query_edges_directed", "input")) {
        if (!edge.isArray() || edge.arrayValue().size() != 2U ||
            !edge.arrayValue()[0].isUnsignedInteger() ||
            !edge.arrayValue()[1].isUnsignedInteger()) {
            throw ClassifiedV3Error(
                kV3CliOrSchemaError, "query edge authority shape drift");
        }
        spec.query_parsed.directed_edges.emplace_back(
            edge.arrayValue()[0].unsignedInteger(),
            edge.arrayValue()[1].unsignedInteger());
    }
    for (const Json& cpu : arrayField(
             resources, "affinity_cpu_set", "resources")) {
        if (!cpu.isUnsignedInteger()) {
            throw ClassifiedV3Error(
                kV3CliOrSchemaError, "affinity CPU must be uint64");
        }
        spec.resources.affinity_cpu_set.push_back(cpu.unsignedInteger());
    }
    spec.resources.memory_max_bytes = uintField(
        resources, "memory_max_bytes", "resources");
    spec.resources.memory_swap_max_bytes = uintField(
        resources, "memory_swap_max_bytes", "resources");
    spec.resources.smt_active_required = uintField(
        resources, "smt_active_required", "resources");
    spec.resources.threads_requested = uintField(
        resources, "threads_requested", "resources");
    spec.resources.threads_used_required = uintField(
        resources, "threads_used_required", "resources");
    spec.resources.timeout_seconds = uintField(
        resources, "timeout_seconds", "resources");

    const Json& graph = caps.at("graph_validation");
#define DSM_V3_GRAPH_CAP(name) spec.graph_validation_budget.name = \
    uintField(graph, #name, "numeric_caps.graph_validation")
    DSM_V3_GRAPH_CAP(data_vertices);
    DSM_V3_GRAPH_CAP(query_vertices);
    DSM_V3_GRAPH_CAP(combined_outgoing_adjacency_entries);
    DSM_V3_GRAPH_CAP(combined_incoming_adjacency_entries);
    DSM_V3_GRAPH_CAP(temporal_edges);
    DSM_V3_GRAPH_CAP(interval_records);
    DSM_V3_GRAPH_CAP(derived_incident_interval_visits);
    DSM_V3_GRAPH_CAP(derived_neighbor_label_updates);
    DSM_V3_GRAPH_CAP(snapshot_storage_bytes);
    DSM_V3_GRAPH_CAP(temporary_validation_bytes);
    DSM_V3_GRAPH_CAP(seal_peak_live_bytes);
#undef DSM_V3_GRAPH_CAP
    const Json& candidate = caps.at("candidate_build");
#define DSM_V3_CANDIDATE_CAP(name) spec.candidate_build_budget.name = \
    uintField(candidate, #name, "numeric_caps.candidate_build")
    DSM_V3_CANDIDATE_CAP(adjacency_probes);
    DSM_V3_CANDIDATE_CAP(reverse_edge_lookups);
    DSM_V3_CANDIDATE_CAP(arc_checks);
    DSM_V3_CANDIDATE_CAP(relation_entries);
    DSM_V3_CANDIDATE_CAP(candidate_value_occurrences);
    DSM_V3_CANDIDATE_CAP(blocks);
    DSM_V3_CANDIDATE_CAP(semijoin_entry_visits);
    DSM_V3_CANDIDATE_CAP(index_entries);
    DSM_V3_CANDIDATE_CAP(payload_memory_bytes);
    DSM_V3_CANDIDATE_CAP(fixed_memory_headroom_bytes);
    DSM_V3_CANDIDATE_CAP(candidate_tree_tokens);
#undef DSM_V3_CANDIDATE_CAP
    const Json& planner = caps.at("planner");
    spec.proposal_budget.maximum_states_expanded = uintField(
        planner, "proposal_state_expansions", "numeric_caps.planner");
    spec.proposal_budget.maximum_complete_plans = uintField(
        planner, "complete_proposal_cap", "numeric_caps.planner");
    spec.proposal_budget.maximum_quote_cover_value_visits = uintField(
        planner, "quote_cover_value_visits", "numeric_caps.planner");
    const std::uint64_t prefix = uintField(
        planner, "prefix_samples_per_plan", "numeric_caps.planner");
    spec.lazy_budget.prefix_sample_cap = checkedSize(
        prefix, "planner.prefix_samples_per_plan",
        kV3PlannerNotIntegrated);
    spec.rescore_budget.prefix_sample_cap = spec.lazy_budget.prefix_sample_cap;
    const Json& lazy = caps.at("lazy");
    spec.lazy_budget.maximum_adjacency_probes = uintField(
        lazy, "adjacency_probes", "numeric_caps.lazy");
    spec.lazy_budget.maximum_reverse_lookups = uintField(
        lazy, "reverse_edge_lookups", "numeric_caps.lazy");
    spec.lazy_budget.maximum_arc_checks = uintField(
        lazy, "arc_checks", "numeric_caps.lazy");
    spec.lazy_budget.maximum_relation_entries = uintField(
        lazy, "emitted_entries", "numeric_caps.lazy");
    spec.lazy_budget.maximum_cache_capacity_bytes = uintField(
        lazy, "cache_capacity_bytes", "numeric_caps.lazy");
    spec.lazy_budget.maximum_extension_probes = uintField(
        lazy, "extension_probes", "numeric_caps.lazy");
    spec.lazy_budget.maximum_interval_intersections = uintField(
        lazy, "interval_intersections", "numeric_caps.lazy");
    spec.rescore_budget.maximum_extension_probes =
        spec.lazy_budget.maximum_extension_probes;
    spec.rescore_budget.maximum_interval_intersections =
        spec.lazy_budget.maximum_interval_intersections;
    const std::uint64_t q = std::max<std::uint64_t>(
        1, spec.query_parsed.vertices);
    const std::uint64_t per_plan_blocks = checkedMultiply(
        prefix, q, "derived prefix-query cap");
    spec.lazy_budget.maximum_block_requests = checkedMultiply(
        per_plan_blocks,
        spec.proposal_budget.maximum_complete_plans,
        "derived lazy block request cap");
    spec.lazy_budget.maximum_cache_entries =
        spec.lazy_budget.maximum_block_requests;
    spec.rescore_budget.maximum_block_requests = per_plan_blocks;
    const Json& metadata = caps.at("metadata");
    spec.metadata_payload_bytes = uintField(
        metadata, "payload_bytes", "numeric_caps.metadata");
    spec.metadata_receipt_tokens = uintField(
        metadata, "receipt_tokens", "numeric_caps.metadata");
    spec.run_contract_path = stringField(
        output, "run_contract_path", "output");
    spec.authorization_path = stringField(
        contract.at("authorization"), "authorization_path", "authorization");
    spec.matcher_receipt_path = stringField(
        output, "matcher_receipt_path", "output");
    spec.executable_sha256 = stringField(
        contract.at("build"), "executable_sha256", "build");
    spec.source_bundle_sha256 = stringField(
        contract.at("build"), "source_bundle_sha256", "build");
    spec.parameter_bundle_sha256 = stringField(
        contract.at("build"), "parameter_bundle_sha256", "build");
    spec.compiler_identity_sha256 = stringField(
        contract.at("build"), "compiler_identity_sha256", "build");
    spec.runner_sha256 = stringField(
        contract.at("build"), "runner_sha256", "build");
    spec.validator_sha256 = stringField(
        contract.at("build"), "validator_sha256", "build");
    return spec;
}

V3EnvironmentObservation observeV3Environment() {
    V3EnvironmentObservation result;
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    if (::sched_getaffinity(0, sizeof(affinity), &affinity) != 0) {
        throw ClassifiedV3Error(
            kV3EnvironmentError, "sched_getaffinity failed");
    }
    for (std::size_t cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
        if (CPU_ISSET(static_cast<int>(cpu), &affinity)) {
            result.affinity_cpu_set.push_back(cpu);
        }
    }
    result.smt_active = parseDecimal(
        readTokenFile("/sys/devices/system/cpu/smt/active"), "smt.active");
    std::ifstream cgroup("/proc/self/cgroup");
    std::string line;
    std::string path;
    while (std::getline(cgroup, line)) {
        if (line.rfind("0::", 0) == 0) {
            if (!path.empty()) {
                throw ClassifiedV3Error(
                    kV3EnvironmentError, "duplicate unified cgroup entry");
            }
            path = line.substr(3);
        }
    }
    if (!cgroup.eof() || !canonicalNonrootCgroup(path)) {
        throw ClassifiedV3Error(
            kV3EnvironmentError,
            "unified cgroup path is missing/noncanonical/root");
    }
    result.cgroup_path = path;
    const std::string directory = "/sys/fs/cgroup" + path;
    struct stat status {};
    if (::stat(directory.c_str(), &status) != 0 ||
        !S_ISDIR(status.st_mode)) {
        throw ClassifiedV3Error(
            kV3EnvironmentError, "cannot stat unified cgroup directory");
    }
    result.cgroup_id = std::to_string(major(status.st_dev)) + ":" +
        std::to_string(minor(status.st_dev)) + ":" +
        std::to_string(static_cast<std::uint64_t>(status.st_ino));
    result.memory_max_bytes = parseDecimal(
        readTokenFile(directory + "/memory.max"), "memory.max");
    result.memory_swap_max_bytes = parseDecimal(
        readTokenFile(directory + "/memory.swap.max"), "memory.swap.max");
    return result;
}

void requireV3Environment(
    const V3EnvironmentObservation& observed,
    const V3ResourceSpec& required) {
    if (observed.affinity_cpu_set != required.affinity_cpu_set ||
        observed.smt_active != required.smt_active_required ||
        observed.memory_max_bytes != required.memory_max_bytes ||
        observed.memory_swap_max_bytes != required.memory_swap_max_bytes ||
        !canonicalNonrootCgroup(observed.cgroup_path) ||
        observed.cgroup_id.empty() ||
        required.threads_requested == 0 ||
        required.threads_requested != required.threads_used_required) {
        throw ClassifiedV3Error(
            kV3EnvironmentError,
            "observed affinity/SMT/cgroup resource envelope differs from contract");
    }
}

int executeValidatedV3Run(
    const ValidatedV3RunContract& authority,
    const std::string& cli_receipt_path) noexcept {
    try {
        const Clock::time_point matcher_start = Clock::now();
        const V3ExecutionSpec spec = projectV3ExecutionSpec(authority);
        // The currently accepted authority generation carries only concrete
        // numeric caps.  It cannot prove that those caps were independently
        // derived, with checked arithmetic, from the post-filter n_f/m_f/q
        // observed by this fresh process.  Never let that synthetic
        // integration contract reach input I/O or emit a timing-eligible
        // production receipt.  The DSM_TCC_V3_TESTING entry point remains
        // available solely for closed synthetic plumbing tests.
        if (spec.numeric_cap_binding_mode == V3NumericCapBindingMode::
                ContractMaterializedWithoutFormulaDerivation) {
            throw ClassifiedV3Error(
                kV3AuthorizationOrHashError,
                "formula-derived post-filter numeric-cap authority is not "
                "integrated; static contract caps are synthetic-only");
        }
        if (cli_receipt_path != spec.matcher_receipt_path) {
            throw ClassifiedV3Error(
                kV3AuthorizationOrHashError,
                "CLI receipt path differs from typed run contract");
        }
        const V3EnvironmentObservation environment = observeV3Environment();
        return executeCore(
            authority, spec, environment, true, matcher_start);
    } catch (const ClassifiedV3Error& error) {
        std::cerr << "V3 execution error (exit " << error.exitCode()
                  << "): " << error.what() << '\n';
        return error.exitCode();
    } catch (const std::bad_alloc&) {
        std::cerr << "V3 execution allocation failure.\n";
        return kV3PlannerNotIntegrated;
    } catch (const std::exception& error) {
        std::cerr << "V3 execution unexpected failure: " << error.what() << '\n';
        return kV3PlannerNotIntegrated;
    } catch (...) {
        std::cerr << "V3 execution unknown failure.\n";
        return kV3PlannerNotIntegrated;
    }
}

#ifdef DSM_TCC_V3_TESTING
int executeProjectedV3RunForTesting(
    const ValidatedV3RunContract& authority,
    const V3ExecutionSpec& spec,
    const V3EnvironmentObservation& environment) noexcept {
    try {
        const Clock::time_point matcher_start = Clock::now();
        return executeCore(
            authority, spec, environment, false, matcher_start);
    } catch (const ClassifiedV3Error& error) {
        std::cerr << "V3 synthetic execution error (exit "
                  << error.exitCode() << "): " << error.what() << '\n';
        return error.exitCode();
    } catch (const std::exception& error) {
        std::cerr << "V3 synthetic unexpected failure: " << error.what() << '\n';
        return kV3PlannerNotIntegrated;
    } catch (...) {
        return kV3PlannerNotIntegrated;
    }
}
#endif

#endif // _WIN32
} // namespace dsm::v3
