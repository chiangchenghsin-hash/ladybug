// BICONNECTED_COMPONENTS - Hopcroft-Tarjan biconnected components (undirected).
// P1-01 (NASH×GNN P1 batch): 割点所在双连通分量是策略依赖的「最小不可再分
// 单元」; GNN 分量内消息传递避免信息经单点瓶颈泄漏。对标 NetworkX
// `biconnected_components` (O(n+m)). Articulation points belong to every
// component they connect (one output row per (node, component) membership);
// isolated nodes form their own singleton component.
#include "binder/binder.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <cstdint>
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

static constexpr char NODE_COLUMN_NAME[] = "node";
static constexpr char COMPONENT_COLUMN_NAME[] = "component";

namespace {

// Iterative Hopcroft-Tarjan. An edge stack accumulates tree/back edges; when a
// DFS finishes a child v with low[v] >= disc[u], everything above the parent
// edge on the stack forms one biconnected component.
struct BiconnectedComponentsFinder {
    const std::vector<std::vector<uint64_t>>& adj;
    std::vector<int64_t> disc;
    std::vector<int64_t> low;
    // Per-node DFS cursor: next adjacency index to visit.
    std::vector<size_t> cursor;
    std::vector<uint64_t> parent;
    std::vector<std::pair<uint64_t, uint64_t>> edgeStack;
    std::vector<std::pair<uint64_t, uint64_t>> components; // (node, compId)
    int64_t time = 0;
    uint64_t numComponents = 0;

    explicit BiconnectedComponentsFinder(const std::vector<std::vector<uint64_t>>& a)
        : adj{a}, disc(a.size(), -1), low(a.size(), 0), cursor(a.size(), 0),
          parent(a.size(), UINT64_MAX) {}

    void find() {
        const auto n = adj.size();
        // Explicit DFS stack of visited nodes (path from root to current).
        std::vector<uint64_t> dfsStack;
        std::vector<uint8_t> seen(n, 0);
        for (uint64_t root = 0; root < n; ++root) {
            if (disc[root] != -1) {
                continue;
            }
            disc[root] = low[root] = ++time;
            dfsStack.push_back(root);
            while (!dfsStack.empty()) {
                const auto u = dfsStack.back();
                if (cursor[u] < adj[u].size()) {
                    const auto v = adj[u][cursor[u]++];
                    if (v == parent[u]) {
                        continue; // edge back to the DFS parent (deduped: no parallels)
                    }
                    if (v == u) {
                        continue; // self-loops are ignored (fallback below covers the node)
                    }
                    if (disc[v] == -1) {
                        disc[v] = low[v] = ++time;
                        parent[v] = u;
                        edgeStack.emplace_back(u, v);
                        dfsStack.push_back(v);
                    } else if (disc[v] < disc[u]) {
                        // Back edge to an already-discovered node (above u).
                        edgeStack.emplace_back(u, v);
                        low[u] = std::min(low[u], disc[v]);
                    }
                } else {
                    dfsStack.pop_back();
                    const auto p = parent[u];
                    if (p == UINT64_MAX) {
                        continue;
                    }
                    low[p] = std::min(low[p], low[u]);
                    if (low[u] >= disc[p]) {
                        // Pop the edge stack down to (and including) (p, u):
                        // one biconnected component.
                        std::vector<uint64_t> members;
                        while (!edgeStack.empty()) {
                            const auto [a, b] = edgeStack.back();
                            edgeStack.pop_back();
                            members.push_back(a);
                            members.push_back(b);
                            if (a == p && b == u) {
                                break;
                            }
                        }
                        std::sort(members.begin(), members.end());
                        members.erase(std::unique(members.begin(), members.end()), members.end());
                        for (const auto m : members) {
                            components.emplace_back(m, numComponents);
                            seen[m] = 1;
                        }
                        numComponents++;
                    }
                }
            }
        }
        // Nodes not covered by any edge component (isolated or self-loop-only)
        // get their own singleton component.
        for (uint64_t v = 0; v < n; ++v) {
            if (!seen[v]) {
                components.emplace_back(v, numComponents++);
            }
        }
    }
};

} // namespace

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(COMPONENT_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput});
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
        throw BinderException{
            "BICONNECTED_COMPONENTS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    BiconnectedComponentsFinder finder{adj};
    finder.find();

    // Deterministic output order: by (component, node).
    auto& memberships = finder.components;
    std::sort(memberships.begin(), memberships.end(),
        [](const auto& a, const auto& b) { return a.second < b.second || (a.second == b.second && a.first < b.first); });

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto componentVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    componentVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), componentVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& [node, component] : memberships) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(node), tableID});
        componentVector->setValue<int64_t>(0, static_cast<int64_t>(component));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set BiconnectedComponentsFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
