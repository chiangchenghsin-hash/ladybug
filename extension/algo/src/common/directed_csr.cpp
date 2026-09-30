#include "common/directed_csr.h"

#include "common/types/types.h"
#include "graph/on_disk_graph.h"

#include <algorithm>
#include <queue>

using namespace lbug::common;

namespace lbug {
namespace algo_extension {

bool DirectedCSR::edgeExists(uint64_t from, uint64_t to) const {
    const auto begin = fwdNeighbors.begin() + static_cast<ptrdiff_t>(fwdOffsets[from]);
    const auto end = fwdNeighbors.begin() + static_cast<ptrdiff_t>(fwdOffsets[from + 1]);
    return std::binary_search(begin, end, to);
}

DirectedCSR buildDirectedCSR(uint64_t numNodes,
    const std::vector<std::pair<uint64_t, uint64_t>>& edges) {
    DirectedCSR csr;
    csr.numNodes = numNodes;
    csr.numEdges = edges.size();

    // 1. Count degrees.
    std::vector<uint64_t> fwdCount(numNodes, 0), bwdCount(numNodes, 0);
    for (const auto& [src, dst] : edges) {
        fwdCount[src]++;
        bwdCount[dst]++;
    }
    // 2. Build offsets.
    csr.fwdOffsets.assign(numNodes + 1, 0);
    csr.bwdOffsets.assign(numNodes + 1, 0);
    for (uint64_t i = 0; i < numNodes; ++i) {
        csr.fwdOffsets[i + 1] = csr.fwdOffsets[i] + fwdCount[i];
        csr.bwdOffsets[i + 1] = csr.bwdOffsets[i] + bwdCount[i];
    }
    // 3. Fill neighbors (unsorted), then sort each node's range.
    csr.fwdNeighbors.resize(edges.size());
    csr.bwdNeighbors.resize(edges.size());
    std::vector<uint64_t> fwdCursor = csr.fwdOffsets; // copy: current insertion position
    std::vector<uint64_t> bwdCursor = csr.bwdOffsets;
    for (const auto& [src, dst] : edges) {
        csr.fwdNeighbors[fwdCursor[src]++] = dst;
        csr.bwdNeighbors[bwdCursor[dst]++] = src;
    }
    for (uint64_t i = 0; i < numNodes; ++i) {
        std::sort(csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[i]),
            csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[i + 1]));
        std::sort(csr.bwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.bwdOffsets[i]),
            csr.bwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.bwdOffsets[i + 1]));
    }
    return csr;
}

DirectedCSR buildGraphCSR(graph::Graph* graph, uint64_t tableID, uint64_t numNodes) {
    std::vector<std::pair<uint64_t, uint64_t>> edges;
    const auto nbrTables = graph->getRelInfos(tableID);
    const auto nbrInfo = nbrTables[0];
    const auto scanState = graph->prepareRelScan(*nbrInfo.relGroupEntry, nbrInfo.relTableID,
        nbrInfo.dstTableID, {}, false /*randomLookup*/);
    for (uint64_t nodeId = 0; nodeId < numNodes; ++nodeId) {
        const nodeID_t nid = {static_cast<offset_t>(nodeId), static_cast<table_id_t>(tableID)};
        for (auto chunk : graph->scanFwd(nid, *scanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                edges.emplace_back(nodeId, neighbors[i].offset);
            });
        }
    }
    return buildDirectedCSR(numNodes, edges);
}

std::vector<std::vector<uint64_t>> buildUndirectedAdjacency(const DirectedCSR& csr) {
    std::vector<std::vector<uint64_t>> adj(csr.numNodes);
    for (uint64_t u = 0; u < csr.numNodes; ++u) {
        auto& neighbors = adj[u];
        neighbors.insert(neighbors.end(), csr.fwdNeighbors.begin() +
                static_cast<ptrdiff_t>(csr.fwdOffsets[u]),
            csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]));
        neighbors.insert(neighbors.end(), csr.bwdNeighbors.begin() +
                static_cast<ptrdiff_t>(csr.bwdOffsets[u]),
            csr.bwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.bwdOffsets[u + 1]));
        std::sort(neighbors.begin(), neighbors.end());
        neighbors.erase(std::unique(neighbors.begin(), neighbors.end()), neighbors.end());
    }
    return adj;
}

void detectDAG(DirectedCSR& csr) {
    const auto n = csr.numNodes;
    std::vector<uint64_t> inDeg(n, 0);
    for (uint64_t v = 0; v < n; ++v) {
        inDeg[v] = csr.bwdOffsets[v + 1] - csr.bwdOffsets[v];
    }
    std::queue<uint64_t> ready;
    for (uint64_t v = 0; v < n; ++v) {
        if (inDeg[v] == 0) {
            ready.push(v);
        }
    }
    csr.depth.assign(n, 0);
    csr.topoOrder.reserve(n);
    uint64_t visited = 0;
    while (!ready.empty()) {
        const auto u = ready.front();
        ready.pop();
        visited++;
        csr.topoOrder.push_back(u);
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            const auto w = *it;
            if (csr.depth[w] < csr.depth[u] + 1) {
                csr.depth[w] = csr.depth[u] + 1;
            }
            if (--inDeg[w] == 0) {
                ready.push(w);
            }
        }
    }
    csr.isDAG = (visited == n);
    if (!csr.isDAG) {
        csr.depth.clear();
        csr.topoOrder.clear();
    }
}

} // namespace algo_extension
} // namespace lbug
