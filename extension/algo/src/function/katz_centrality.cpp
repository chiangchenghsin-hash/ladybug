// KATZ_CENTRALITY — Katz (1953) decaying path sum, power iteration.
// 角色深度扩展: 与 PageRank 互补 — 不直接相连但长程影响的角色(幕后黑手)。
// x_v = beta + alpha * sum_{w: v->w} x_w, i.e. influence decays along directed
// edges from the source (directed chain 1->2->3 scores monotonically decreasing).
// Iterates until max |dx| < 1e-6 or maxIterations; non-convergence (alpha too
// close to 1/lambda_max) raises a BinderException — never silently truncated.
#include "binder/binder.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/config/katz_centrality_config.h"
#include "function/config/max_iterations_config.h"
#include "function/gds/gds.h"
#include "function/gds/gds_utils.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <cmath>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char SCORE_COLUMN_NAME[] = "katz_score";
static constexpr double KATZ_TOLERANCE = 1e-6;
static constexpr double KATZ_DIVERGENCE_BOUND = 1e12;

struct KatzOptionalParams final : public MaxIterationOptionalParams {
    function::OptionalParam<KatzAlpha> alpha;
    function::OptionalParam<KatzBeta> beta;

    explicit KatzOptionalParams(const expression_vector& optionalParams);

    // For copy only
    KatzOptionalParams(function::OptionalParam<MaxIterations> maxIterations,
        function::OptionalParam<KatzAlpha> alpha, function::OptionalParam<KatzBeta> beta)
        : MaxIterationOptionalParams{std::move(maxIterations)}, alpha{std::move(alpha)},
          beta{std::move(beta)} {}

    void evaluateParams(main::ClientContext* context) override {
        MaxIterationOptionalParams::evaluateParams(context);
        alpha.evaluateParam(context);
        beta.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        return std::make_unique<KatzOptionalParams>(maxIterations, alpha, beta);
    }
};

KatzOptionalParams::KatzOptionalParams(const expression_vector& optionalParams)
    : MaxIterationOptionalParams{constructMaxIterationParam(optionalParams)} {
    for (auto& optionalParam : optionalParams) {
        auto paramName = StringUtils::getLower(optionalParam->getAlias());
        if (paramName == MaxIterations::NAME) {
            continue;
        } else if (paramName == KatzAlpha::NAME) {
            alpha = function::OptionalParam<KatzAlpha>(optionalParam);
        } else if (paramName == KatzBeta::NAME) {
            beta = function::OptionalParam<KatzBeta>(optionalParam);
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
}

static void computeKatz(const DirectedCSR& csr, double alpha, double beta,
    int64_t maxIterations, std::vector<double>& scores) {
    const auto n = csr.numNodes;
    scores.assign(n, 0.0);
    if (n == 0) {
        return;
    }
    std::vector<double> next(n, beta);
    for (int64_t iter = 0; iter < maxIterations; ++iter) {
        double maxDiff = 0.0;
        for (uint64_t v = 0; v < n; ++v) {
            double sum = 0.0;
            for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[v]);
                 it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[v + 1]);
                 ++it) {
                sum += scores[*it];
            }
            next[v] = beta + alpha * sum;
            maxDiff = std::max(maxDiff, std::fabs(next[v] - scores[v]));
        }
        scores.swap(next);
        if (maxDiff < KATZ_TOLERANCE) {
            return;
        }
        if (maxDiff > KATZ_DIVERGENCE_BOUND) {
            throw BinderException{
                "Katz centrality failed to converge: alpha must be smaller than 1/lambda_max."};
        }
    }
    throw BinderException{
        "Katz centrality failed to converge within " + std::to_string(maxIterations) +
        " iterations: alpha must be smaller than 1/lambda_max."};
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    auto bindData = std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput});
    bindData->optionalParams = std::make_unique<KatzOptionalParams>(input->optionalParamsLegacy);
    return bindData;
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
            "KATZ_CENTRALITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto& katzParams =
        input.bindData->optionalParams->constCast<KatzOptionalParams>();
    const auto alpha = katzParams.alpha.getParamVal();
    const auto beta = katzParams.beta.getParamVal();
    const auto maxIterations = katzParams.maxIterations.getParamVal();

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<double> scores;
    computeKatz(csr, alpha, beta, maxIterations, scores);

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

function_set KatzCentralityFunction::getFunctionSet() {
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
