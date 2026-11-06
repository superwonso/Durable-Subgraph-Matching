// Bind the shared implementation to Gopher's Graph/TemporalEdge ABI before
// entering the canonical header.  Without this include, the quoted include in
// ../ours/query_decomposition.h resolves ../ours/Utils.h first, so this one
// translation unit sees a different TemporalEdge layout from the rest of the
// Gopher executable (an ODR violation that crashes the structural/survival
// selectivity scan).
#include "Utils.h"

// Keep every CPU/GPU Gopher engine on the same selectivity definitions and
// TCC metadata schema as Ours.  The implementation remains compiled inside
// ours_gopher; this forwarding header prevents the two copies from drifting.
#include "../ours/query_decomposition.h"
