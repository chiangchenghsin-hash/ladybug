// TOPOLOGICAL_SORT — DAG 拓扑排序 + 最长路径深度（依赖图分层验证）。
// 复用 DirectedCSR::detectDAG（Kahn 算法，与 VF2++ 模式侧共享）。
// 单图单 node 输出：完全走标准 GDS 流程（bindNodeOutput + GDSFunction 计划钩子）。
#include "binder/binder.h"
#include "binder/expression/node_expression.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "processor/execution_context.h"
#include "processor/operator/table_function_call.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char TOPO_RANK_COLUMN_NAME[] = "topo_rank";
static constexpr char DEPTH_COLUMN_NAME[] = "depth";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(TOPO_RANK_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DEPTH_COLUMN_NAME, LogicalType::INT64()));

    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{std::move(nodeOutput)});
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

static DirectedCSR buildCSR(table_id_t tableID, offset_t numNodes, Graph* graph) {
    std::vector<std::pair<uint64_t, uint64_t>> edges;
    const auto nbrTables = graph->getRelInfos(tableID);
    const auto nbrInfo = nbrTables[0];
    const auto scanState = graph->prepareRelScan(*nbrInfo.relGroupEntry, nbrInfo.relTableID,
        nbrInfo.dstTableID, {}, false /*randomLookup*/);
    for (offset_t nodeId = 0; nodeId < numNodes; ++nodeId) {
        const nodeID_t nid = {nodeId, tableID};
        for (auto chunk : graph->scanFwd(nid, *scanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                edges.emplace_back(nodeId, neighbors[i].offset);
            });
        }
    }
    auto csr = buildDirectedCSR(numNodes, edges);
    detectDAG(csr);
    return csr;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"TOPOLOGICAL_SORT currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    // 1. Projected graph -> directed CSR (Kahn: isDAG + depth + topoOrder).
    auto csr = buildCSR(tableID, numNodes, graph);
    if (!csr.isDAG) {
        throw BinderException{
            "Graph contains a cycle; TOPOLOGICAL_SORT is only defined for DAGs. "
            "Use STRONGLY_CONNECTED_COMPONENTS to locate the cycle."};
    }

    // 2. Stream (node, topo_rank, depth) rows.
    auto nodeVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeVector->state = DataChunkState::getSingleValueDataChunkState();
    auto rankVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    rankVector->state = DataChunkState::getSingleValueDataChunkState();
    auto depthVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    depthVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeVector.get(), rankVector.get(), depthVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t rank = 0; rank < csr.topoOrder.size(); ++rank) {
        const auto u = csr.topoOrder[rank];
        nodeVector->setValue<nodeID_t>(0, nodeID_t{u, tableID});
        rankVector->setValue<int64_t>(0, static_cast<int64_t>(rank));
        depthVector->setValue<int64_t>(0, csr.depth[u]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set TopologicalSortFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(TopologicalSortFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY});
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
