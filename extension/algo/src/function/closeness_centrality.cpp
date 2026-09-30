// CLOSENESS_CENTRALITY - Freeman closeness with exact / harmonic / approx
// variants (undirected view). P1-04 (NASH×GNN P1 batch): 高紧密 = 快速传播
// 策略影响的「广播者」。`exact` follows NetworkX `closeness_centrality`
// (wf_improved scaling controlled by `normalized`); `harmonic` follows
// NetworkX `harmonic_centrality` (raw reciprocal sum, handles disconnected
// graphs); `approx` estimates distances via pivot sampling (deterministic,
// up to 100 pivots, triangle-inequality detour bound d̂(v,u) =
// min_p (d(p,v)+d(p,u))). `rank` is the 1-based competition rank by score
// descending (ties share the rank).
#include "binder/binder.h"
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
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <algorithm>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char SCORE_COLUMN_NAME[] = "score";
static constexpr char RANK_COLUMN_NAME[] = "rank";

static constexpr char VARIANT_EXACT[] = "exact";
static constexpr char VARIANT_HARMONIC[] = "harmonic";
static constexpr char VARIANT_APPROX[] = "approx";

static constexpr uint64_t APPROX_MAX_PIVOTS = 100;
// Fixed seed: sampled pivots are reproducible across runs.
static constexpr uint64_t SAMPLING_SEED = 0xC105E5E5;

struct ClosenessBindData final : public GDSBindData {
    std::string variant;
    bool normalized;

    ClosenessBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, std::string variant, bool normalized)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          variant{std::move(variant)}, normalized{normalized} {}

    ClosenessBindData(const ClosenessBindData& other)
        : GDSBindData{other}, variant{other.variant}, normalized{other.normalized} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ClosenessBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    std::string variant = VARIANT_EXACT;
    bool normalized = true;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "variant") {
            variant = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else if (paramName == "normalized") {
            normalized = ExpressionUtil::evaluateLiteral<bool>(context, optionalParam,
                LogicalType::BOOL());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (variant != VARIANT_EXACT && variant != VARIANT_HARMONIC && variant != VARIANT_APPROX) {
        throw BinderException{"CLOSENESS_CENTRALITY variant must be one of "
                              "'exact', 'harmonic', 'approx' (got '" +
                              variant + "')."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    columns.push_back(input->binder->createVariable(RANK_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<ClosenessBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, variant, normalized);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

// BFS distances from `source` over the undirected adjacency.
static void bfsDistances(const std::vector<std::vector<uint64_t>>& adj, uint64_t source,
    std::vector<int64_t>& dist, std::vector<uint64_t>& queue) {
    std::fill(dist.begin(), dist.end(), -1);
    dist[source] = 0;
    queue.clear();
    queue.push_back(source);
    for (uint64_t head = 0; head < queue.size(); ++head) {
        const auto u = queue[head];
        for (const auto v : adj[u]) {
            if (dist[v] == -1) {
                dist[v] = dist[u] + 1;
                queue.push_back(v);
            }
        }
    }
}

static void computeClosenessExact(const std::vector<std::vector<uint64_t>>& adj, bool normalized,
    std::vector<double>& scores) {
    const auto n = adj.size();
    scores.assign(n, 0.0);
    if (n <= 1) {
        if (n == 1) {
            scores[0] = 0.0; // no other nodes: NetworkX returns 0
        }
        return;
    }
    std::vector<int64_t> dist(n);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (uint64_t v = 0; v < n; ++v) {
        bfsDistances(adj, v, dist, queue);
        double totsp = 0.0;
        uint64_t reachable = 0; // including v itself (NetworkX len(sp))
        for (uint64_t u = 0; u < n; ++u) {
            if (dist[u] >= 0) {
                reachable++;
                totsp += static_cast<double>(dist[u]);
            }
        }
        double closeness = 0.0;
        if (totsp > 0.0) {
            closeness = (static_cast<double>(reachable) - 1.0) / totsp;
            if (normalized) {
                const double s = (static_cast<double>(reachable) - 1.0) /
                                 (static_cast<double>(n) - 1.0);
                closeness *= s;
            }
        }
        scores[v] = closeness;
    }
}

static void computeClosenessHarmonic(const std::vector<std::vector<uint64_t>>& adj,
    std::vector<double>& scores) {
    const auto n = adj.size();
    scores.assign(n, 0.0);
    if (n <= 1) {
        return;
    }
    std::vector<int64_t> dist(n);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (uint64_t v = 0; v < n; ++v) {
        bfsDistances(adj, v, dist, queue);
        double sum = 0.0;
        for (uint64_t u = 0; u < n; ++u) {
            if (u != v && dist[u] > 0) {
                sum += 1.0 / static_cast<double>(dist[u]);
            }
        }
        scores[v] = sum;
    }
}

static void computeClosenessApprox(const std::vector<std::vector<uint64_t>>& adj, bool normalized,
    std::vector<double>& scores) {
    const auto n = adj.size();
    scores.assign(n, 0.0);
    if (n <= 1) {
        return;
    }
    // Deterministic pivot sample.
    std::vector<uint64_t> pivots;
    if (n <= APPROX_MAX_PIVOTS) {
        pivots.resize(n);
        for (uint64_t i = 0; i < n; ++i) {
            pivots[i] = i;
        }
    } else {
        std::mt19937_64 rng{SAMPLING_SEED};
        std::vector<uint64_t> all(n);
        for (uint64_t i = 0; i < n; ++i) {
            all[i] = i;
        }
        for (uint64_t i = 0; i < APPROX_MAX_PIVOTS; ++i) {
            const auto j = i + rng() % (n - i);
            std::swap(all[i], all[j]);
        }
        pivots.assign(all.begin(), all.begin() + APPROX_MAX_PIVOTS);
    }

    // d[p][v]: distances from every pivot to every node.
    const auto numPivots = pivots.size();
    std::vector<std::vector<int64_t>> pdist(numPivots, std::vector<int64_t>(n));
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (size_t i = 0; i < numPivots; ++i) {
        bfsDistances(adj, pivots[i], pdist[i], queue);
    }

    for (uint64_t v = 0; v < n; ++v) {
        double totsp = 0.0;
        uint64_t reachable = 0; // nodes u with a finite detour bound (incl. v)
        for (uint64_t u = 0; u < n; ++u) {
            int64_t best = -1;
            for (size_t i = 0; i < numPivots; ++i) {
                const auto dv = pdist[i][v];
                const auto du = pdist[i][u];
                if (dv < 0 || du < 0) {
                    continue;
                }
                const auto detour = dv + du;
                if (best < 0 || detour < best) {
                    best = detour;
                }
            }
            if (best >= 0) {
                reachable++;
                if (u != v) {
                    totsp += static_cast<double>(best);
                }
            }
        }
        double closeness = 0.0;
        if (totsp > 0.0) {
            closeness = (static_cast<double>(reachable) - 1.0) / totsp;
            if (normalized) {
                const double s = (static_cast<double>(reachable) - 1.0) /
                                 (static_cast<double>(n) - 1.0);
                closeness *= s;
            }
        }
        scores[v] = closeness;
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<ClosenessBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "CLOSENESS_CENTRALITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);

    std::vector<double> scores;
    if (bindData->variant == VARIANT_EXACT) {
        computeClosenessExact(adj, bindData->normalized, scores);
    } else if (bindData->variant == VARIANT_HARMONIC) {
        computeClosenessHarmonic(adj, scores);
    } else {
        computeClosenessApprox(adj, bindData->normalized, scores);
    }

    // Competition ranking by score descending: 1, 2, 2, 4, ...
    std::vector<uint64_t> order(numNodes);
    for (uint64_t i = 0; i < numNodes; ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(),
        [&scores](uint64_t a, uint64_t b) { return scores[a] > scores[b]; });
    std::vector<int64_t> rank(numNodes, 0);
    for (uint64_t pos = 0; pos < order.size(); ++pos) {
        const auto v = order[pos];
        if (pos > 0 && scores[v] == scores[order[pos - 1]]) {
            rank[v] = rank[order[pos - 1]];
        } else {
            rank[v] = static_cast<int64_t>(pos + 1);
        }
    }

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    auto rankVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    rankVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), scoreVector.get(), rankVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        scoreVector->setValue<double>(0, scores[i]);
        rankVector->setValue<int64_t>(0, rank[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set ClosenessCentralityFunction::getFunctionSet() {
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
