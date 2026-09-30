// DIAMETER - exact graph diameter via BFS from every node (undirected).
// P1-12 (NASH×GNN P1 batch): 策略传播延迟; 小直径 = 均衡敏感。对标 NetworkX
// `diameter` (O(n(n+m))). Disconnected graphs return -1 (documented
// convention: 精确直径只在连通图上有定义); n <= 1 returns 0.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <cstdint>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char DIAMETER_COLUMN_NAME[] = "diameter";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    expression_vector columns;
    columns.push_back(input->binder->createVariable(DIAMETER_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{});
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

// Returns the exact diameter over the undirected view, or -1 when the graph is
// disconnected (or empty).
int64_t computeUndirectedDiameter(const std::vector<std::vector<uint64_t>>& adj) {
    const auto n = adj.size();
    if (n == 0) {
        return -1;
    }
    if (n == 1) {
        return 0;
    }
    int64_t diameter = 0;
    std::vector<int64_t> dist(n);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (uint64_t s = 0; s < n; ++s) {
        std::fill(dist.begin(), dist.end(), -1);
        dist[s] = 0;
        queue.clear();
        queue.push_back(s);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto u = queue[head];
            for (const auto v : adj[u]) {
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    queue.push_back(v);
                }
            }
        }
        for (uint64_t v = 0; v < n; ++v) {
            if (dist[v] == -1) {
                return -1; // unreachable pair: graph disconnected
            }
            if (dist[v] > diameter) {
                diameter = dist[v];
            }
        }
    }
    return diameter;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"DIAMETER currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    const auto diameter = computeUndirectedDiameter(adj);

    auto diameterVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    diameterVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{diameterVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    diameterVector->setValue<int64_t>(0, diameter);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar-only output: bare TableFunctionCall plan (same as BRIDGES/GRAPH_DIFF).
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set DiameterFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
