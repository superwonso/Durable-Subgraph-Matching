#ifndef ORIGINAL_E3_E4_ADAPTER_V1_H
#define ORIGINAL_E3_E4_ADAPTER_V1_H

#include <cstdint>
#include <vector>

#include "TDTree.h"
#include "../benchmark/e3_e4_adapter_v1/E3E4AdapterCommonV1.h"

namespace dsm::e3e4::adapter_v1::td_tree {

// This pair is an integration source slice, not a publication authority.  It
// intentionally exposes neither a main() nor a filesystem receipt writer.
inline constexpr bool kSourceReceiptAuthority = false;
inline constexpr bool kBuildOrExecutionAuthority = false;

// Reconstruct the Original temporal graph exclusively from bytes already
// parsed by the common held-input loader, injecting its validated explicit
// runtime labels by exact external vertex ID.  No seed, ambient label mode,
// raw path, filter, or legacy graph reader is accepted by this API.
[[nodiscard]] Graph buildExplicitRuntimeLabeledTemporalGraph(
    const std::vector<RawTemporalEdge>& raw_temporal_edges,
    const std::vector<RuntimeLabelRow>& runtime_labels_by_external_id);

// Complete in-memory child receipt construction for the source-only adapter.
// Worker affinity comes from the observed native core; durable O_EXCL receipt
// creation and any publication remain obligations of the separately audited
// launcher/writer slice.
struct TdTreeSourceOnlyResult {
    ClosedInvocation invocation;
    InputIdentitySet input_identities;
    ExcludedLoadDurations loads;
    PhaseLedgerSnapshot phase_ledger;
    MatchSummary match_summary;
    ThreadObservation threads;
    CanonicalChildReceipt canonical_child_receipt;
};

// Production-shaped source entrypoint.  It captures child_start before
// opening the exact canonical repository anchor, then constructs the complete
// canonical child receipt in memory.  It never opens or writes the receipt
// destination; the separately audited exclusive writer owns that claim.
[[nodiscard]] TdTreeSourceOnlyResult runTdTreeClosedCountOnly(
    int argc,
    const char* const argv[]);

}  // namespace dsm::e3e4::adapter_v1::td_tree

#endif  // ORIGINAL_E3_E4_ADAPTER_V1_H
