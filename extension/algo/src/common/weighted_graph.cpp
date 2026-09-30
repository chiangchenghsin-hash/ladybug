#include "common/weighted_graph.h"

#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "graph/on_disk_graph.h"

#include <algorithm>

using namespace lbug::common;

namespace lbug {
namespace algo_extension {

uint64_t WeightedGraph::numUndEdges() const {
    uint64_t count = 0;
    for (const auto& nbrs : und) {
        count += nbrs.size();
    }
    return count / 2; // each undirected edge appears in both endpoint lists
}

// Reads one value out of a scanned rel property vector as a double.
// NULL values fall back to the caller's default (see buildWeightedGraph).
static double readWeightAsDouble(const ValueVector& vector, uint64_t pos) {
    switch (vector.dataType.getLogicalTypeID()) {
    case LogicalTypeID::DOUBLE:
        return vector.getValue<double>(pos);
    case LogicalTypeID::FLOAT:
        return static_cast<double>(vector.getValue<float>(pos));
    case LogicalTypeID::INT64:
        return static_cast<double>(vector.getValue<int64_t>(pos));
    case LogicalTypeID::INT32:
        return static_cast<double>(vector.getValue<int32_t>(pos));
    case LogicalTypeID::INT16:
        return static_cast<double>(vector.getValue<int16_t>(pos));
    case LogicalTypeID::UINT64:
        return static_cast<double>(vector.getValue<uint64_t>(pos));
    case LogicalTypeID::UINT32:
        return static_cast<double>(vector.getValue<uint32_t>(pos));
    default:
        throw BinderException{"Edge weight property must be numeric, got: " +
                              vector.dataType.toString()};
    }
}

WeightedGraph buildWeightedGraph(graph::Graph* graph, uint64_t tableID, uint64_t numNodes,
    const std::string& weightProperty) {
    const auto nbrTables = graph->getRelInfos(tableID);
    if (nbrTables.empty()) {
        throw BinderException{"Projected graph has no relationship tables."};
    }
    const auto nbrInfo = nbrTables[0];

    const bool hasWeight = nbrInfo.relGroupEntry->containsProperty(weightProperty);
    const auto scanState = graph->prepareRelScan(*nbrInfo.relGroupEntry, nbrInfo.relTableID,
        nbrInfo.dstTableID, hasWeight ? std::vector<std::string>{weightProperty}
                                      : std::vector<std::string>{},
        false /*randomLookup*/);

    WeightedGraph wg;
    wg.numNodes = numNodes;
    wg.out.assign(numNodes, {});

    for (uint64_t nodeId = 0; nodeId < numNodes; ++nodeId) {
        const nodeID_t nid = {static_cast<offset_t>(nodeId), static_cast<table_id_t>(tableID)};
        for (auto chunk : graph->scanFwd(nid, *scanState)) {
            chunk.forEach([&](auto neighbors, auto props, auto i) {
                double w = 1.0;
                if (hasWeight && props[0] && !props[0]->isNull(i)) {
                    w = readWeightAsDouble(*props[0], i);
                }
                wg.out[nodeId].emplace_back(neighbors[i].offset, w);
            });
        }
    }

    // Undirected view: merge out arcs with reversed arcs per unordered pair,
    // keeping ONE edge whose weight is the maximum arc weight (bidirectional
    // storage of the same relationship must not double the capacity).
    wg.und.assign(numNodes, {});
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto& [v, w] : wg.out[u]) {
            if (v == u) {
                continue; // self-loops are no-ops for undirected algorithms
            }
            wg.und[u].emplace_back(v, w);
            wg.und[v].emplace_back(u, w);
        }
    }
    for (uint64_t u = 0; u < numNodes; ++u) {
        auto& nbrs = wg.und[u];
        std::sort(nbrs.begin(), nbrs.end());
        std::vector<std::pair<uint64_t, double>> merged;
        merged.reserve(nbrs.size());
        for (const auto& [v, w] : nbrs) {
            if (!merged.empty() && merged.back().first == v) {
                merged.back().second = std::max(merged.back().second, w);
            } else {
                merged.emplace_back(v, w);
            }
        }
        nbrs = std::move(merged);
    }
    return wg;
}

void validateNonNegativeWeights(const WeightedGraph& wg, const std::string& algoName) {
    for (const auto& nbrs : wg.out) {
        for (const auto& [v, w] : nbrs) {
            if (w < 0) {
                throw BinderException{algoName +
                                      " does not support negative edge weights/capacities."};
            }
        }
    }
}

} // namespace algo_extension
} // namespace lbug
