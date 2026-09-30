// GRAPH_SIGNATURE — 架构漂移检测特征向量。
// 一图一签名（多行 feature/value）：节点/边规模、度分布、双向边/自环、
// SCC/WCC 数、DAG 判定 + 最长路径深度。
// 消费路径：
//   1. 每架构快照投影一图 → 一行签名向量 → detect_drift_points（libtimeseries）
//      找时间序列漂移点（架构退化/演化检测）
//   2. 两快照签名向量差的 L2 范数 → 连续漂移度量（增量快照 diff 的量化基础）
// 复用：DirectedCSR + detectDAG（Kahn，与 TOPOLOGICAL_SORT/VF2++ 共享）。
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/graph_signature.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "processor/operator/table_function_call.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <utility>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char FEATURE_COLUMN_NAME[] = "feature";
static constexpr char VALUE_COLUMN_NAME[] = "value";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    expression_vector columns;
    columns.push_back(input->binder->createVariable(FEATURE_COLUMN_NAME, LogicalType::STRING()));
    columns.push_back(input->binder->createVariable(VALUE_COLUMN_NAME, LogicalType::DOUBLE()));
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

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"GRAPH_SIGNATURE currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto sig = computeSignature(csr);

    // Stream (feature, value) rows.
    auto featureVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    featureVector->state = DataChunkState::getSingleValueDataChunkState();
    auto valueVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    valueVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{featureVector.get(), valueVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& [feature, value] : sig.features) {
        featureVector->setValue(0, feature);
        valueVector->setValue<double>(0, value);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// GRAPH_SIGNATURE has no node output (pure value columns) -> custom logical plan
// hook (stock GDS hook dereferences output[0] as a node expression).
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set GraphSignatureFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GraphSignatureFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY});
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
