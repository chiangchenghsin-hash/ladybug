// LOCAL_DEGREE_SPARSIFICATION - keep edges with the highest local-degree score
// d(u)+d(v), preserving the top `ratio` fraction of undirected edges.
// P2-06 (NASH×GNN P2 batch): 降 GNN 计算量保持结构。对标 NetworKit
// `sparsification::LocalDegreeScore`.
//
//   CALL local_degree_sparsification('g', ratio := 0.5)
//     YIELD source, target, is_kept, rank
//
// All undirected edges are emitted as one row each (source/target = internal
// offsets). rank is 1-based over the score-descending order (ties broken by
// (source, target) ascending so it is stable); is_kept = rank <= ceil(ratio*m).
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
#include <cmath>
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char KEPT_COLUMN_NAME[] = "is_kept";
static constexpr char RANK_COLUMN_NAME[] = "rank";

static constexpr double DEFAULT_RATIO = 0.5;

struct LocalDegreeSparsificationBindData final : public GDSBindData {
    double ratio;

    LocalDegreeSparsificationBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        double ratio)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          ratio{ratio} {}

    LocalDegreeSparsificationBindData(const LocalDegreeSparsificationBindData& other)
        : GDSBindData{other}, ratio{other.ratio} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<LocalDegreeSparsificationBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    double ratio = DEFAULT_RATIO;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "ratio") {
            ratio = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (ratio < 0.0 || ratio > 1.0) {
        throw BinderException{"LOCAL_DEGREE_SPARSIFICATION ratio must be in [0, 1]."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(KEPT_COLUMN_NAME, LogicalType::BOOL()));
    columns.push_back(input->binder->createVariable(RANK_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<LocalDegreeSparsificationBindData>(std::move(columns),
        std::move(graphEntry), ratio);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<LocalDegreeSparsificationBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "LOCAL_DEGREE_SPARSIFICATION currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);

    // Enumerate undirected edges (u < v) with their local-degree score.
    struct EdgeScore {
        uint64_t u;
        uint64_t v;
        uint64_t score;
    };
    std::vector<EdgeScore> edges;
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto v : adj[u]) {
            if (v <= u) {
                continue;
            }
            edges.push_back(EdgeScore{u, v, adj[u].size() + adj[v].size()});
        }
    }
    const auto m = edges.size();

    std::sort(edges.begin(), edges.end(), [](const EdgeScore& a, const EdgeScore& b) {
        if (a.score != b.score) {
            return a.score > b.score;
        }
        if (a.u != b.u) {
            return a.u < b.u;
        }
        return a.v < b.v;
    });

    const auto keepCount = static_cast<uint64_t>(std::ceil(bindData->ratio * static_cast<double>(m)));

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto keptVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    keptVector->state = DataChunkState::getSingleValueDataChunkState();
    auto rankVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    rankVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), keptVector.get(),
        rankVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t rank = 0; rank < m; ++rank) {
        srcVector->setValue<int64_t>(0, static_cast<int64_t>(edges[rank].u));
        dstVector->setValue<int64_t>(0, static_cast<int64_t>(edges[rank].v));
        keptVector->setValue<bool>(0, rank < keepCount);
        rankVector->setValue<int64_t>(0, static_cast<int64_t>(rank + 1));
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

function_set LocalDegreeSparsificationFunction::getFunctionSet() {
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
