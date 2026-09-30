// APPROX_BETWEENNESS - (epsilon, delta)-approximated betweenness centrality
// via shortest-path sampling (undirected). P1-05 (NASH×GNN P1 batch): 10^5+
// 玩家时精确 Brandes 不可行, 采样近似识别「策略中介者」。
//
// Estimator: sample k node pairs (s, t) uniformly at random, pick a UNIFORMLY
// RANDOM shortest s-t path for each (backwards choice weighted by sigma), and
// count interior path nodes. The scaled hit count is an unbiased estimator of
// the NetworkX-normalized betweenness (star center = 1), matching the exact
// BETWEENNESS function. Sample count k: user-provided, or auto =
// ceil(8 * ln(2n/delta) / epsilon^2) (Hoeffding + union bound over n nodes),
// capped at 100000. Randomness is seeded deterministically for
// reproducibility. Each BFS stops as soon as the level of `t` is complete.
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

#include <cmath>
#include <cstdint>
#include <random>
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
static constexpr char IS_APPROX_COLUMN_NAME[] = "is_approx";

static constexpr double DEFAULT_EPSILON = 0.01;
static constexpr double DEFAULT_DELTA = 0.1;
static constexpr int64_t DEFAULT_K = 0; // 0 = auto
static constexpr uint64_t MAX_SAMPLES_CAP = 100000;
static constexpr uint64_t SAMPLING_SEED = 0xB37B37B3;

struct ApproxBetweennessBindData final : public GDSBindData {
    double epsilon;
    double delta;
    int64_t k;

    ApproxBetweennessBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, double epsilon, double delta, int64_t k)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          epsilon{epsilon}, delta{delta}, k{k} {}

    ApproxBetweennessBindData(const ApproxBetweennessBindData& other)
        : GDSBindData{other}, epsilon{other.epsilon}, delta{other.delta}, k{other.k} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<ApproxBetweennessBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    double epsilon = DEFAULT_EPSILON;
    double delta = DEFAULT_DELTA;
    int64_t k = DEFAULT_K;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "epsilon") {
            epsilon = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "delta") {
            delta = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "k") {
            k = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (epsilon <= 0.0 || epsilon >= 1.0) {
        throw BinderException{"APPROX_BETWEENNESS epsilon must be in (0, 1)."};
    }
    if (delta <= 0.0 || delta >= 1.0) {
        throw BinderException{"APPROX_BETWEENNESS delta must be in (0, 1)."};
    }
    if (k < 0) {
        throw BinderException{"APPROX_BETWEENNESS k must be >= 0 (0 = auto sample count)."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    columns.push_back(
        input->binder->createVariable(IS_APPROX_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<ApproxBetweennessBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, epsilon, delta, k);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

// Samples one uniformly random shortest s-t path and counts interior nodes.
// Returns false when s and t are unreachable (contributes zero).
static bool sampleOnePath(const std::vector<std::vector<uint64_t>>& adj, uint64_t s, uint64_t t,
    std::vector<int64_t>& dist, std::vector<double>& sigma,
    std::vector<std::vector<uint64_t>>& pred, std::vector<uint64_t>& queue,
    std::mt19937_64& rng, std::vector<uint64_t>& hits) {
    const auto n = adj.size();
    std::fill(dist.begin(), dist.end(), -1);
    std::fill(sigma.begin(), sigma.end(), 0.0);
    for (auto& p : pred) {
        p.clear();
    }
    dist[s] = 0;
    sigma[s] = 1.0;
    queue.clear();
    queue.push_back(s);
    bool reached = false;
    for (uint64_t head = 0; head < queue.size(); ++head) {
        const auto u = queue[head];
        if (reached && dist[u] >= dist[t]) {
            break; // level of t complete: deeper nodes cannot be predecessors
        }
        for (const auto v : adj[u]) {
            if (dist[v] == -1) {
                dist[v] = dist[u] + 1;
                sigma[v] = sigma[u];
                pred[v].push_back(u);
                queue.push_back(v);
                if (v == t) {
                    reached = true;
                }
            } else if (dist[v] == dist[u] + 1) {
                sigma[v] += sigma[u];
                pred[v].push_back(u);
            }
        }
    }
    if (!reached) {
        return false;
    }
    // Walk backwards from t to s, choosing a predecessor with probability
    // sigma[pred] / sigma[u] (uniform over shortest paths). Only interior
    // nodes (strictly between s and t) are credited.
    uint64_t cur = t;
    while (cur != s) {
        const auto& ps = pred[cur];
        double total = sigma[cur];
        double r = std::uniform_real_distribution<double>(0.0, total)(rng);
        uint64_t chosen = ps[0];
        double acc = 0.0;
        for (const auto p : ps) {
            acc += sigma[p];
            if (r <= acc) {
                chosen = p;
                break;
            }
        }
        cur = chosen;
        if (cur != s) {
            hits[cur]++;
        }
    }
    return true;
}

static void computeApproxBetweenness(const std::vector<std::vector<uint64_t>>& adj, double epsilon,
    double delta, int64_t k, std::vector<double>& scores) {
    const auto n = adj.size();
    scores.assign(n, 0.0);
    if (n < 3) {
        return; // no interior nodes possible
    }
    const uint64_t numPairs = n * (n - 1) / 2;
    if (numPairs == 0) {
        return;
    }

    uint64_t samples = static_cast<uint64_t>(k);
    if (samples == 0) {
        const double logTerm = std::log(2.0 * static_cast<double>(n) / delta);
        samples = static_cast<uint64_t>(std::ceil(8.0 * logTerm / (epsilon * epsilon)));
        samples = std::min<uint64_t>(samples, MAX_SAMPLES_CAP);
    }
    if (samples == 0) {
        samples = 1;
    }

    std::mt19937_64 rng{SAMPLING_SEED};
    std::uniform_int_distribution<uint64_t> nodePicker(0, n - 1);

    std::vector<int64_t> dist(n);
    std::vector<double> sigma(n);
    std::vector<std::vector<uint64_t>> pred(n);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    std::vector<uint64_t> hits(n, 0);
    uint64_t validSamples = 0;
    for (uint64_t i = 0; i < samples; ++i) {
        uint64_t s = nodePicker(rng);
        uint64_t t = nodePicker(rng);
        if (s == t) {
            continue;
        }
        if (sampleOnePath(adj, s, t, dist, sigma, pred, queue, rng, hits)) {
            validSamples++;
        }
    }
    if (validSamples == 0) {
        return;
    }
    // Scale: a uniformly random unordered pair contributes 1/numPairs of the
    // total; NetworkX normalization for undirected graphs divides the doubled
    // raw score by (n-1)(n-2), i.e. multiplies the pair mean by 2n(n-1)/((n-1)(n-2)).
    const double scale = 2.0 * static_cast<double>(numPairs) /
                         (static_cast<double>(n - 1) * static_cast<double>(n - 2));
    for (uint64_t v = 0; v < n; ++v) {
        scores[v] = scale * static_cast<double>(hits[v]) /
                    static_cast<double>(validSamples);
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<ApproxBetweennessBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "APPROX_BETWEENNESS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    std::vector<double> scores;
    computeApproxBetweenness(adj, bindData->epsilon, bindData->delta, bindData->k, scores);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    auto isApproxVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    isApproxVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), scoreVector.get(),
        isApproxVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        scoreVector->setValue<double>(0, scores[i]);
        isApproxVector->setValue<bool>(0, true);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set ApproxBetweennessFunction::getFunctionSet() {
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
