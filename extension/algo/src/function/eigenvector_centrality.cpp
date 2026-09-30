// EIGENVECTOR_CENTRALITY - power iteration for the dominant eigenvector of the
// (undirected) adjacency matrix. P1-03 (NASH×GNN P1 batch): Bonacich 中心性
// 直接决定网络博弈 Nash 均衡活动水平 (Ballester et al. 2006); GNN 谱特征代理。
//
// Follows NetworkX `eigenvector_centrality`: (A + I) iteration from the
// all-ones vector, L2 normalization per step, convergence when the L1 change
// drops below n * tolerance; non-convergence raises an error (never silently
// truncated). Deviation from NetworkX: the final vector is normalized by its
// MAXIMUM entry instead of the L2 norm, so a star's center scores exactly 1
// (spec known-answer); this differs only by a global scale factor.
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

static constexpr double DEFAULT_TOLERANCE = 1e-9;
static constexpr int64_t DEFAULT_MAX_ITERATIONS = 1000;
static constexpr double DIVERGENCE_BOUND = 1e12;

struct EigenvectorCentralityBindData final : public GDSBindData {
    double tolerance;
    int64_t maxIterations;

    EigenvectorCentralityBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, double tolerance, int64_t maxIterations)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          tolerance{tolerance}, maxIterations{maxIterations} {}

    EigenvectorCentralityBindData(const EigenvectorCentralityBindData& other)
        : GDSBindData{other}, tolerance{other.tolerance},
          maxIterations{other.maxIterations} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<EigenvectorCentralityBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    double tolerance = DEFAULT_TOLERANCE;
    int64_t maxIterations = DEFAULT_MAX_ITERATIONS;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "tolerance" || paramName == "tol") {
            tolerance = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "maxiterations") {
            maxIterations = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (tolerance <= 0.0) {
        throw BinderException{"EIGENVECTOR_CENTRALITY tolerance must be positive."};
    }
    if (maxIterations <= 0) {
        throw BinderException{"EIGENVECTOR_CENTRALITY maxiterations must be positive."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<EigenvectorCentralityBindData>(std::move(columns),
        std::move(graphEntry), expression_vector{nodeOutput}, tolerance, maxIterations);
}

static void computeEigenvectorCentrality(const std::vector<std::vector<uint64_t>>& adj,
    double tolerance, int64_t maxIterations, std::vector<double>& scores) {
    const auto n = adj.size();
    scores.assign(n, 0.0);
    if (n == 0) {
        return;
    }
    if (n == 1) {
        scores[0] = 1.0;
        return;
    }

    // All-ones start normalized by the sum (NetworkX default nstart).
    std::vector<double> x(n, 1.0 / static_cast<double>(n));
    std::vector<double> next(n);
    bool converged = false;
    for (int64_t iter = 0; iter < maxIterations; ++iter) {
        // next = x + A * x   (the (A + I) iteration)
        for (uint64_t v = 0; v < n; ++v) {
            double sum = x[v];
            for (const auto u : adj[v]) {
                sum += x[u];
            }
            next[v] = sum;
        }
        double norm = 0.0;
        for (const auto val : next) {
            norm += val * val;
        }
        norm = std::sqrt(norm);
        if (norm == 0.0 || !std::isfinite(norm)) {
            throw BinderException{
                "EIGENVECTOR_CENTRALITY diverged (non-finite vector); check the graph."};
        }
        double l1Diff = 0.0;
        for (uint64_t v = 0; v < n; ++v) {
            next[v] /= norm;
            l1Diff += std::fabs(next[v] - x[v]);
        }
        x.swap(next);
        if (l1Diff < static_cast<double>(n) * tolerance) {
            converged = true;
            break;
        }
        if (l1Diff > DIVERGENCE_BOUND) {
            throw BinderException{"EIGENVECTOR_CENTRALITY diverged (norm exceeded " +
                                  std::to_string(DIVERGENCE_BOUND) + ")."};
        }
    }
    if (!converged) {
        throw BinderException{"EIGENVECTOR_CENTRALITY failed to converge within " +
                              std::to_string(maxIterations) +
                              " iterations; raise maxiterations or tolerance."};
    }
    // Converged: rescale by the max entry so a star's center is exactly 1.
    double maxVal = 0.0;
    for (const auto val : x) {
        maxVal = std::max(maxVal, val);
    }
    if (maxVal <= 0.0) {
        throw BinderException{"EIGENVECTOR_CENTRALITY produced a degenerate vector."};
    }
    for (auto& val : x) {
        val /= maxVal;
    }
    scores = std::move(x);
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
    auto bindData = input.bindData->constPtrCast<EigenvectorCentralityBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "EIGENVECTOR_CENTRALITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    std::vector<double> scores;
    computeEigenvectorCentrality(adj, bindData->tolerance, bindData->maxIterations, scores);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), scoreVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        scoreVector->setValue<double>(0, scores[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set EigenvectorCentralityFunction::getFunctionSet() {
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
