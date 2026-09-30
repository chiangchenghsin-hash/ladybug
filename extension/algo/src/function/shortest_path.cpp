// SHORTEST_PATH — single-source shortest-path lengths via BFS (unweighted, directed).
// 最少步骤/最短路径: 任务依赖分析的价值点 —— 从源节点出发的 hop 距离。
// Directed semantics: 沿 fwd 邻居（出边）做 BFS，无权图每条边权重 1。对标 NetworkX
// `single_source_shortest_path_length(G, source)`（无权 = Dijkstra 的特例，O(m+n)）。
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

static constexpr char DIST_COLUMN_NAME[] = "distance";

// GDSBindData + 源节点 offset（内部 dense offset，0..numNodes-1）。
struct ShortestPathBindData final : public GDSBindData {
    int64_t sourceOffset;

    ShortestPathBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t sourceOffset)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          sourceOffset{sourceOffset} {}

    ShortestPathBindData(const ShortestPathBindData& other)
        : GDSBindData{other}, sourceOffset{other.sourceOffset} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ShortestPathBindData>(*this);
    }
};

static void computeShortestPath(const DirectedCSR& csr, uint64_t source,
    std::vector<int64_t>& dist) {
    const auto n = csr.numNodes;
    dist.assign(n, -1);
    if (source >= n) {
        throw BinderException{"SHORTEST_PATH source node offset out of range."};
    }
    dist[source] = 0;
    std::vector<uint64_t> queue;
    queue.reserve(n);
    queue.push_back(source);
    for (uint64_t head = 0; head < queue.size(); ++head) {
        const auto u = queue[head];
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            const auto v = *it;
            if (dist[v] == -1) {
                dist[v] = dist[u] + 1;
                queue.push_back(v);
            }
        }
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto sourceOffset = input->getLiteralVal<int64_t>(1);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(DIST_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<ShortestPathBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, sourceOffset);
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
    auto bindData = input.bindData->constPtrCast<ShortestPathBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"SHORTEST_PATH currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<int64_t> dist;
    computeShortestPath(csr, static_cast<uint64_t>(bindData->sourceOffset), dist);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto distVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    distVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), distVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        if (dist[i] < 0) {
            continue; // unreachable from source: omit (matches NetworkX single-source semantics)
        }
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        distVector->setValue<int64_t>(0, dist[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set ShortestPathFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64});
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
