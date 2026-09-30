// HITS - Hyperlink-Induced Topic Search (hub/authority) on a directed graph.
// P2-14 (NASH×GNN P2 batch): 有向枢纽/权威; 有向星(中心->叶) 中心 hub 高、
// 叶 authority 高。对标 NetworkX `hits`.
//
//   CALL hits('D', max_iter := 100, tol := 1e-8) YIELD node, hub, authority
//
// Power iteration over the authority/hub pair, L1-normalized each step:
//   authority(u) = sum of hub(v)   over in-neighbors v (bwd[u])
//   hub(u)       = sum of auth(v)  over out-neighbors v (fwd[u])
// Converges when both L1 deltas drop below n * tol; non-convergence raises an
// error (never silently truncated). Empty graph -> no rows.
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

static constexpr char HUB_COLUMN_NAME[] = "hub";
static constexpr char AUTHORITY_COLUMN_NAME[] = "authority";

static constexpr int64_t DEFAULT_MAX_ITERATIONS = 100;
static constexpr double DEFAULT_TOLERANCE = 1e-8;
static constexpr double DIVERGENCE_BOUND = 1e12;

struct HitsBindData final : public GDSBindData {
    int64_t maxIterations;
    double tolerance;

    HitsBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t maxIterations, double tolerance)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          maxIterations{maxIterations}, tolerance{tolerance} {}

    HitsBindData(const HitsBindData& other)
        : GDSBindData{other}, maxIterations{other.maxIterations}, tolerance{other.tolerance} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<HitsBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    int64_t maxIterations = DEFAULT_MAX_ITERATIONS;
    double tolerance = DEFAULT_TOLERANCE;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "maxiterations" || paramName == "maxiter") {
            maxIterations = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "tolerance" || paramName == "tol") {
            tolerance = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (maxIterations <= 0) {
        throw BinderException{"HITS max_iter must be positive."};
    }
    if (tolerance <= 0.0) {
        throw BinderException{"HITS tolerance must be positive."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(HUB_COLUMN_NAME, LogicalType::DOUBLE()));
    columns.push_back(input->binder->createVariable(AUTHORITY_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<HitsBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, maxIterations, tolerance);
}

// Computes hub/authority over the directed CSR (fwd = out, bwd = in).
void computeHits(const DirectedCSR& csr, int64_t maxIterations, double tolerance,
    std::vector<double>& hub, std::vector<double>& authority) {
    const auto n = csr.numNodes;
    hub.assign(n, 1.0);
    authority.assign(n, 1.0);
    if (n == 0) {
        return;
    }

    std::vector<double> nextHub(n), nextAuth(n);
    bool converged = false;
    for (int64_t iter = 0; iter < maxIterations; ++iter) {
        for (uint64_t u = 0; u < n; ++u) {
            double a = 0.0, h = 0.0;
            for (uint64_t k = csr.bwdOffsets[u]; k < csr.bwdOffsets[u + 1]; ++k) {
                a += hub[csr.bwdNeighbors[k]];
            }
            for (uint64_t k = csr.fwdOffsets[u]; k < csr.fwdOffsets[u + 1]; ++k) {
                h += authority[csr.fwdNeighbors[k]];
            }
            nextAuth[u] = a;
            nextHub[u] = h;
        }
        double normA = 0.0, normH = 0.0;
        for (uint64_t u = 0; u < n; ++u) {
            normA += nextAuth[u];
            normH += nextHub[u];
        }
        if (normA <= 0.0 || normH <= 0.0 || !std::isfinite(normA) || !std::isfinite(normH)) {
            throw BinderException{
                "HITS diverged (zero or non-finite norm); check the graph."};
        }
        double l1A = 0.0, l1H = 0.0;
        for (uint64_t u = 0; u < n; ++u) {
            nextAuth[u] /= normA;
            nextHub[u] /= normH;
            l1A += std::fabs(nextAuth[u] - authority[u]);
            l1H += std::fabs(nextHub[u] - hub[u]);
        }
        authority.swap(nextAuth);
        hub.swap(nextHub);
        if (l1A < static_cast<double>(n) * tolerance &&
            l1H < static_cast<double>(n) * tolerance) {
            converged = true;
            break;
        }
        if (l1A > DIVERGENCE_BOUND || l1H > DIVERGENCE_BOUND) {
            throw BinderException{
                "HITS diverged (norm exceeded " + std::to_string(DIVERGENCE_BOUND) + ")."};
        }
    }
    if (!converged) {
        throw BinderException{"HITS failed to converge within " +
                              std::to_string(maxIterations) +
                              " iterations; raise max_iter or tolerance."};
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<HitsBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"HITS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<double> hub, authority;
    computeHits(csr, bindData->maxIterations, bindData->tolerance, hub, authority);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto hubVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    hubVector->state = DataChunkState::getSingleValueDataChunkState();
    auto authVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    authVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), hubVector.get(), authVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        hubVector->setValue<double>(0, hub[i]);
        authVector->setValue<double>(0, authority[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set HitsFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
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
