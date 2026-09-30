#pragma once

#include <cstdint>
#include <utility>
#include <vector>

namespace lbug {
namespace algo_extension {

// Dinic's maximum-flow algorithm over an explicit arc list. Each arc is
// (source, sink, capacity); parallel arcs are summed into the network,
// self-loops are dropped. `source` and `sink` must be distinct and in-range.
//
// For an UNDIRECTED edge of capacity c, pass the two antiparallel arcs
// (u, v, c) and (v, u, c): the resulting max-flow value equals the undirected
// min s-t cut capacity (each crossing edge contributes c exactly once).
//
// If `sourceSide` is non-null it is filled with the nodes reachable from
// `source` in the residual graph (the source side of one minimum cut).
double computeMaxFlow(uint64_t numNodes,
    const std::vector<std::pair<std::pair<uint64_t, uint64_t>, double>>& arcs, uint64_t source,
    uint64_t sink, std::vector<uint8_t>* sourceSide = nullptr);

} // namespace algo_extension
} // namespace lbug
