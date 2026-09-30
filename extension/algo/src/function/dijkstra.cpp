// DIJKSTRA - weighted single-source shortest path (directed, non-negative weights).
// P1-10 (NASH×GNN P1 batch): 交互图带权时的加权影响传播半径, 对标 NetworkX
// `single_source_dijkstra_path_length(G, source, weight)` (O(m log n)).
//
// Source node: either positional (`CALL dijkstra('g', 0)`) or named
// (`CALL dijkstra('g', source_node := 0)`); the value is the internal node
// offset (0-based), same convention as SHORTEST_PATH. Weight property name
// defaults to "weight"; a missing property or NULL value means weight 1.0.
// Negative weights raise a BinderException (对齐 NetworkX 语义).
#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/weighted_graph.h"
#include "function/algo_function.h"
#include "function/algo_param_utils.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <cstdint>
#include <queue>
#include <string>
#include <utility>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char DIST_COLUMN_NAME[] = "distance";

struct DijkstraBindData final : public GDSBindData {
    int64_t sourceOffset;
    std::string weightProperty;

    DijkstraBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t sourceOffset, std::string weightProperty)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          sourceOffset{sourceOffset}, weightProperty{std::move(weightProperty)} {}

    DijkstraBindData(const DijkstraBindData& other)
        : GDSBindData{other}, sourceOffset{other.sourceOffset},
          weightProperty{other.weightProperty} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DijkstraBindData>(*this);
    }
};

// Shared bind logic. `input->params.size()` is 2 for the positional-signature
// overload (`dijkstra('g', 0)`) and 1 for the named-only one
// (`dijkstra('g', source_node := 0)`).
static std::unique_ptr<TableFuncBindData> bindDijkstra(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    int64_t sourceOffset = -1;
    std::string weightProperty = "weight";
    if (input->params.size() >= 2) {
        sourceOffset = input->getLiteralVal<int64_t>(1);
    }
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "sourcenode") {
            sourceOffset = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "weightproperty") {
            weightProperty = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (sourceOffset < 0) {
        throw BinderException{"DIJKSTRA requires a source node offset "
                              "(positional 2nd argument or source_node := <offset>)."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(DIST_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<DijkstraBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, sourceOffset, weightProperty);
}

static void computeDijkstra(const WeightedGraph& wg, uint64_t source,
    std::vector<double>& dist) {
    const auto n = wg.numNodes;
    dist.assign(n, -1.0);
    if (source >= n) {
        throw BinderException{"DIJKSTRA source node offset out of range."};
    }
    using QEntry = std::pair<double, uint64_t>; // (dist, node)
    std::priority_queue<QEntry, std::vector<QEntry>, std::greater<QEntry>> pq;
    dist[source] = 0.0;
    pq.emplace(0.0, source);
    while (!pq.empty()) {
        const auto [d, u] = pq.top();
        pq.pop();
        if (d > dist[u]) {
            continue; // stale heap entry
        }
        for (const auto& [v, w] : wg.out[u]) {
            const double cand = d + w;
            if (dist[v] < 0.0 || cand < dist[v]) {
                dist[v] = cand;
                pq.emplace(cand, v);
            }
        }
    }
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
    auto bindData = input.bindData->constPtrCast<DijkstraBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"DIJKSTRA currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto wg = buildWeightedGraph(graph, tableID, numNodes, bindData->weightProperty);
    validateNonNegativeWeights(wg, "DIJKSTRA");
    std::vector<double> dist;
    computeDijkstra(wg, static_cast<uint64_t>(bindData->sourceOffset), dist);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto distVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    distVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), distVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        if (dist[i] < 0.0) {
            continue; // unreachable from source: omit (matches SHORTEST_PATH semantics)
        }
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        distVector->setValue<double>(0, dist[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set DijkstraFunction::getFunctionSet() {
    function_set result;
    // Overload 1: positional source offset (`dijkstra('g', 0)`).
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64});
    func->bindFunc = bindDijkstra;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    // Overload 2: named-only (`dijkstra('g', source_node := 0)`).
    auto funcNamed = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    funcNamed->bindFunc = bindDijkstra;
    funcNamed->tableFunc = tableFunc;
    funcNamed->initSharedStateFunc = initSharedState;
    funcNamed->initLocalStateFunc = TableFunction::initEmptyLocalState;
    funcNamed->canParallelFunc = [] { return false; };
    funcNamed->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    funcNamed->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(funcNamed));
    return result;
}

} // namespace algo_extension
} // namespace lbug
