// ASSORTATIVITY — Newman (2002) degree-degree assortativity, graph-level scalar.
// 角色深度扩展: 正值 = 相似度节点相连(同类聚集/阵营同质化); 负值 = 异质相连(对立结构)。
// Undirected semantics: each undirected edge (pair) contributes once.
//   r = [M^-1 sum(ij) - (M^-1 sum((i+j)/2))^2] / [M^-1 sum((i^2+j^2)/2) - (M^-1 sum((i+j)/2))^2]
// A perfect star scores exactly -1; a regular graph (denominator ~ 0) scores 0.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <cmath>
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

static constexpr char SCORE_COLUMN_NAME[] = "assortativity_score";

static double computeAssortativity(const DirectedCSR& csr) {
    const auto n = csr.numNodes;
    if (n < 2) {
        return 0.0;
    }
    const auto adj = buildUndirectedAdjacency(csr);
    const auto deg = [&adj](uint64_t v) { return static_cast<double>(adj[v].size()); };

    double m = 0.0;      // number of undirected edges (self-loops count once)
    double sumJ = 0.0;   // sum over edges of (i+j)/2
    double sumJJ = 0.0;  // sum over edges of (i^2+j^2)/2
    double sumIJ = 0.0;  // sum over edges of i*j
    for (uint64_t u = 0; u < n; ++u) {
        for (const auto v : adj[u]) {
            if (v < u) {
                continue; // count each undirected pair once
            }
            const auto du = deg(u);
            if (v == u) {
                // self-loop: one edge with both endpoints = u
                m += 1.0;
                sumJ += du;
                sumJJ += du * du;
                sumIJ += du * du;
            } else {
                const auto dv = deg(v);
                m += 1.0;
                sumJ += (du + dv) / 2.0;
                sumJJ += (du * du + dv * dv) / 2.0;
                sumIJ += du * dv;
            }
        }
    }
    if (m == 0.0) {
        return 0.0;
    }
    const auto meanJ = sumJ / m;
    const auto denom = sumJJ / m - meanJ * meanJ;
    if (std::fabs(denom) < 1e-12) {
        return 0.0; // regular graph: assortativity undefined -> 0
    }
    return (sumIJ / m - meanJ * meanJ) / denom;
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
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
        throw BinderException{"ASSORTATIVITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto score = computeAssortativity(csr);

    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{scoreVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    scoreVector->setValue<double>(0, score);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// ASSORTATIVITY has no node output (pure scalar) -> custom logical plan hook
// (stock GDS hook dereferences output[0] as a node expression).
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set AssortativityFunction::getFunctionSet() {
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
