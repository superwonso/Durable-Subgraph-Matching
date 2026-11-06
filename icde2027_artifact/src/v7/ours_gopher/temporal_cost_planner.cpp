// Select Gopher's TDTree API before compiling the shared planner body.  The
// include guards then prevent the canonical source from rebinding TDTree to
// the non-Gopher implementation.
#include "TDTree.h"
#include "temporal_cost_planner.h"

#include "../ours/temporal_cost_planner.cpp"
