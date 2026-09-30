// MAX_FLOW + MIN_CUT_VALUE - maximum s-t flow / minimum s-t cut value
// (directed, weighted capacities). P1-13 (NASH×GNN P1 batch): 网络流博弈
// 均衡流量; 联盟/分割分析; GNN 边级约束特征。Dinic's algorithm (O(V^2 E)).
// The s-t min cut value equals the max-flow value (max-flow min-cut theorem),
// so MIN_CUT_VALUE shares the same computation and differs only in the column.
//
//   CALL max_flow('D', 0, 3, weight_property := 'weight')  -> (max_flow_value)
//   CALL min_cut_value('D', 0, 3)                          -> (min_cut_value)
// Source/sink are internal node offsets (0-based). Weight property defaults to
// "weight"; missing property / NULL -> capacity 1.0; negative capacity errors.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/max_flow.h"
#include "common/types/types.h"
#include "common/weighted_graph.h"
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

#include <cstdint>
#include <string>
#include <utility>
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

struct MaxFlowBindData final : public GDSBindData {
    int64_t sourceOffset;
    int64_t sinkOffset;
    std::string weightProperty;

    MaxFlowBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        int64_t sourceOffset, int64_t sinkOffset, std::string weightProperty)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          sourceOffset{sourceOffset}, sinkOffset{sinkOffset},
          weightProperty{std::move(weightProperty)} {}

    MaxFlowBindData(const MaxFlowBindData& other)
        : GDSBindData{other}, sourceOffset{other.sourceOffset}, sinkOffset{other.sinkOffset},
          weightProperty{other.weightProperty} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<MaxFlowBindData>(*this);
    }
};

// Shared bind. Positional overload: (graph, source, sink). Named overload:
// (graph, source_node := s, sink_node := t).
static std::unique_ptr<TableFuncBindData> bindMaxFlow(main::ClientContext* context,
    const TableFuncBindInput* input, const char* valueColumn) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    int64_t sourceOffset = -1;
    int64_t sinkOffset = -1;
    std::string weightProperty = "weight";
    if (input->params.size() >= 3) {
        sourceOffset = input->getLiteralVal<int64_t>(1);
        sinkOffset = input->getLiteralVal<int64_t>(2);
    }
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "sourcenode") {
            sourceOffset = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "sinknode") {
            sinkOffset = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "weightproperty") {
            weightProperty = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (sourceOffset < 0 || sinkOffset < 0) {
        throw BinderException{"MAX_FLOW/MIN_CUT_VALUE require source and sink offsets "
                              "(positional or source_node := / sink_node := )."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(valueColumn, LogicalType::DOUBLE()));
    return std::make_unique<MaxFlowBindData>(std::move(columns), std::move(graphEntry),
        sourceOffset, sinkOffset, weightProperty);
}

static std::unique_ptr<TableFuncBindData> bindMaxFlowFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    return bindMaxFlow(context, input, "max_flow_value");
}

static std::unique_ptr<TableFuncBindData> bindMinCutFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    return bindMaxFlow(context, input, "min_cut_value");
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
    auto bindData = input.bindData->constPtrCast<MaxFlowBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"MAX_FLOW/MIN_CUT_VALUE supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto wg = buildWeightedGraph(graph, tableID, numNodes, bindData->weightProperty);

    std::vector<std::pair<std::pair<uint64_t, uint64_t>, double>> arcs;
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto& [v, w] : wg.out[u]) {
            if (u != v) {
                arcs.emplace_back(std::make_pair(u, v), w);
            }
        }
    }
    const double flow = computeMaxFlow(numNodes, arcs,
        static_cast<uint64_t>(bindData->sourceOffset), static_cast<uint64_t>(bindData->sinkOffset));

    auto valueVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    valueVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{valueVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    valueVector->setValue<double>(0, flow);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar-only output: bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

// Builds both the positional (graph, source, sink) and named overloads.
static function_set buildMaxFlowFunctionSet(const char* name, table_func_bind_t bindFn) {
    function_set result;
    auto positional = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64, LogicalTypeID::INT64});
    positional->bindFunc = bindFn;
    positional->tableFunc = tableFunc;
    positional->initSharedStateFunc = initSharedState;
    positional->initLocalStateFunc = TableFunction::initEmptyLocalState;
    positional->canParallelFunc = [] { return false; };
    positional->getLogicalPlanFunc = getLogicalPlan;
    positional->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(positional));

    auto named = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    named->bindFunc = bindFn;
    named->tableFunc = tableFunc;
    named->initSharedStateFunc = initSharedState;
    named->initLocalStateFunc = TableFunction::initEmptyLocalState;
    named->canParallelFunc = [] { return false; };
    named->getLogicalPlanFunc = getLogicalPlan;
    named->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(named));
    return result;
}

function_set MaxFlowFunction::getFunctionSet() {
    return buildMaxFlowFunctionSet(name, bindMaxFlowFunc);
}

function_set MinCutValueFunction::getFunctionSet() {
    return buildMaxFlowFunctionSet(name, bindMinCutFunc);
}

} // namespace algo_extension
} // namespace lbug
