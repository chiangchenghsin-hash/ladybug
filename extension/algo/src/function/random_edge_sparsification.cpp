// RANDOM_EDGE_SPARSIFICATION - keep each undirected edge independently with
// probability `ratio`, seeded so runs are reproducible. P2-08 (NASH×GNN P2
// batch): DropEdge 正则化。对标 NetworKit `sparsification::RandomEdgeScore`.
//
//   CALL random_edge_sparsification('g', ratio := 0.5, seed := 42)
//     YIELD source, target, is_kept
//
// All undirected edges are emitted as one row each (source/target = internal
// offsets); is_kept is drawn from a fixed `std::mt19937(seed)`.
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

#include <cstdint>
#include <random>
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char KEPT_COLUMN_NAME[] = "is_kept";

static constexpr double DEFAULT_RATIO = 0.5;
static constexpr int64_t DEFAULT_SEED = 42;

struct RandomEdgeSparsificationBindData final : public GDSBindData {
    double ratio;
    uint64_t seed;

    RandomEdgeSparsificationBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        double ratio, uint64_t seed)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          ratio{ratio}, seed{seed} {}

    RandomEdgeSparsificationBindData(const RandomEdgeSparsificationBindData& other)
        : GDSBindData{other}, ratio{other.ratio}, seed{other.seed} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<RandomEdgeSparsificationBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    double ratio = DEFAULT_RATIO;
    int64_t seed = DEFAULT_SEED;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "ratio") {
            ratio = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "seed") {
            seed = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (ratio < 0.0 || ratio > 1.0) {
        throw BinderException{"RANDOM_EDGE_SPARSIFICATION ratio must be in [0, 1]."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(KEPT_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<RandomEdgeSparsificationBindData>(std::move(columns),
        std::move(graphEntry), ratio, seed);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<RandomEdgeSparsificationBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "RANDOM_EDGE_SPARSIFICATION currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);

    std::mt19937 rng(static_cast<uint64_t>(bindData->seed));
    std::uniform_real_distribution<double> draw(0.0, 1.0);

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto keptVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    keptVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), keptVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto v : adj[u]) {
            if (v <= u) {
                continue;
            }
            const bool kept = draw(rng) < bindData->ratio;
            srcVector->setValue<int64_t>(0, static_cast<int64_t>(u));
            dstVector->setValue<int64_t>(0, static_cast<int64_t>(v));
            keptVector->setValue<bool>(0, kept);
            localFT->append(vectors);
        }
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

function_set RandomEdgeSparsificationFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
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
