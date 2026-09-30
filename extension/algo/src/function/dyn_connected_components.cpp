// DYN_CONNECTED_COMPONENTS - incremental connected components after one
// hypothetical edge insertion. P2-01 (NASH×GNN P2 batch): 流式博弈增量连通分量。
// 对标 NetworKit `components::DynConnectedComponents`.
//
//   CALL dyn_connected_components('g', u, v, op := 'insert')
//     YIELD component, changed
//
// Table functions are stateless: each call evaluates "the projection graph plus
// the extra edge (u, v)" once (u/v = internal offsets). op is accepted for
// signature compatibility but only 'insert' is supported (delete cannot be
// expressed on a static projection). One row per resulting component:
//   component = the component id (the smallest node offset in it)
//   changed   = true iff that component was produced by merging two components
//               across the inserted edge (false for all components otherwise)
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

static constexpr char COMPONENT_COLUMN_NAME[] = "component";
static constexpr char CHANGED_COLUMN_NAME[] = "changed";

struct DynConnectedComponentsBindData final : public GDSBindData {
    int64_t u;
    int64_t v;

    DynConnectedComponentsBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        int64_t u, int64_t v)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}}, u{u}, v{v} {}

    DynConnectedComponentsBindData(const DynConnectedComponentsBindData& other)
        : GDSBindData{other}, u{other.u}, v{other.v} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DynConnectedComponentsBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    int64_t u = -1;
    int64_t v = -1;
    if (input->params.size() >= 3) {
        u = input->getLiteralVal<int64_t>(1);
        v = input->getLiteralVal<int64_t>(2);
    }
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "u") {
            u = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "v") {
            v = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "op") {
            const auto op = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
            if (op != "insert") {
                throw BinderException{
                    "DYN_CONNECTED_COMPONENTS op must be 'insert' (delete is not supported on "
                    "a static projection)."};
            }
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (u < 0 || v < 0) {
        throw BinderException{
            "DYN_CONNECTED_COMPONENTS requires endpoints u and v (internal offsets)."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(COMPONENT_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(CHANGED_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<DynConnectedComponentsBindData>(std::move(columns),
        std::move(graphEntry), u, v);
}

// Undirected connected components; the component id is the smallest offset in it.
std::vector<uint64_t> computeComponents(const std::vector<std::vector<uint64_t>>& adj) {
    const auto n = adj.size();
    std::vector<uint64_t> comp(n, UINT64_MAX);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (uint64_t s = 0; s < n; ++s) {
        if (comp[s] != UINT64_MAX) {
            continue;
        }
        comp[s] = s;
        queue.clear();
        queue.push_back(s);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto w = queue[head];
            for (const auto t : adj[w]) {
                if (comp[t] == UINT64_MAX) {
                    comp[t] = s;
                    queue.push_back(t);
                }
            }
        }
    }
    return comp;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<DynConnectedComponentsBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "DYN_CONNECTED_COMPONENTS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;
    const auto u = static_cast<uint64_t>(bindData->u);
    const auto v = static_cast<uint64_t>(bindData->v);
    if (u >= numNodes || v >= numNodes) {
        throw BinderException{"DYN_CONNECTED_COMPONENTS endpoint out of range."};
    }

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t w = 0; w < adj.size(); ++w) {
        auto& nbrs = adj[w];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), w), nbrs.end());
    }
    const auto before = computeComponents(adj);
    const bool merged = before[u] != before[v];
    if (u != v) {
        adj[u].push_back(v);
        adj[v].push_back(u);
    }
    const auto after = computeComponents(adj);

    // Unique component ids (smallest offsets), preserving ascending order.
    std::vector<uint64_t> ids;
    for (uint64_t w = 0; w < numNodes; ++w) {
        const auto c = after[w];
        if (std::find(ids.begin(), ids.end(), c) == ids.end()) {
            ids.push_back(c);
        }
    }

    auto compVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    compVector->state = DataChunkState::getSingleValueDataChunkState();
    auto changedVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    changedVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{compVector.get(), changedVector.get()};

    const auto mergedComp = after[u];
    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto c : ids) {
        compVector->setValue<int64_t>(0, static_cast<int64_t>(c));
        const bool changed = merged && c == mergedComp;
        changedVector->setValue<bool>(0, changed);
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

function_set DynConnectedComponentsFunction::getFunctionSet() {
    function_set result;
    auto positional = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64, LogicalTypeID::INT64});
    positional->bindFunc = bindFunc;
    positional->tableFunc = tableFunc;
    positional->initSharedStateFunc = GDSFunction::initSharedState;
    positional->initLocalStateFunc = TableFunction::initEmptyLocalState;
    positional->canParallelFunc = [] { return false; };
    positional->getLogicalPlanFunc = getLogicalPlan;
    positional->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(positional));

    auto named = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    named->bindFunc = bindFunc;
    named->tableFunc = tableFunc;
    named->initSharedStateFunc = GDSFunction::initSharedState;
    named->initLocalStateFunc = TableFunction::initEmptyLocalState;
    named->canParallelFunc = [] { return false; };
    named->getLogicalPlanFunc = getLogicalPlan;
    named->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(named));
    return result;
}

} // namespace algo_extension
} // namespace lbug
