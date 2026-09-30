#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace lbug {
namespace graph {
class Graph;
}
} // namespace lbug

namespace lbug {
namespace algo_extension {

// Flat directed CSR. fwd = out-edges (src -> dst), bwd = in-edges (dst <- src).
// Per-node neighbor lists are sorted ascending to support binary-search edge lookup.
struct DirectedCSR {
    std::vector<uint64_t> fwdOffsets; // size numNodes + 1
    std::vector<uint64_t> fwdNeighbors;
    std::vector<uint64_t> bwdOffsets; // size numNodes + 1
    std::vector<uint64_t> bwdNeighbors;
    uint64_t numNodes = 0;
    uint64_t numEdges = 0;

    bool isDAG = false;         // result of Kahn's algorithm (only meaningful for the pattern)
    std::vector<int64_t> depth; // longest-path depth, valid iff isDAG (else empty)
    std::vector<uint64_t> topoOrder; // a valid topological order, valid iff isDAG (else empty)

    bool edgeExists(uint64_t from, uint64_t to) const; // binary search in fwd[from]
};

// Builds a DirectedCSR from an edge list (node ids must be in [0, numNodes)).
// Used by unit tests and the extension glue alike.
DirectedCSR buildDirectedCSR(uint64_t numNodes,
    const std::vector<std::pair<uint64_t, uint64_t>>& edges);

// Builds a CSR from a single-node-table graph projection (scans every node's forward
// neighbors once). Shared by the CSR-backed ALGO functions (GRAPH_SIGNATURE,
// BETWEENNESS, LOCAL_CLUSTERING_COEFFICIENT, KATZ_CENTRALITY, ASSORTATIVITY).
DirectedCSR buildGraphCSR(graph::Graph* graph, uint64_t tableID, uint64_t numNodes);

// Undirected view of a CSR: per node the union of fwd + bwd neighbors, deduped and
// sorted ascending. Shared by BETWEENNESS / LOCAL_CLUSTERING_COEFFICIENT /
// ASSORTATIVITY, which all use undirected semantics (双向边算一次).
std::vector<std::vector<uint64_t>> buildUndirectedAdjacency(const DirectedCSR& csr);

// Kahn's algorithm: sets `isDAG` and fills `depth` (longest path from any source) when acyclic.
void detectDAG(DirectedCSR& csr);

} // namespace algo_extension
} // namespace lbug
