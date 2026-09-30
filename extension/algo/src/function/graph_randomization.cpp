// GRAPH_RANDOMIZATION - degree-preserving null-model randomization of an
// existing projected graph. P3-04 (NASH×GNN P3 batch): Null Model 检验
// (显著异于随机?). 对标 NetworKit `randomization::Curveball`(默认)与
// NetworkX `algorithms.swap.double_edge_swap`(备选)。
//
//   CALL graph_randomization('g', method := 'curveball', iterations := 100,
//       seed := 42) YIELD node1, node2
//
// Undirected view (internal offsets). Two methods:
//   - 'curveball' (default): per iteration, a random node permutation trades
//     neighbourhoods of consecutive pairs, redistributing the symmetric
//     difference randomly while common neighbours stay shared. EXACT degree
//     sequence preservation. iterations = number of full rounds.
//   - 'double_edge_swap': repeatedly pick two distinct edges (u,v),(x,y) and
//     replace them by (u,x),(v,y) (or the alternate orientation), rejecting
//     self-loops / duplicates. iterations = number of successful swaps
//     (NetworkX `nswap` semantics), capped by max_tries.
// Both preserve the degree sequence and the edge count; deterministic with
// std::mt19937(seed). Output edges sorted (u < v, u asc, v asc).
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
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
#include <numeric>
#include <random>
#include <string>
#include <unordered_set>
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

static constexpr char NODE1_COLUMN_NAME[] = "node1";
static constexpr char NODE2_COLUMN_NAME[] = "node2";

static constexpr char DEFAULT_METHOD[] = "curveball";
static constexpr int64_t DEFAULT_ITERATIONS = 100;
static constexpr int64_t DEFAULT_SEED = 42;

struct GraphRandomizationBindData final : GDSBindData {
    std::string method;
    int64_t iterations;
    uint64_t seed;

    GraphRandomizationBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        std::string method, int64_t iterations, uint64_t seed)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          method{std::move(method)}, iterations{iterations}, seed{seed} {}

    GraphRandomizationBindData(const GraphRandomizationBindData& other)
        : GDSBindData{other}, method{other.method}, iterations{other.iterations},
          seed{other.seed} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GraphRandomizationBindData>(*this);
    }
};

// Curveball: iterate full rounds of trades over consecutive permutation pairs.
static void randomizeCurveball(std::vector<std::unordered_set<uint64_t>>& adj, int64_t iterations,
    uint64_t seed) {
    const auto n = adj.size();
    std::mt19937 rng(seed);
    std::vector<int64_t> perm(n);
    std::iota(perm.begin(), perm.end(), 0);

    std::vector<int64_t> aBuf, bBuf, common, onlyA, onlyB, combined;
    std::unordered_set<int64_t> bset, cset;
    for (int64_t it = 0; it < iterations; ++it) {
        std::shuffle(perm.begin(), perm.end(), rng);
        for (size_t t = 0; t + 1 < perm.size(); t += 2) {
            const auto u = perm[t];
            const auto v = perm[t + 1];
            const auto adjacent = adj[static_cast<size_t>(u)].contains(
                static_cast<uint64_t>(v));

            aBuf.clear();
            bBuf.clear();
            for (const auto x : adj[static_cast<size_t>(u)]) {
                if (x != static_cast<uint64_t>(v)) {
                    aBuf.push_back(static_cast<int64_t>(x));
                }
            }
            for (const auto x : adj[static_cast<size_t>(v)]) {
                if (x != static_cast<uint64_t>(u)) {
                    bBuf.push_back(static_cast<int64_t>(x));
                }
            }

            bset.clear();
            bset.insert(bBuf.begin(), bBuf.end());
            common.clear();
            onlyA.clear();
            cset.clear();
            for (const auto x : aBuf) {
                if (bset.contains(x)) {
                    common.push_back(x);
                    cset.insert(x);
                } else {
                    onlyA.push_back(x);
                }
            }
            onlyB.clear();
            for (const auto x : bBuf) {
                if (!cset.contains(x)) {
                    onlyB.push_back(x);
                }
            }

            combined.clear();
            combined.insert(combined.end(), onlyA.begin(), onlyA.end());
            combined.insert(combined.end(), onlyB.begin(), onlyB.end());
            std::shuffle(combined.begin(), combined.end(), rng);

            // The symmetric difference is randomly split between u and v. The
            // nodes being re-assigned must have their OWN adjacency flipped:
            // a node moved from v's exclusive side to u's side loses v and
            // gains u (and vice versa), so the global graph stays symmetric
            // and every degree (including the moved nodes') is preserved.
            std::unordered_set<int64_t> onlyASet(onlyA.begin(), onlyA.end());
            std::unordered_set<int64_t> onlyBSet(onlyB.begin(), onlyB.end());
            const auto na = onlyA.size();
            auto& adjU = adj[static_cast<size_t>(u)];
            auto& adjV = adj[static_cast<size_t>(v)];
            adjU.clear();
            adjV.clear();
            for (const auto x : common) {
                adjU.insert(static_cast<uint64_t>(x));
                adjV.insert(static_cast<uint64_t>(x));
            }
            for (size_t k = 0; k < na; ++k) {
                const auto w = combined[k];
                adjU.insert(static_cast<uint64_t>(w));
                if (onlyBSet.contains(w)) { // was v's exclusive, now u's
                    adj[static_cast<size_t>(w)].erase(static_cast<uint64_t>(v));
                    adj[static_cast<size_t>(w)].insert(static_cast<uint64_t>(u));
                }
            }
            for (size_t k = na; k < combined.size(); ++k) {
                const auto w = combined[k];
                adjV.insert(static_cast<uint64_t>(w));
                if (onlyASet.contains(w)) { // was u's exclusive, now v's
                    adj[static_cast<size_t>(w)].erase(static_cast<uint64_t>(u));
                    adj[static_cast<size_t>(w)].insert(static_cast<uint64_t>(v));
                }
            }
            if (adjacent) {
                adjU.insert(static_cast<uint64_t>(v));
                adjV.insert(static_cast<uint64_t>(u));
            }
        }
    }
}

// double-edge-swap: iterations successful swaps (NetworkX `nswap`), each with
// up to 100*iterations attempts; two edges (u,v),(x,y) with four distinct
// endpoints swap to a random orientation of the cross matching.
static void randomizeDoubleEdgeSwap(std::vector<std::unordered_set<uint64_t>>& adj,
    int64_t iterations, uint64_t seed) {
    const auto n = adj.size();
    std::mt19937 rng(seed);
    std::uniform_int_distribution<size_t> coin01(0, 1);

    struct Edge {
        uint64_t a;
        uint64_t b;
    };
    std::vector<Edge> edges;
    std::unordered_set<uint64_t> eset;
    for (uint64_t u = 0; u < n; ++u) {
        for (const auto v : adj[u]) {
            if (u < v) {
                edges.push_back({u, v});
                eset.insert(u * n + v);
            }
        }
    }
    if (edges.size() < 2) {
        return;
    }

    std::uniform_int_distribution<size_t> edgeDraw(0, edges.size() - 1);
    int64_t swapped = 0;
    const auto maxTries = 100 * iterations;
    for (int64_t tries = 0; tries < maxTries && swapped < iterations; ++tries) {
        const auto i = edgeDraw(rng);
        const auto j = edgeDraw(rng);
        if (i == j) {
            continue;
        }
        const auto [u, v] = edges[i];
        const auto [x, y] = edges[j];
        if (u == x || u == y || v == x || v == y) {
            continue;
        }
        // Random orientation of the cross matching.
        uint64_t a, b, c, d;
        if (coin01(rng) == 0) {
            a = u;
            b = x;
            c = v;
            d = y;
        } else {
            a = u;
            b = y;
            c = v;
            d = x;
        }
        if (a == b || c == d) {
            continue;
        }
        const auto keyAB = a < b ? a * n + b : b * n + a;
        const auto keyCD = c < d ? c * n + d : d * n + c;
        if (eset.contains(keyAB) || eset.contains(keyCD)) {
            continue;
        }
        eset.erase(u * n + v);
        eset.erase(x * n + y);
        eset.insert(keyAB);
        eset.insert(keyCD);
        edges[i] = {a, b};
        edges[j] = {c, d};
        ++swapped;
    }

    // Rebuild the adjacency from the final edge set.
    for (auto& s : adj) {
        s.clear();
    }
    for (const auto& e : edges) {
        adj[static_cast<size_t>(e.a)].insert(e.b);
        adj[static_cast<size_t>(e.b)].insert(e.a);
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    std::string method = DEFAULT_METHOD;
    int64_t iterations = DEFAULT_ITERATIONS;
    int64_t seed = DEFAULT_SEED;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "method") {
            method = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else if (paramName == "iterations") {
            iterations = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "seed") {
            seed = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (method != "curveball" && method != "double_edge_swap") {
        throw BinderException{
            "graph_randomization method must be 'curveball' or 'double_edge_swap'."};
    }
    if (iterations < 0) {
        throw BinderException{"graph_randomization iterations must be >= 0."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(NODE1_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(NODE2_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GraphRandomizationBindData>(std::move(columns),
        std::move(graphEntry), method, iterations, static_cast<uint64_t>(seed));
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GraphRandomizationBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"graph_randomization currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adjVec = buildUndirectedAdjacency(csr);

    std::vector<std::unordered_set<uint64_t>> adj(numNodes);
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto v : adjVec[u]) {
            if (u < v) {
                adj[u].insert(v);
                adj[v].insert(u);
            }
        }
    }

    if (bindData->method == "curveball") {
        randomizeCurveball(adj, bindData->iterations, bindData->seed);
    } else {
        randomizeDoubleEdgeSwap(adj, bindData->iterations, bindData->seed);
    }

    // Emit undirected edges sorted (u < v, u asc, v asc).
    std::vector<std::pair<uint64_t, uint64_t>> edges;
    for (uint64_t u = 0; u < numNodes; ++u) {
        for (const auto v : adj[u]) {
            if (u < v) {
                edges.emplace_back(u, v);
            }
        }
    }
    std::sort(edges.begin(), edges.end());

    auto node1Vector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    node1Vector->state = DataChunkState::getSingleValueDataChunkState();
    auto node2Vector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    node2Vector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{node1Vector.get(), node2Vector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& [u, v] : edges) {
        node1Vector->setValue<int64_t>(0, static_cast<int64_t>(u));
        node2Vector->setValue<int64_t>(0, static_cast<int64_t>(v));
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
    auto bindData = call.getBindData()->constPtrCast<GraphRandomizationBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set GraphRandomizationFunction::getFunctionSet() {
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
