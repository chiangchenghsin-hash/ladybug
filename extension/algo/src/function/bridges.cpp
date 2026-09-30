// BRIDGES — Tarjan bridge-finding (undirected). 桥 = 删掉就断链的关键边（割边）。
// 角色深度扩展续: 单点故障/关键路径分析的价值点 —— 找"删掉就断链"的边。
// Undirected semantics: the graph is viewed as undirected (双向边算一次), per
// buildUndirectedAdjacency. 对标 NetworkX `bridges(G)` (chain-decomposition, O(m+n)).
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
#include "processor/operator/table_function_call.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <algorithm>
#include <cstdint>
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

static constexpr char SRC_COLUMN_NAME[] = "src";
static constexpr char DST_COLUMN_NAME[] = "dst";

namespace {

// Tarjan bridge-finding over an undirected adjacency list.
// An edge (u, v) — with v discovered via u in the DFS tree — is a bridge iff
// low[v] > disc[u] (no back edge from v's subtree reaches u or an ancestor of u).
// buildUndirectedAdjacency dedupes neighbors, so there are no parallel edges to
// special-case; self-loops are no-ops (disc[v] already set → back-edge branch).
struct BridgeFinder {
    const std::vector<std::vector<uint64_t>>& adj;
    std::vector<int64_t> disc; // discovery time, -1 = unvisited
    std::vector<int64_t> low;
    std::vector<std::pair<uint64_t, uint64_t>> bridges;
    int64_t time = 0;

    explicit BridgeFinder(const std::vector<std::vector<uint64_t>>& a)
        : adj{a}, disc(a.size(), -1), low(a.size(), 0) {}

    void find() {
        for (uint64_t s = 0; s < adj.size(); ++s) {
            if (disc[s] == -1) {
                dfs(s, UINT64_MAX);
            }
        }
    }

private:
    void dfs(uint64_t u, uint64_t parent) {
        disc[u] = low[u] = ++time;
        for (const uint64_t v : adj[u]) {
            if (v == parent) {
                continue; // tree edge back to parent (no parallel edges in deduped adj)
            }
            if (disc[v] == -1) {
                dfs(v, u);
                low[u] = std::min(low[u], low[v]);
                if (low[v] > disc[u]) {
                    bridges.emplace_back(u, v);
                }
            } else {
                low[u] = std::min(low[u], disc[v]);
            }
        }
    }
};

} // namespace

static void computeBridges(const DirectedCSR& csr,
    std::vector<std::pair<uint64_t, uint64_t>>& bridges) {
    const auto adj = buildUndirectedAdjacency(csr);
    BridgeFinder finder{adj};
    finder.find();
    bridges = std::move(finder.bridges);
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
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
        throw BinderException{"BRIDGES currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<std::pair<uint64_t, uint64_t>> bridges;
    computeBridges(csr, bridges);

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& [src, dst] : bridges) {
        srcVector->setValue<int64_t>(0, static_cast<int64_t>(src));
        dstVector->setValue<int64_t>(0, static_cast<int64_t>(dst));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// BRIDGES 输出的是边（src/dst 两个 offset），没有 GDS 的"单节点输出"列，因此不能走
// GDSFunction::getLogicalPlan（它会访问 output[0] 做节点属性扫描/join）。使用与 GRAPH_DIFF
// 相同的简化逻辑计划：只建 TableFunctionCall，不附加节点属性扫描。
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set BridgesFunction::getFunctionSet() {
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
