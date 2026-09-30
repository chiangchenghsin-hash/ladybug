// K_TRUSS - k-truss decomposition (undirected, unweighted). P1-02 (NASH×GNN
// P1 batch, P1.5 priority): 高 truss 子图 = 高内聚策略联盟; GNN truss 值作边
// 特征。对标 NetworkX `k_truss` / NetWorKit `KTruss`. An edge's truss number
// tau(e) = max k such that e belongs to a k-truss (an edge in t triangles can
// be in at most a (t+2)-truss). Edges with truss number >= k are returned.
//
// Algorithm (Wang-Cheng peeling): edges carry a "support" = number of triangles
// containing them; repeatedly pop the minimum-support edge at the current
// truss level (level = support + 3), assign tau = level - 1, and decrement the
// support of the two other edges in each triangle it closed. Self-loops are
// ignored; a bridge/edge in no triangle has truss number 2.
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
#include <queue>
#include <unordered_map>
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char TRUSS_COLUMN_NAME[] = "truss_number";

static constexpr int64_t DEFAULT_K = 3;

struct KTrussBindData final : public GDSBindData {
    int64_t k;

    KTrussBindData(expression_vector columns, graph::NativeGraphEntry graphEntry, int64_t k)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}}, k{k} {}

    KTrussBindData(const KTrussBindData& other) : GDSBindData{other}, k{other.k} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<KTrussBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    int64_t k = DEFAULT_K;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "k") {
            k = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (k < 2) {
        throw BinderException{"K_TRUSS k must be >= 2."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(TRUSS_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<KTrussBindData>(std::move(columns), std::move(graphEntry), k);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

namespace {

struct TrussResult {
    // Each edge (u < v) with its truss number.
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, uint64_t>> edges;
};

TrussResult computeTruss(const std::vector<std::vector<uint64_t>>& adj) {
    TrussResult result;
    const auto n = adj.size();
    if (n == 0) {
        return result;
    }

    // Enumerate edges u < v (no self-loops in adj already).
    struct Edge {
        uint64_t u;
        uint64_t v;
        uint64_t support;
        bool alive = true;
        uint64_t truss = 2; // minimum truss number
    };
    std::vector<Edge> edges;
    std::unordered_map<uint64_t, uint64_t> edgeId; // key = u*n+v -> index
    for (uint64_t u = 0; u < n; ++u) {
        for (const auto v : adj[u]) {
            if (v <= u) {
                continue;
            }
            const auto key = u * n + v;
            edgeId[key] = edges.size();
            edges.push_back(Edge{u, v, 0});
        }
    }
    const auto m = edges.size();
    if (m == 0) {
        return result;
    }

    // Support = number of triangles containing the edge = |N(u) ∩ N(v)|.
    for (auto& e : edges) {
        const auto& nu = adj[e.u];
        const auto& nv = adj[e.v];
        size_t i = 0, j = 0;
        while (i < nu.size() && j < nv.size()) {
            if (nu[i] < nv[j]) {
                i++;
            } else if (nv[j] < nu[i]) {
                j++;
            } else {
                e.support++;
                i++;
                j++;
            }
        }
    }

    // Peeling by ascending support with lazy-deletion min-heap. At truss level
    // `level` every edge with current support < level - 2 is peeled (and gets
    // truss number level - 1); peeling one edge decrements the support of the
    // two other edges in each triangle it closes.
    using HeapEntry = std::pair<uint64_t, uint64_t>; // (support, edgeIndex)
    std::priority_queue<HeapEntry, std::vector<HeapEntry>, std::greater<HeapEntry>> heap;
    for (uint64_t i = 0; i < m; ++i) {
        heap.emplace(edges[i].support, i);
    }

    // Peek the minimum support among the remaining (alive) edges.
    auto peekMinSupport = [&]() -> uint64_t {
        while (!heap.empty()) {
            const auto [support, idx] = heap.top();
            if (!edges[idx].alive || edges[idx].support != support) {
                heap.pop();
                continue;
            }
            return support;
        }
        return UINT64_MAX;
    };

    uint64_t remaining = m;
    uint64_t level = 3;
    while (remaining > 0) {
        // Peel every edge with support < level - 2 (cascading).
        bool peeled;
        do {
            peeled = false;
            const auto minSupport = peekMinSupport();
            if (minSupport == UINT64_MAX || minSupport >= level - 2) {
                break;
            }
            const auto [support, idx] = heap.top();
            heap.pop();
            auto& e = edges[idx];
            e.alive = false;
            e.truss = level - 1;
            remaining--;
            // Decrement the two other edges in each triangle containing e.
            const auto& nu = adj[e.u];
            const auto& nv = adj[e.v];
            size_t i = 0, j = 0;
            while (i < nu.size() && j < nv.size()) {
                if (nu[i] < nv[j]) {
                    i++;
                } else if (nv[j] < nu[i]) {
                    j++;
                } else {
                    const auto w = nu[i];
                    auto dec = [&](uint64_t a, uint64_t b) {
                        const auto key = std::min(a, b) * n + std::max(a, b);
                        auto it = edgeId.find(key);
                        if (it != edgeId.end() && edges[it->second].alive &&
                            edges[it->second].support > 0) {
                            edges[it->second].support--;
                            heap.emplace(edges[it->second].support, it->second);
                        }
                    };
                    dec(e.u, w);
                    dec(e.v, w);
                    i++;
                    j++;
                }
            }
            peeled = true;
        } while (peeled);
        // Advance to the level at which the current minimum support is peeled.
        if (remaining > 0) {
            const auto minSupport = peekMinSupport();
            if (minSupport != UINT64_MAX) {
                level = minSupport + 3;
            }
        }
    }

    for (const auto& e : edges) {
        result.edges.push_back({{e.u, e.v}, e.truss});
    }
    return result;
}

} // namespace

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<KTrussBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"K_TRUSS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t u = 0; u < adj.size(); ++u) {
        auto& nbrs = adj[u];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), u), nbrs.end());
    }
    auto truss = computeTruss(adj);

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto trussVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    trussVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), trussVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& [endpoints, trussNumber] : truss.edges) {
        if (static_cast<int64_t>(trussNumber) < bindData->k) {
            continue;
        }
        srcVector->setValue<int64_t>(0, static_cast<int64_t>(endpoints.first));
        dstVector->setValue<int64_t>(0, static_cast<int64_t>(endpoints.second));
        trussVector->setValue<int64_t>(0, static_cast<int64_t>(trussNumber));
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

function_set KTrussFunction::getFunctionSet() {
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
