#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lbug {
namespace graph {
class Graph;
}
} // namespace lbug

namespace lbug {
namespace algo_extension {

// Weighted directed graph built from a single-node-table projection. Feeds the
// weight-aware algorithms of the NASH/GNN P1 batch (DIJKSTRA, MAX_FLOW,
// MIN_CUT_VALUE, CUT_CLUSTERING, SPANNING_EDGE_CENTRALITY).
//
// Weight rules (documented in the API reference):
//   - the rel property named `weightProperty` is read per edge; a missing
//     property or a NULL value falls back to weight 1.0;
//   - numeric property types (DOUBLE/FLOAT/INT*) are all accepted.
struct WeightedGraph {
    uint64_t numNodes = 0;

    // Directed view: out-arcs exactly as stored (parallel arcs kept separate,
    // self-loops kept). DIJKSTRA relaxes over these (parallel arcs -> min via
    // relaxation), MAX_FLOW treats them as separate capacity units.
    std::vector<std::vector<std::pair<uint64_t, double>>> out;

    // Undirected view: union of arcs and reversed arcs, self-loops dropped,
    // parallel/bidirectional storage collapsed to ONE edge per unordered pair
    // whose weight is the maximum arc weight between the two endpoints.
    std::vector<std::vector<std::pair<uint64_t, double>>> und;

    uint64_t numUndEdges() const;
};

// Scans every node's forward rels once, reading `weightProperty` when present.
WeightedGraph buildWeightedGraph(graph::Graph* graph, uint64_t tableID, uint64_t numNodes,
    const std::string& weightProperty);

// Negative-weight / negative-capacity guard shared by DIJKSTRA and MAX_FLOW.
void validateNonNegativeWeights(const WeightedGraph& wg, const std::string& algoName);

} // namespace algo_extension
} // namespace lbug
