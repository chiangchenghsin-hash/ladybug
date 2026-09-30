// MAX_WEIGHT_MATCHING - maximum-weight matching (greedy 1/2-approximation).
// P2-11 (NASH×GNN P2 batch): 双边市场稳定匹配初始化; 超边。对标 NetworKit
// `matching::PathGrowingMatcher` (近似), NOT the exact NetworkX Blossom —
// documented divergence: this is a weight-descending greedy (1/2-approximation).
//
//   CALL max_weight_matching('g') YIELD source, target, weight, is_matched
//
// Weighted undirected view (parallel arcs folded to max weight; missing weight
// -> 1.0). All undirected edges are emitted once (source/target = internal
// offsets); is_matched marks the edges chosen by the greedy.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/weighted_graph.h"
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char WEIGHT_COLUMN_NAME[] = "weight";
static constexpr char MATCHED_COLUMN_NAME[] = "is_matched";

static constexpr char WEIGHT_PROPERTY[] = "weight";

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(WEIGHT_COLUMN_NAME, LogicalType::DOUBLE()));
    columns.push_back(input->binder->createVariable(MATCHED_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{});
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"MAX_WEIGHT_MATCHING currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto wg = buildWeightedGraph(graph, tableID, numNodes, WEIGHT_PROPERTY);

    // Undirected edges (u < v) with their weight.
    struct Edge {
        uint64_t u;
        uint64_t v;
        double weight;
    };
    std::vector<Edge> edges;
    for (uint64_t u = 0; u < wg.und.size(); ++u) {
        for (const auto& [v, w] : wg.und[u]) {
            if (v <= u) {
                continue;
            }
            edges.push_back(Edge{u, v, w});
        }
    }
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) {
        if (a.weight != b.weight) {
            return a.weight > b.weight;
        }
        if (a.u != b.u) {
            return a.u < b.u;
        }
        return a.v < b.v;
    });

    std::vector<uint8_t> matched(numNodes, 0);
    std::vector<uint8_t> chosen(edges.size(), 0);
    for (size_t i = 0; i < edges.size(); ++i) {
        if (!matched[edges[i].u] && !matched[edges[i].v]) {
            matched[edges[i].u] = 1;
            matched[edges[i].v] = 1;
            chosen[i] = 1;
        }
    }

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto weightVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    weightVector->state = DataChunkState::getSingleValueDataChunkState();
    auto matchedVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    matchedVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), weightVector.get(),
        matchedVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (size_t i = 0; i < edges.size(); ++i) {
        srcVector->setValue<int64_t>(0, static_cast<int64_t>(edges[i].u));
        dstVector->setValue<int64_t>(0, static_cast<int64_t>(edges[i].v));
        weightVector->setValue<double>(0, edges[i].weight);
        matchedVector->setValue<bool>(0, chosen[i] == 1);
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

function_set MaxWeightMatchingFunction::getFunctionSet() {
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
