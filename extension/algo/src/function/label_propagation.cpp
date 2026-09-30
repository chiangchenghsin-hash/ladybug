// LABEL_PROPAGATION - LPA community detection (undirected). P1-08 (NASH×GNN
// P1 batch): 近线性; 标签传播可解释为「策略模仿」动态; GNN 半监督伪标签。
//
// Asynchronous in-order sweeps (nodes 0..n-1 adopt the most frequent neighbor
// label each time they are visited). Ties break toward the SMALLEST label id;
// with `update_seed := false` a node keeps its current label whenever that
// label is among the tied maxima (inertia, dampens oscillation). Sweeps stop
// on convergence (no change), on a detected 2-cycle, or at max_iterations.
// Final labels are renumbered to consecutive community ids 0..C-1.
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

static constexpr char COMMUNITY_COLUMN_NAME[] = "community";

static constexpr int64_t DEFAULT_MAX_ITERATIONS = 100;
static constexpr bool DEFAULT_UPDATE_SEED = true;

struct LabelPropagationBindData final : public GDSBindData {
    int64_t maxIterations;
    bool updateSeed;

    LabelPropagationBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t maxIterations, bool updateSeed)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          maxIterations{maxIterations}, updateSeed{updateSeed} {}

    LabelPropagationBindData(const LabelPropagationBindData& other)
        : GDSBindData{other}, maxIterations{other.maxIterations}, updateSeed{other.updateSeed} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<LabelPropagationBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    int64_t maxIterations = DEFAULT_MAX_ITERATIONS;
    bool updateSeed = DEFAULT_UPDATE_SEED;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "maxiterations") {
            maxIterations = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "updateseed") {
            updateSeed = ExpressionUtil::evaluateLiteral<bool>(context, optionalParam,
                LogicalType::BOOL());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (maxIterations <= 0) {
        throw BinderException{"LABEL_PROPAGATION maxiterations must be positive."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(COMMUNITY_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<LabelPropagationBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, maxIterations, updateSeed);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

static void computeLabelPropagation(const std::vector<std::vector<uint64_t>>& adj,
    int64_t maxIterations, bool updateSeed, std::vector<uint64_t>& labels) {
    const auto n = adj.size();
    labels.resize(n);
    for (uint64_t v = 0; v < n; ++v) {
        labels[v] = v;
    }
    if (n <= 1) {
        return;
    }

    std::vector<uint64_t> previous = labels;
    std::vector<uint64_t> twoAgo = labels;
    // Label frequency scratch: only entries for the labels of the current
    // node's neighbors (and its own label) are valid.
    std::vector<uint64_t> freq(n, 0);

    for (int64_t iter = 0; iter < maxIterations; ++iter) {
        twoAgo = previous;
        previous = labels;
        bool changed = false;
        for (uint64_t v = 0; v < n; ++v) {
            if (adj[v].empty()) {
                continue;
            }
            // Count neighbor label frequencies (reset the entries we touch).
            freq[labels[v]] = 0;
            for (const auto u : adj[v]) {
                freq[labels[u]] = 0;
            }
            for (const auto u : adj[v]) {
                freq[labels[u]]++;
            }
            // Most frequent neighbor label, ties -> smallest label id.
            uint64_t bestLabel = labels[v];
            uint64_t bestCount = 0;
            for (const auto u : adj[v]) {
                const auto l = labels[u];
                const auto c = freq[l];
                if (c > bestCount || (c == bestCount && l < bestLabel)) {
                    bestCount = c;
                    bestLabel = l;
                }
            }
            if (bestLabel == labels[v]) {
                continue;
            }
            if (!updateSeed && freq[labels[v]] == bestCount) {
                continue; // inertia: keep own label when it ties for the maximum
            }
            labels[v] = bestLabel;
            changed = true;
        }
        if (!changed) {
            break; // converged
        }
        if (labels == twoAgo) {
            break; // 2-cycle oscillation: return the current state
        }
    }

    // Renumber to consecutive community ids in order of first appearance.
    std::vector<int64_t> renumber(n, -1);
    std::vector<uint64_t> out(n, 0);
    uint64_t nextId = 0;
    for (uint64_t v = 0; v < n; ++v) {
        if (renumber[labels[v]] < 0) {
            renumber[labels[v]] = static_cast<int64_t>(nextId++);
        }
        out[v] = static_cast<uint64_t>(renumber[labels[v]]);
    }
    labels = std::move(out);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<LabelPropagationBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "LABEL_PROPAGATION currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    std::vector<uint64_t> labels;
    computeLabelPropagation(adj, bindData->maxIterations, bindData->updateSeed, labels);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto communityVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    communityVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), communityVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        communityVector->setValue<int64_t>(0, static_cast<int64_t>(labels[i]));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set LabelPropagationFunction::getFunctionSet() {
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
