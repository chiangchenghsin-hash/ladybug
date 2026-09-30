// TRIANGLE_COUNT + GLOBAL_CLUSTERING - triangle density of the undirected view.
// P1-14 (NASH×GNN P1 batch): 三角形/传递性 = 局部协作密度。对标 NetworkX
// `triangles` (sum / 3) and `transitivity` (3 * triangles / triads).
// TRIANGLE_COUNT('g') -> (count INT64)
// GLOBAL_CLUSTERING('g') -> (clustering_coefficient DOUBLE)
// Self-loops are ignored. Triangles are counted once by orienting each edge
// u < v and counting common neighbors w > v.
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

namespace {

// Undirected adjacency with self-loops removed (lists stay sorted+unique).
std::vector<std::vector<uint64_t>> buildSimpleUndirectedAdjacency(const DirectedCSR& csr) {
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t u = 0; u < adj.size(); ++u) {
        auto& nbrs = adj[u];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), u), nbrs.end());
    }
    return adj;
}

// Number of triangles. Each triangle {a < b < c} is credited exactly once via
// its lowest edge (a, b) counting the common neighbor c > b.
uint64_t countTriangles(const std::vector<std::vector<uint64_t>>& adj) {
    const auto n = adj.size();
    uint64_t count = 0;
    for (uint64_t u = 0; u < n; ++u) {
        for (const auto v : adj[u]) {
            if (v <= u) {
                continue; // only the ascending orientation of the edge
            }
            // |N(u) ∩ N(v) ∩ {w > v}|: triangles with the third vertex above v.
            const auto& nu = adj[u];
            const auto& nv = adj[v];
            size_t i = 0, j = 0;
            while (i < nu.size() && j < nv.size()) {
                if (nu[i] < nv[j]) {
                    i++;
                } else if (nv[j] < nu[i]) {
                    j++;
                } else {
                    if (nu[i] > v) {
                        count++;
                    }
                    i++;
                    j++;
                }
            }
        }
    }
    return count;
}

} // namespace

// ---- shared glue (both functions differ only in the output column) ----

static std::unique_ptr<TableFuncBindData> bindTriangleCount(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    expression_vector columns;
    columns.push_back(input->binder->createVariable("count", LogicalType::INT64()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{});
}

static std::unique_ptr<TableFuncBindData> bindGlobalClustering(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    expression_vector columns;
    columns.push_back(
        input->binder->createVariable("clustering_coefficient", LogicalType::DOUBLE()));
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

// Builds the CSR + simple adjacency, returns the graph (via the shared state).
static offset_t tableFuncCommon(const TableFuncInput& input, bool globalClustering) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "TRIANGLE_COUNT/GLOBAL_CLUSTERING supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildSimpleUndirectedAdjacency(csr);
    const auto triangles = countTriangles(adj);

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    if (globalClustering) {
        // transitivity = 3 * triangles / (# paths of length 2)
        uint64_t triads = 0;
        for (const auto& nbrs : adj) {
            const auto d = nbrs.size();
            triads += d * (d - 1) / 2;
        }
        double coeff = 0.0;
        if (triads > 0) {
            coeff = 3.0 * static_cast<double>(triangles) / static_cast<double>(triads);
        }
        auto vec = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
        vec->state = DataChunkState::getSingleValueDataChunkState();
        std::vector<ValueVector*> vectors{vec.get()};
        vec->setValue<double>(0, coeff);
        localFT->append(vectors);
    } else {
        auto vec = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
        vec->state = DataChunkState::getSingleValueDataChunkState();
        std::vector<ValueVector*> vectors{vec.get()};
        vec->setValue<int64_t>(0, static_cast<int64_t>(triangles));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static offset_t tableFuncTriangle(const TableFuncInput& input, TableFuncOutput&) {
    return tableFuncCommon(input, false);
}

static offset_t tableFuncClustering(const TableFuncInput& input, TableFuncOutput&) {
    return tableFuncCommon(input, true);
}

// Scalar-only outputs: bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set TriangleCountFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindTriangleCount;
    func->tableFunc = tableFuncTriangle;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

function_set GlobalClusteringFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindGlobalClustering;
    func->tableFunc = tableFuncClustering;
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
