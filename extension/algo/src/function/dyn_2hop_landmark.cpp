// DYN_2HOP_LANDMARK - BFS distance query from a node after one hypothetical
// edge insertion. P2-03 (NASH×GNN P2 batch): 动态快速距离查询。
// 对标 NetworKit `distance::Dyn2HopLandmark`.
//
//   CALL dyn_2hop_landmark('g', query_node, u := a, v := b)
//     YIELD target, distance
//
// Evaluates "the projection graph plus the extra undirected edge (u, v)" once
// (query_node / u / v = internal offsets) and returns, for every reachable node,
// its distance from query_node. If u/v are omitted the query runs on the
// projection as-is. Unreachable nodes are omitted.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
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
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
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
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char TARGET_COLUMN_NAME[] = "target";
static constexpr char DISTANCE_COLUMN_NAME[] = "distance";

struct Dyn2HopLandmarkBindData final : public GDSBindData {
    int64_t queryNode;
    int64_t u;
    int64_t v;
    bool hasDelta;

    Dyn2HopLandmarkBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t queryNode, int64_t u, int64_t v, bool hasDelta)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          queryNode{queryNode}, u{u}, v{v}, hasDelta{hasDelta} {}

    Dyn2HopLandmarkBindData(const Dyn2HopLandmarkBindData& other)
        : GDSBindData{other}, queryNode{other.queryNode}, u{other.u}, v{other.v},
          hasDelta{other.hasDelta} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<Dyn2HopLandmarkBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    const auto queryNode = input->getLiteralVal<int64_t>(1);
    int64_t u = -1;
    int64_t v = -1;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "u") {
            u = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "v") {
            v = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if ((u < 0) != (v < 0)) {
        throw BinderException{
            "DYN_2HOP_LANDMARK requires both u and v, or neither (no delta edge)."};
    }
    const bool hasDelta = u >= 0;

    expression_vector columns;
    columns.push_back(input->binder->createVariable(TARGET_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DISTANCE_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<Dyn2HopLandmarkBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, queryNode, u, v, hasDelta);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<Dyn2HopLandmarkBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "DYN_2HOP_LANDMARK currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;
    const auto query = static_cast<uint64_t>(bindData->queryNode);
    if (query >= numNodes) {
        throw BinderException{"DYN_2HOP_LANDMARK query_node out of range."};
    }

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t w = 0; w < adj.size(); ++w) {
        auto& nbrs = adj[w];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), w), nbrs.end());
    }
    if (bindData->hasDelta) {
        const auto u = static_cast<uint64_t>(bindData->u);
        const auto v = static_cast<uint64_t>(bindData->v);
        if (u >= numNodes || v >= numNodes) {
            throw BinderException{"DYN_2HOP_LANDMARK delta endpoint out of range."};
        }
        if (u != v) {
            adj[u].push_back(v);
            adj[v].push_back(u);
        }
    }

    std::vector<int64_t> dist(numNodes, -1);
    std::vector<uint64_t> queue;
    queue.reserve(numNodes);
    dist[query] = 0;
    queue.push_back(query);
    for (uint64_t head = 0; head < queue.size(); ++head) {
        const auto w = queue[head];
        for (const auto t : adj[w]) {
            if (dist[t] == -1) {
                dist[t] = dist[w] + 1;
                queue.push_back(t);
            }
        }
    }

    auto targetVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    targetVector->state = DataChunkState::getSingleValueDataChunkState();
    auto distVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    distVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{targetVector.get(), distVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t w = 0; w < numNodes; ++w) {
        if (dist[w] < 0) {
            continue;
        }
        targetVector->setValue<int64_t>(0, static_cast<int64_t>(w));
        distVector->setValue<int64_t>(0, dist[w]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Edge output (no node column): bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set Dyn2HopLandmarkFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
