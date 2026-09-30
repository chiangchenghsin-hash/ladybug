// PARTITION_INTERSECTION - combine two partitions (per-node community pair)
// into a single combined-community id. P2-05 (NASH×GNN P2 batch): 比较不同均衡
// 假设的社区结构。对标 NetworKit `community::PartitionIntersection`.
//
//   CALL partition_intersection('g', algo1 := 'label_propagation', algo2 := 'wcc')
//     YIELD node, combined_community
//
// The two partitions are computed internally on the SAME projection graph
// (deterministic: LPA ties break to the smallest label; WCC ids are the
// smallest offset). Each node's (c1, c2) pair is renumbered to a consecutive
// combined-community id 0..C-1 in order of first appearance.
//
// Supported algorithms: 'label_propagation' (LPA community) and 'wcc'
// (connected components). spec suggested 'leiden' as a second algorithm, but
// that requires the full Leiden implementation; WCC is used as the default
// coarse baseline instead (documented divergence).
#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/algo_param_utils.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char COMBINED_COMMUNITY_COLUMN_NAME[] = "combined_community";

static constexpr char DEFAULT_ALGO1[] = "label_propagation";
static constexpr char DEFAULT_ALGO2[] = "wcc";

struct PartitionIntersectionBindData final : public GDSBindData {
    bool algo1Lpa;
    bool algo2Lpa;

    PartitionIntersectionBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, bool algo1Lpa, bool algo2Lpa)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          algo1Lpa{algo1Lpa}, algo2Lpa{algo2Lpa} {}

    PartitionIntersectionBindData(const PartitionIntersectionBindData& other)
        : GDSBindData{other}, algo1Lpa{other.algo1Lpa}, algo2Lpa{other.algo2Lpa} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<PartitionIntersectionBindData>(*this);
    }
};

static bool parseAlgo(const std::string& algo, const char* which) {
    if (algo == "label_propagation") {
        return true;
    }
    if (algo == "wcc") {
        return false;
    }
    throw BinderException{std::string{"PARTITION_INTERSECTION "} + which +
                          " must be 'label_propagation' or 'wcc'."};
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    std::string algo1 = DEFAULT_ALGO1;
    std::string algo2 = DEFAULT_ALGO2;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "algo1") {
            algo1 = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else if (paramName == "algo2") {
            algo2 = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(
        input->binder->createVariable(COMBINED_COMMUNITY_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<PartitionIntersectionBindData>(std::move(columns),
        std::move(graphEntry), expression_vector{nodeOutput}, parseAlgo(algo1, "algo1"),
        parseAlgo(algo2, "algo2"));
}

// LPA community ids (same core as LABEL_PROPAGATION, update_seed, tie to
// smallest label).
void lpaPartition(const std::vector<std::vector<uint64_t>>& adj, std::vector<uint64_t>& labels) {
    const auto n = adj.size();
    labels.resize(n);
    for (uint64_t v = 0; v < n; ++v) {
        labels[v] = v;
    }
    if (n <= 1) {
        return;
    }
    std::vector<uint64_t> previous = labels;
    std::vector<uint64_t> twoAgo = labels;
    std::vector<uint64_t> freq(n, 0);
    for (int64_t iter = 0; iter < 100; ++iter) {
        twoAgo = previous;
        previous = labels;
        bool changed = false;
        for (uint64_t v = 0; v < n; ++v) {
            if (adj[v].empty()) {
                continue;
            }
            freq[labels[v]] = 0;
            for (const auto u : adj[v]) {
                freq[labels[u]] = 0;
            }
            for (const auto u : adj[v]) {
                freq[labels[u]]++;
            }
            uint64_t bestLabel = labels[v];
            uint64_t bestCount = 0;
            for (const auto u : adj[v]) {
                const auto l = labels[u];
                const auto c = freq[l];
                if (c > bestCount || (c == bestCount && l < bestLabel)) {
                    bestCount = c;
                    bestLabel = l;
                }
            }
            if (bestLabel == labels[v]) {
                continue;
            }
            labels[v] = bestLabel;
            changed = true;
        }
        if (!changed || labels == twoAgo) {
            break;
        }
    }
    std::vector<int64_t> renumber(n, -1);
    std::vector<uint64_t> out(n, 0);
    uint64_t nextId = 0;
    for (uint64_t v = 0; v < n; ++v) {
        if (renumber[labels[v]] < 0) {
            renumber[labels[v]] = static_cast<int64_t>(nextId++);
        }
        out[v] = static_cast<uint64_t>(renumber[labels[v]]);
    }
    labels = std::move(out);
}

// WCC partition: component id = smallest offset in the component.
void wccPartition(const std::vector<std::vector<uint64_t>>& adj, std::vector<uint64_t>& comp) {
    const auto n = adj.size();
    comp.assign(n, UINT64_MAX);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (uint64_t s = 0; s < n; ++s) {
        if (comp[s] != UINT64_MAX) {
            continue;
        }
        comp[s] = s;
        queue.clear();
        queue.push_back(s);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto w = queue[head];
            for (const auto t : adj[w]) {
                if (comp[t] == UINT64_MAX) {
                    comp[t] = s;
                    queue.push_back(t);
                }
            }
        }
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<PartitionIntersectionBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "PARTITION_INTERSECTION currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t w = 0; w < adj.size(); ++w) {
        auto& nbrs = adj[w];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), w), nbrs.end());
    }

    std::vector<uint64_t> p1;
    std::vector<uint64_t> p2;
    if (bindData->algo1Lpa) {
        lpaPartition(adj, p1);
    } else {
        wccPartition(adj, p1);
    }
    if (bindData->algo2Lpa) {
        lpaPartition(adj, p2);
    } else {
        wccPartition(adj, p2);
    }

    // (c1, c2) -> combined id, in order of first appearance.
    std::unordered_map<uint64_t, uint64_t> combo;
    std::vector<uint64_t> combined(numNodes);
    uint64_t nextId = 0;
    for (uint64_t v = 0; v < numNodes; ++v) {
        const auto key = p1[v] * (numNodes + 1) + p2[v];
        auto it = combo.find(key);
        if (it == combo.end()) {
            combo.emplace(key, nextId);
            combined[v] = nextId++;
        } else {
            combined[v] = it->second;
        }
    }

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto combinedVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    combinedVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), combinedVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        combinedVector->setValue<int64_t>(0, static_cast<int64_t>(combined[i]));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set PartitionIntersectionFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
