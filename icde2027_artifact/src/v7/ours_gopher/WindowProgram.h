#ifndef DSM_V5_WINDOW_PROGRAM_H
#define DSM_V5_WINDOW_PROGRAM_H
#include "CandidateDag.h"

// Typed W lowering of a Candidate-DAG's temporal subgraph. Structural node
// identities remain those of the input IR. The old temporal/run roots are NOT
// executed: each is replaced by one BatchWindowAnd with two explicit inputs,
// the structural producer's edge-handle rows and the branch's W(J).
// DirectP4Evaluator never constructs or consumes this program.
class WindowProgram {
public:
    struct BatchWindowAnd {
        int query_vertex = -1;
        DagNodeId producer = kInvalidDagNode;
        std::vector<DagDirectedEdgeSlot> history_slots;
        std::vector<std::size_t> operand_order;
        bool consumes_branch_support = true;
        bool absorbs_lmax = true;
        // P8 compile eligibility is determined solely by the existing P4
        // producer pattern: the first canonical adjacency drives the index.
        // The explicit W(J) input supplies state-dependent bucket selection.
        bool jindex_choice = false;
        DagNodeId jindex_driver = kInvalidDagNode;
        int jindex_anchor_query_vertex = -1;
        bool jindex_outgoing = false;
    };
    WindowProgram(const CandidateDag& dag, const QueryPlan& plan, bool jindex = false);
    const BatchWindowAnd& node(int q) const;
    void dump(const CandidateDag& dag, const std::string& prefix) const;
private:
    std::vector<BatchWindowAnd> nodes_;
};
#endif
