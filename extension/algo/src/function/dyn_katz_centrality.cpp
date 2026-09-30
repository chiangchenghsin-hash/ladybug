// DYN_KATZ_CENTRALITY - Katz centrality after one hypothetical edge insertion,
// with a top-k membership change flag. P2-02 (NASH×GNN P2 batch): 动态 Top-k
// 关键玩家跟踪。对标 NetworKit `centrality::DynKatzCentrality`.
//
//   CALL dyn_katz_centrality('g', u, v, k := 10)
//     YIELD node, score, rank_changed
//
// Stateless: computes Katz on the projection, then on the projection plus the
// undirected edge (u, v) (offsets). `score` is the post-insertion Katz value
// (power iteration x = beta + alpha * sum over neighbors, alpha=0.05, beta=1,
// tol 1e-6); `rank_changed` is true iff the node is in the top-k set (by score,
// ties to smaller offset) in exactly one of the two states.
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
#include <cmath>
#include <cstdint>
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
static constexpr char RANK_CHANGED_COLUMN_NAME[] = "rank_changed";

static constexpr int64_t DEFAULT_K = 10;
static constexpr double KATZ_ALPHA = 0.05;
static constexpr double KATZ_BETA = 1.0;
static constexpr double KATZ_TOLERANCE = 1e-6;
static constexpr double KATZ_DIVERGENCE_BOUND = 1e12;

struct DynKatzCentralityBindData final : public GDSBindData {
    int64_t u;
    int64_t v;
    int64_t k;

    DynKatzCentralityBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t u, int64_t v, int64_t k)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          u{u}, v{v}, k{k} {}

    DynKatzCentralityBindData(const DynKatzCentralityBindData& other)
        : GDSBindData{other}, u{other.u}, v{other.v}, k{other.k} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<DynKatzCentralityBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    const auto u = input->getLiteralVal<int64_t>(1);
    const auto v = input->getLiteralVal<int64_t>(2);
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
    if (k <= 0) {
        throw BinderException{"DYN_KATZ_CENTRALITY k must be >= 1."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    columns.push_back(input->binder->createVariable(RANK_CHANGED_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<DynKatzCentralityBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, u, v, k);
}

void katzScores(const std::vector<std::vector<uint64_t>>& adj, std::vector<double>& scores) {
    const auto n = adj.size();
    scores.assign(n, 0.0);
    if (n == 0) {
        return;
    }
    std::vector<double> next(n, KATZ_BETA);
    for (int64_t iter = 0; iter < 100; ++iter) {
        double maxDiff = 0.0;
        for (uint64_t w = 0; w < n; ++w) {
            double sum = 0.0;
            for (const auto t : adj[w]) {
                sum += scores[t];
            }
            next[w] = KATZ_BETA + KATZ_ALPHA * sum;
            maxDiff = std::max(maxDiff, std::fabs(next[w] - scores[w]));
        }
        scores.swap(next);
        if (maxDiff < KATZ_TOLERANCE) {
            return;
        }
        if (maxDiff > KATZ_DIVERGENCE_BOUND) {
            throw BinderException{"DYN_KATZ_CENTRALITY diverged (alpha too close to 1/lambda_max)."};
        }
    }
    throw BinderException{
        "DYN_KATZ_CENTRALITY failed to converge within 100 iterations (alpha too large)."};
}

// Top-k set of node offsets (score desc, ties to smaller offset).
std::vector<uint8_t> topKSet(const std::vector<double>& scores, int64_t k) {
    const auto n = scores.size();
    std::vector<uint64_t> order(n);
    for (uint64_t v = 0; v < n; ++v) {
        order[v] = v;
    }
    std::sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) {
        if (scores[a] != scores[b]) {
            return scores[a] > scores[b];
        }
        return a < b;
    });
    std::vector<uint8_t> inSet(n, 0);
    const auto cnt = std::min<uint64_t>(static_cast<uint64_t>(k), n);
    for (uint64_t i = 0; i < cnt; ++i) {
        inSet[order[i]] = 1;
    }
    return inSet;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<DynKatzCentralityBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "DYN_KATZ_CENTRALITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;
    const auto u = static_cast<uint64_t>(bindData->u);
    const auto v = static_cast<uint64_t>(bindData->v);
    if (u >= numNodes || v >= numNodes) {
        throw BinderException{"DYN_KATZ_CENTRALITY endpoint out of range."};
    }

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t w = 0; w < adj.size(); ++w) {
        auto& nbrs = adj[w];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), w), nbrs.end());
    }

    std::vector<double> score1;
    katzScores(adj, score1);
    const auto top1 = topKSet(score1, bindData->k);
    if (u != v) {
        adj[u].push_back(v);
        adj[v].push_back(u);
    }
    std::vector<double> score2;
    katzScores(adj, score2);
    const auto top2 = topKSet(score2, bindData->k);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    auto changedVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    changedVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), scoreVector.get(), changedVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        scoreVector->setValue<double>(0, score2[i]);
        changedVector->setValue<bool>(0, top1[i] != top2[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set DynKatzCentralityFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64, LogicalTypeID::INT64});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
