// ECCENTRICITY + RADIUS - undirected eccentricity (max BFS distance per node)
// and graph radius (min eccentricity). P2-13 (NASH×GNN P2 batch): 星形中心
// ecc=1 (radius=1); 策略传播的最大/最小延迟上界。对标 NetworkX
// `eccentricity` / `radius`. Both are O(n(n+m)).
//
//   CALL eccentricity('S') YIELD node, eccentricity   -- 每节点一行; 不可达节点省略
//   CALL radius('S') YIELD radius                      -- 单行标量; 不连通 -> -1
//
// Disconnected convention (documented): a node with an unreachable pair has
// infinite eccentricity (NetworkX) -> ECCENTRICITY omits that row, RADIUS
// reports -1 (same convention as DIAMETER).
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
#include <limits>
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

static constexpr char ECCENTRICITY_COLUMN_NAME[] = "eccentricity";
static constexpr char RADIUS_COLUMN_NAME[] = "radius";
static constexpr int64_t INF_ECC = std::numeric_limits<int64_t>::max();

// Per-node eccentricity over the undirected view; INT64_MAX marks a node that
// cannot reach every other node (infinite eccentricity in NetworkX).
std::vector<int64_t> computeEccentricities(const std::vector<std::vector<uint64_t>>& adj) {
    const auto n = adj.size();
    std::vector<int64_t> ecc(n, 0);
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
        int64_t maxDist = 0;
        bool disconnected = false;
        for (uint64_t v = 0; v < n; ++v) {
            if (dist[v] == -1) {
                disconnected = true;
                break;
            }
            if (dist[v] > maxDist) {
                maxDist = dist[v];
            }
        }
        ecc[s] = disconnected ? INF_ECC : maxDist;
    }
    return ecc;
}

// ---- ECCENTRICITY ---------------------------------------------------------------

static std::unique_ptr<TableFuncBindData> bindEccentricity(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(ECCENTRICITY_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput});
}

static offset_t tableFuncEccentricity(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"ECCENTRICITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    const auto ecc = computeEccentricities(adj);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto eccVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    eccVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), eccVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        if (ecc[i] == INF_ECC) {
            continue; // unreachable pair: infinite eccentricity, omit the row
        }
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        eccVector->setValue<int64_t>(0, ecc[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set EccentricityFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindEccentricity;
    func->tableFunc = tableFuncEccentricity;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

// ---- RADIUS ----------------------------------------------------------------------

static std::unique_ptr<TableFuncBindData> bindRadius(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    expression_vector columns;
    columns.push_back(input->binder->createVariable(RADIUS_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{});
}

// Min eccentricity over a connected graph; -1 when disconnected or empty
// (mirrors DIAMETER's convention).
int64_t computeRadius(const std::vector<int64_t>& ecc) {
    if (ecc.empty()) {
        return -1;
    }
    int64_t radius = INF_ECC;
    for (const auto e : ecc) {
        if (e == INF_ECC) {
            return -1; // any unreachable pair: graph disconnected
        }
        if (e < radius) {
            radius = e;
        }
    }
    return radius;
}

static offset_t tableFuncRadius(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"RADIUS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    const auto ecc = computeEccentricities(adj);
    const auto radius = computeRadius(ecc);

    auto radiusVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    radiusVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{radiusVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    radiusVector->setValue<int64_t>(0, radius);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar-only output: bare TableFunctionCall plan (same as DIAMETER).
static void getLogicalPlanRadius(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set RadiusFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindRadius;
    func->tableFunc = tableFuncRadius;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = getLogicalPlanRadius;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
