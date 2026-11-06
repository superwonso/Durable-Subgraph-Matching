#include "WindowProgram.h"
#include <filesystem>
#include <fstream>
#include <numeric>
#include <stdexcept>

WindowProgram::WindowProgram(const CandidateDag& dag, const QueryPlan& plan, bool jindex)
    : nodes_(dag.structural_roots.size()) {
    if (!dag.passes.p4 || dag.passes.temporalRewriteAny())
        throw std::invalid_argument("W requires P4 and P1-P3 off");
    for (int q : plan.order) {
        auto& out = nodes_.at(static_cast<std::size_t>(q));
        out.query_vertex = q;
        if (q == plan.root) continue; // optional root self-loop is initialized separately
        out.producer = dag.structural_roots.at(static_cast<std::size_t>(q));
        const auto& producer = dag.node(out.producer);
        if (producer.op != DagOp::FusedEdgeProbe || producer.history_slots.empty())
            throw std::logic_error("W requires a compiled P4 edge-handle producer");
        out.history_slots = producer.history_slots;
        if (jindex && producer.parents.size() > 1) {
            const auto& driver = dag.node(producer.parents[1]);
            const bool outgoing = driver.op == DagOp::OutNeighborLookup ||
                driver.op == DagOp::OutNeighborView;
            const bool incoming = driver.op == DagOp::InNeighborLookup ||
                driver.op == DagOp::InNeighborView;
            if (!outgoing && !incoming)
                throw std::logic_error("P8 requires a canonical adjacency driver");
            out.jindex_choice = true;
            out.jindex_driver = producer.parents[1];
            out.jindex_anchor_query_vertex = driver.parameter.first_query_vertex;
            out.jindex_outgoing = outgoing;
        }
        out.operand_order.resize(out.history_slots.size());
        std::iota(out.operand_order.begin(), out.operand_order.end(), 0);
        // Stable query-ID slot order, fixed before measurements. J is the
        // first input. Every following AND can terminate on empty support.
    }
}
const WindowProgram::BatchWindowAnd& WindowProgram::node(int q) const {
    return nodes_.at(static_cast<std::size_t>(q));
}
void WindowProgram::dump(const CandidateDag& dag, const std::string& prefix) const {
    const auto dot_path = prefix + ".window.dot";
    const auto csv_path = prefix + ".window_nodes.csv";
    if (std::filesystem::exists(dot_path) || std::filesystem::exists(csv_path))
        throw std::runtime_error("refusing to replace W DAG dump");
    std::ofstream dot(dot_path), csv(csv_path);
    if (!dot || !csv) throw std::runtime_error("cannot open W DAG dump");
    dot << "digraph WindowCandidateDAG {\n";
    csv << "query_vertex,producer,operation,history_inputs,branch_inputs,lmax_absorbed,jindex_driver,anchor_query_vertex,direction\n";
    std::vector<bool> seen(dag.nodes.size());
    auto structural = [&](auto&& self, DagNodeId id) -> void {
        if (seen.at(id)) return;
        seen[id] = true;
        const auto& n = dag.node(id);
        dot << "n" << id << " [label=\"" << dagOpName(n.op) << " #" << id << "\"];\n";
        for (auto p : n.parents) {
            self(self, p);
            dot << "n" << p << " -> n" << id << ";\n";
        }
    };
    for (const auto& n : nodes_) {
        if (n.producer == kInvalidDagNode) continue;
        structural(structural, n.producer);
        dot << "j" << n.query_vertex << " [label=\"BranchWindowSource W(J)\"];\n"
            << "w" << n.query_vertex << " [label=\"BatchWindowAnd q" << n.query_vertex << "\"];\n"
            << "n" << n.producer << " -> w" << n.query_vertex << " [label=\"handle rows\"];\n"
            << "j" << n.query_vertex << " -> w" << n.query_vertex << ";\n";
        if (n.jindex_choice) {
            // Overlay identifies the replacement physical producer, while
            // retaining the original P4 pattern as the scan alternative.
            dot << "x" << n.query_vertex << " [label=\"JIndexChoice q" << n.query_vertex
                << "\\nindex / original S1 scan\"];\n"
                << "j" << n.query_vertex << " -> x" << n.query_vertex << " [label=\"blocks(W(J))\"];\n"
                << "n" << n.jindex_driver << " -> x" << n.query_vertex << " [label=\"CSR driver\"];\n"
                << "x" << n.query_vertex << " -> n" << n.producer << " [label=\"necessary-condition seed + bound driver handle\"];\n";
            csv << n.query_vertex << ',' << n.producer << ",JIndexChoice,1,1,true,"
                << n.jindex_driver << ',' << n.jindex_anchor_query_vertex << ','
                << (n.jindex_outgoing ? "out" : "in") << '\n';
        }
        csv << n.query_vertex << ',' << n.producer << ",BatchWindowAnd,"
            << n.history_slots.size() << ",1,true,,,\n";
    }
    dot << "}\n";
}
