#ifndef VALIDATED_GRAPH_SNAPSHOT_H
#define VALIDATED_GRAPH_SNAPSHOT_H

#include <cstdint>
#include <memory>
#include <string>

#include "Utils.h"

// Every field is a hard, fail-closed cap.  Callers that deliberately want no
// finite policy cap must say so explicitly with unbounded().
struct GraphValidationBudget {
    std::uint64_t data_vertices = 0;
    std::uint64_t query_vertices = 0;
    // Each cap is combined across the data graph and query graph.
    std::uint64_t combined_outgoing_adjacency_entries = 0;
    std::uint64_t combined_incoming_adjacency_entries = 0;
    std::uint64_t temporal_edges = 0;
    std::uint64_t interval_records = 0;
    std::uint64_t derived_incident_interval_visits = 0;
    std::uint64_t derived_neighbor_label_updates = 0;
    std::uint64_t snapshot_storage_bytes = 0;
    std::uint64_t temporary_validation_bytes = 0;
    std::uint64_t seal_peak_live_bytes = 0;

    static GraphValidationBudget unbounded() noexcept;
};

// Pre-allocation, checked quote produced from the freshly parsed and
// internally filtered graph objects.  It uses the same native storage model
// as seal(), and its exact budget projection is the only production input to
// the seal gate.  This object carries no timing and performs no allocation
// proportional to the graph.
enum class GraphValidationQuoteStatus {
    Complete,
    InvalidDataGraph,
    InvalidQueryGraph,
    ArithmeticOverflow
};

const char* graphValidationQuoteStatusName(
    GraphValidationQuoteStatus status) noexcept;

struct GraphValidationQuote {
    bool complete = false;
    bool overflowed = false;
    GraphValidationQuoteStatus status =
        GraphValidationQuoteStatus::InvalidDataGraph;
    GraphValidationBudget exact_budget;
    std::uint64_t source_live_storage_bytes = 0;
    std::uint64_t snapshot_owner_control_bytes = 0;
    std::uint64_t maximum_incident_intervals = 0;

    std::uint64_t storage_quote_graphs_visited = 0;
    std::uint64_t storage_quote_vertices_visited = 0;
    std::uint64_t storage_quote_outgoing_adjacency_entries_visited = 0;
    std::uint64_t storage_quote_incoming_adjacency_entries_visited = 0;
    std::uint64_t storage_quote_incident_outgoing_entries_visited = 0;
    std::uint64_t storage_quote_incident_incoming_entries_visited = 0;
    std::uint64_t storage_quote_temporal_edges_visited = 0;
    std::uint64_t storage_quote_interval_records_visited = 0;

    // These counters are not part of the canonical public evidence.  They
    // preserve the independently visible source-capacity accounting scan so
    // seal() can consume one already-rederived quote without silently
    // performing a second internal scan.
    std::uint64_t storage_quote_passes = 0;
    std::uint64_t storage_accounting_passes = 0;
    std::uint64_t storage_accounting_graphs_visited = 0;
    std::uint64_t storage_accounting_vertices_visited = 0;
    std::uint64_t storage_accounting_temporal_edges_visited = 0;
    std::uint64_t storage_accounting_interval_records_visited = 0;
};

// Pure checked quote. It performs the structural checks required to define an
// unambiguous cap vector before any snapshot allocation. Deeper topology and
// fingerprint validation remains seal()'s responsibility. Every incomplete
// result carries an explicit typed status; only ArithmeticOverflow sets
// overflowed=true.
GraphValidationQuote quoteGraphValidation(
    const Graph& data,
    const Graph& query) noexcept;

// Exact post-filter counts that bind the graph quote to the formula-cap
// materializer.  They deliberately duplicate the structural projection in a
// typed form so the C++ and Python authorities can compare canonical bytes.
struct GraphValidationDerivationInputsV1 {
    std::uint64_t filtered_edges = 0;
    std::uint64_t filtered_vertices = 0;
    std::uint64_t interval_records = 0;
    std::uint64_t query_edges = 0;
    std::uint64_t query_vertices = 0;
};

struct GraphValidationTraversalCountersV1 {
    std::uint64_t storage_quote_graphs_visited = 0;
    std::uint64_t storage_quote_vertices_visited = 0;
    std::uint64_t storage_quote_outgoing_adjacency_entries_visited = 0;
    std::uint64_t storage_quote_incoming_adjacency_entries_visited = 0;
    std::uint64_t storage_quote_incident_outgoing_entries_visited = 0;
    std::uint64_t storage_quote_incident_incoming_entries_visited = 0;
    std::uint64_t storage_quote_temporal_edges_visited = 0;
    std::uint64_t storage_quote_interval_records_visited = 0;
};

// Canonical build-bound accounting descriptor.  The descriptor contains all
// native widths used by the quote, the ownership/peak equations, and the
// generated source/compiler identities.  Its SHA is computed at runtime from
// those inputs; it is never embedded back into the source bundle, avoiding a
// circular hash dependency.
struct GraphValidationAccountingModelArtifactV1 {
    std::string canonical_bytes;
    std::string sha256;

    bool complete() const noexcept;
};

GraphValidationAccountingModelArtifactV1
graphValidationAccountingModelArtifactV1();

// Closed projection matching
// benchmark.icde2027_v3.formula_receipt_model.GRAPH_QUOTE_SCHEMA.  quote_sha256
// covers canonical JSON of every field below except quote_sha256 itself; the
// later seal_reverification object is receipt evidence and is intentionally
// outside this immutable pre-seal quote core.
struct GraphValidationQuoteEvidenceV1 {
    std::string schema =
        "icde2027_v3_graph_validation_quote_evidence";
    std::uint64_t schema_version = 1;
    bool complete = false;
    bool overflowed = false;
    GraphValidationQuoteStatus status =
        GraphValidationQuoteStatus::InvalidDataGraph;
    std::string graph_accounting_model_sha256;
    GraphValidationDerivationInputsV1 derivation_inputs;
    GraphValidationBudget exact_budget;
    std::uint64_t source_live_storage_bytes = 0;
    std::uint64_t snapshot_owner_control_bytes = 0;
    std::uint64_t maximum_incident_intervals = 0;
    GraphValidationTraversalCountersV1 traversal_counters;
    std::string quote_sha256;
};

// The factory performs exactly one native quote scan and no allocation
// proportional to graph size.  Canonicalization allocates only constant-size
// evidence storage and may throw std::bad_alloc; the production seal overload
// below converts such a failure into a typed fail-closed status.
GraphValidationQuoteEvidenceV1 quoteGraphValidationEvidenceV1(
    const Graph& data,
    const Graph& query);

std::string canonicalGraphValidationQuoteEvidenceCoreV1(
    const GraphValidationQuoteEvidenceV1& evidence);
std::string graphValidationQuoteEvidenceSha256V1(
    const GraphValidationQuoteEvidenceV1& evidence);

#ifdef DSM_TCC_V3_TESTING
// Exercises the exact checked multiply/add primitive used by all native
// storage-capacity projections without requiring an impossibly large test
// allocation.
bool graphValidationCheckedStorageAddForTesting(
    std::uint64_t initial_bytes,
    std::uint64_t count,
    std::uint64_t width,
    std::uint64_t& result_bytes) noexcept;
#endif

enum class GraphValidationStatus {
    Complete,
    InvalidDataGraph,
    InvalidQueryGraph,
    BudgetExceeded,
    ArithmeticOverflow,
    AllocationFailure,
    AllocationModelViolation,
    SnapshotTokenExhausted,
    InvalidExpectedQuote,
    GraphAccountingModelMismatch,
    ExpectedQuoteMismatch,
    QuoteEvidenceFailure
};

const char* graphValidationStatusName(GraphValidationStatus status) noexcept;

// Immutable telemetry for the one validation/fingerprint invocation.  The
// adjacency counter is split by graph and direction so a full rescan cannot be
// hidden inside an aggregate value.
struct GraphValidationReceipt {
    GraphValidationStatus status = GraphValidationStatus::InvalidDataGraph;
    std::uint64_t snapshot_token = 0;
    std::uint64_t graph_validation_calls = 0;
    std::uint64_t storage_quote_passes = 0;
    std::uint64_t storage_quote_graphs_visited = 0;
    std::uint64_t storage_quote_vertices_visited = 0;
    std::uint64_t storage_quote_outgoing_adjacency_entries_visited = 0;
    std::uint64_t storage_quote_incoming_adjacency_entries_visited = 0;
    std::uint64_t storage_quote_incident_outgoing_entries_visited = 0;
    std::uint64_t storage_quote_incident_incoming_entries_visited = 0;
    std::uint64_t storage_quote_temporal_edges_visited = 0;
    std::uint64_t storage_quote_interval_records_visited = 0;
    std::uint64_t storage_accounting_passes = 0;
    std::uint64_t storage_accounting_graphs_visited = 0;
    std::uint64_t storage_accounting_vertices_visited = 0;
    std::uint64_t storage_accounting_temporal_edges_visited = 0;
    std::uint64_t storage_accounting_interval_records_visited = 0;
    std::uint64_t data_vertices_visited = 0;
    std::uint64_t query_vertices_visited = 0;
    std::uint64_t data_outgoing_adjacency_entries_visited = 0;
    std::uint64_t data_incoming_adjacency_entries_visited = 0;
    std::uint64_t query_outgoing_adjacency_entries_visited = 0;
    std::uint64_t query_incoming_adjacency_entries_visited = 0;
    std::uint64_t temporal_edges_visited = 0;
    std::uint64_t interval_records_visited = 0;
    std::uint64_t derived_metadata_rebuild_passes = 0;
    std::uint64_t derived_incident_interval_visits = 0;
    std::uint64_t derived_neighbor_label_updates = 0;
    std::uint64_t data_fingerprint_passes = 0;
    std::uint64_t query_fingerprint_passes = 0;
    std::uint64_t validation_nanoseconds = 0;
    std::uint64_t snapshot_storage_bytes = 0;
    std::uint64_t temporary_validation_capacity_bytes = 0;
    std::uint64_t source_live_storage_bytes = 0;
    std::uint64_t quoted_snapshot_storage_bytes = 0;
    std::uint64_t snapshot_owner_control_bytes = 0;
    std::uint64_t quoted_validation_temporary_bytes = 0;
    std::uint64_t quoted_maximum_incident_intervals = 0;
    std::uint64_t quoted_seal_peak_live_bytes = 0;
    std::uint64_t seal_peak_live_bytes = 0;
    std::uint64_t data_fingerprint = 0;
    std::uint64_t query_fingerprint = 0;

    // Populated only by the production expected-evidence overload.  The
    // observed hash comes from an independent internal requote before any
    // graph-proportional clone/temporary allocation.
    bool production_expected_quote_binding = false;
    std::uint64_t expected_quote_reverification_passes = 0;
    std::string graph_accounting_model_sha256;
    std::string expected_quote_sha256;
    std::string observed_quote_sha256;
    bool expected_quote_exact_equality = false;

    bool complete() const noexcept;
};

class ValidatedGraphSnapshot;

struct GraphSnapshotSealResult {
    GraphValidationReceipt receipt;
    std::shared_ptr<const ValidatedGraphSnapshot> snapshot;

    bool complete() const noexcept;
};

// Source graphs stay valid. Fresh private allocations break every mutable
// element alias acquired from the public Graph representation before seal.
class ValidatedGraphSnapshot {
public:
    static GraphSnapshotSealResult seal(
        const Graph& data,
        const Graph& query,
        const GraphValidationBudget& budget) noexcept;

    // Formula-cap production entry point.  The expected quote is treated as
    // data, never as authority: seal requotes the current graphs, recomputes
    // both canonical hashes, and requires exact typed equality before any
    // allocation proportional to the graph.  Legacy synthetic callers retain
    // the budget-only overload above.
    static GraphSnapshotSealResult seal(
        const Graph& data,
        const Graph& query,
        const GraphValidationQuoteEvidenceV1& expected_quote) noexcept;

    ValidatedGraphSnapshot(const ValidatedGraphSnapshot&) = delete;
    ValidatedGraphSnapshot& operator=(
        const ValidatedGraphSnapshot&) = delete;

    const Graph& data() const noexcept;
    const Graph& query() const noexcept;
    std::uint64_t snapshotToken() const noexcept;
    std::uint64_t dataFingerprint() const noexcept;
    std::uint64_t queryFingerprint() const noexcept;
    const GraphValidationReceipt& receipt() const noexcept;

private:
    ValidatedGraphSnapshot() = default;

    static GraphSnapshotSealResult sealRequoted(
        const Graph& data,
        const Graph& query,
        const GraphValidationBudget& budget,
        const GraphValidationQuote& observed_quote,
        GraphValidationReceipt receipt) noexcept;

    Graph data_;
    Graph query_;
    GraphValidationReceipt receipt_;
};

// shared_ptr owner equality, not object address, is the authority check.
bool sharesValidatedGraphSnapshot(
    const std::shared_ptr<const ValidatedGraphSnapshot>& lhs,
    const std::shared_ptr<const ValidatedGraphSnapshot>& rhs) noexcept;

#endif // VALIDATED_GRAPH_SNAPSHOT_H
