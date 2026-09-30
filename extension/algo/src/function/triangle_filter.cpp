// TRIANGLE_FILTER - keep undirected edges that participate in at least
// `min_triangles` triangles. P2-09 (NASH×GNN P2 batch): 强化局部协作结构。
// 对标 NetworKit `sparsification::TriangleEdgeScore`. An edge's triangle count
// is |N(u) ∩ N(v)| (the same support enumeration as K_TRUSS).
//
//   CALL triangle_filter('g', min_triangles := 2)
//     YIELD source, target, triangle_count
//
// Only kept edges are emitted (source/target = internal offsets). Self-loops
// are ignored; an edge in no triangle has count 0.
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char TRIANGLE_COUNT_COLUMN_NAME[] = "triangle_count";

static constexpr int64_t DEFAULT_MIN_TRIANGLES = 2;

struct TriangleFilterBindData final : public GDSBindData {
    int64_t minTriangles;

    TriangleFilterBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        int64_t minTriangles)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          minTriangles{minTriangles} {}

    TriangleFilterBindData(const TriangleFilterBindData& other)
        : GDSBindData{other}, minTriangles{other.minTriangles} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<TriangleFilterBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    int64_t minTriangles = DEFAULT_MIN_TRIANGLES;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "mintriangles") {
            minTriangles = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (minTriangles < 0) {
        throw BinderException{"TRIANGLE_FILTER min_triangles must be >= 0."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(
        input->binder->createVariable(TRIANGLE_COUNT_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<TriangleFilterBindData>(std::move(columns), std::move(graphEntry),
        minTriangles);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<TriangleFilterBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"TRIANGLE_FILTER currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t u = 0; u < adj.size(); ++u) {
        auto& nbrs = adj[u];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), u), nbrs.end());
    }

    // Enumerate undirected edges (u < v) with triangle count |N(u) ∩ N(v)|.
    struct EdgeCount {
        uint64_t u;
        uint64_t v;
        uint64_t count;
    };
    std::vector<EdgeCount> edges;
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto v : adj[u]) {
            if (v <= u) {
                continue;
            }
            const auto& nu = adj[u];
            const auto& nv = adj[v];
            size_t i = 0, j = 0;
            uint64_t count = 0;
            while (i < nu.size() && j < nv.size()) {
                if (nu[i] < nv[j]) {
                    i++;
                } else if (nv[j] < nu[i]) {
                    j++;
                } else {
                    count++;
                    i++;
                    j++;
                }
            }
            edges.push_back(EdgeCount{u, v, count});
        }
    }

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto countVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    countVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), countVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& e : edges) {
        if (static_cast<int64_t>(e.count) < bindData->minTriangles) {
            continue;
        }
        srcVector->setValue<int64_t>(0, static_cast<int64_t>(e.u));
        dstVector->setValue<int64_t>(0, static_cast<int64_t>(e.v));
        countVector->setValue<int64_t>(0, static_cast<int64_t>(e.count));
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

function_set TriangleFilterFunction::getFunctionSet() {
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
