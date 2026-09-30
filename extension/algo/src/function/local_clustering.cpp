// LOCAL_CLUSTERING_COEFFICIENT — Watts & Strogatz (1998), undirected.
// 角色深度扩展: 低 lcc = 角色社会圈松散 = 游离/神秘/孤独角色候选。
// lcc(v) = 2 * (#edges among v's neighbors) / (deg(v) * (deg(v)-1));
// nodes with degree < 2 score 0 by definition. Undirected semantics (双向边算一次).
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

#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char LCC_COLUMN_NAME[] = "lcc";

static void computeLocalClustering(const DirectedCSR& csr, std::vector<double>& lcc) {
    const auto n = csr.numNodes;
    lcc.assign(n, 0.0);
    const auto adj = buildUndirectedAdjacency(csr);

    std::vector<char> isNeighborOfU(n, 0);
    std::vector<uint64_t> touched;
    touched.reserve(n);
    for (uint64_t u = 0; u < n; ++u) {
        const auto& neighbors = adj[u];
        const auto deg = neighbors.size();
        if (deg < 2) {
            continue;
        }
        for (const auto v : neighbors) {
            isNeighborOfU[v] = 1;
            touched.push_back(v);
        }
        // Each unordered pair of neighbors (v, w) that is itself an edge forms a
        // triangle with u; the double loop counts every such pair twice, which is
        // exactly the 2T factor in the Watts-Strogatz formula.
        uint64_t pairs = 0;
        for (const auto v : neighbors) {
            for (const auto w : adj[v]) {
                if (isNeighborOfU[w] && w != u && w != v) {
                    pairs++;
                }
            }
        }
        lcc[u] = static_cast<double>(pairs) / (static_cast<double>(deg) * static_cast<double>(deg - 1));
        for (const auto t : touched) {
            isNeighborOfU[t] = 0;
        }
        touched.clear();
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(LCC_COLUMN_NAME, LogicalType::DOUBLE()));
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
        throw BinderException{
            "LOCAL_CLUSTERING_COEFFICIENT currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<double> lcc;
    computeLocalClustering(csr, lcc);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto lccVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    lccVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), lccVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        lccVector->setValue<double>(0, lcc[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set LocalClusteringCoefficientFunction::getFunctionSet() {
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
