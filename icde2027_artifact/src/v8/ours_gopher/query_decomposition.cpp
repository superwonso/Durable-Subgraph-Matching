#include "query_decomposition.h"

// query_decomposition.h above selects Gopher's Utils.h first and then defines
// QUERY_DECOMPOSITION_H, so the source included below binds to one consistent
// Gopher-compatible Graph/TemporalEdge ABI.
#include "../ours/query_decomposition.cpp"
