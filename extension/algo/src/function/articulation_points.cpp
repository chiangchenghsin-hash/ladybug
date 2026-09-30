// ARTICULATION_POINTS — Tarjan articulation-point finding (undirected).
// 割点/关节点 = 删掉该节点（及其关联边）会断开图的节点（单点故障）。
// 角色深度扩展续: 单点故障分析的价值点 —— 找"删掉就断链"的关键步骤。
// Undirected semantics: the graph is viewed as undirected (双向边算一次), per
// buildUndirectedAdjacency. 对标 NetworkX `articulation_points(G)` (Hopcroft-Tarjan, O(m+n)).
#include "binder/binder.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/algo_function.h"
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
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char NODE_COLUMN_NAME[] = "node";

namespace {

// Tarjan articulation-point finding over an undirected adjacency list.
// A non-root node u is an articulation point iff some DFS child v satisfies
// low[v] >= disc[u]; a DFS root is an articulation point iff it has > 1 children.
struct ArticulationFinder {
    const std::vector<std::vector<uint64_t>>& adj;
    std::vector<int64_t> disc; // discovery time, -1 = unvisited
    std::vector<int64_t> low;
    std::vector<bool> isArticulation;
    int64_t time = 0;

    explicit ArticulationFinder(const std::vector<std::vector<uint64_t>>& a)
        : adj{a}, disc(a.size(), -1), low(a.size(), 0), isArticulation(a.size(), false) {}

    void find() {
        for (uint64_t s = 0; s < adj.size(); ++s) {
            if (disc[s] == -1) {
                dfs(s, UINT64_MAX);
            }
        }
    }

private:
    void dfs(uint64_t u, uint64_t parent) {
        disc[u] = low[u] = ++time;
        uint64_t children = 0;
        for (const uint64_t v : adj[u]) {
            if (v == parent) {
                continue; // tree edge back to parent (no parallel edges in deduped adj)
            }
            if (disc[v] == -1) {
                ++children;
                dfs(v, u);
                low[u] = std::min(low[u], low[v]);
                if (parent != UINT64_MAX && low[v] >= disc[u]) {
                    isArticulation[u] = true;
                }
            } else {
                low[u] = std::min(low[u], disc[v]);
            }
        }
        if (parent == UINT64_MAX && children > 1) {
            isArticulation[u] = true;
        }
    }
};

} // namespace

static void computeArticulationPoints(const DirectedCSR& csr, std::vector<uint64_t>& artPoints) {
    const auto adj = buildUndirectedAdjacency(csr);
    ArticulationFinder finder{adj};
    finder.find();
    for (uint64_t i = 0; i < finder.isArticulation.size(); ++i) {
        if (finder.isArticulation[i]) {
            artPoints.push_back(i);
        }
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput});
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"ARTICULATION_POINTS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<uint64_t> artPoints;
    computeArticulationPoints(csr, artPoints);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const uint64_t node : artPoints) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(node), tableID});
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set ArticulationPointsFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
