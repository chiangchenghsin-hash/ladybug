// BETWEENNESS — Brandes (2001) exact shortest-path betweenness, undirected.
// 角色深度扩展: 桥梁/枢纽角色 = 跨 Leiden 社区的中间人 = 剧情枢纽。
// Undirected semantics: the graph is viewed as undirected (双向边算一次), per
// Brandes' original formulation. Raw accumulation over all source BFS runs counts
// each unordered pair twice; we normalize by (n-1)(n-2) so leaves score 0 and a
// star center scores 1.
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

#include <algorithm>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char SCORE_COLUMN_NAME[] = "betweenness_score";

static void computeBetweenness(const DirectedCSR& csr, std::vector<double>& scores) {
    const auto n = csr.numNodes;
    scores.assign(n, 0.0);
    if (n < 3) {
        return;
    }
    const auto adj = buildUndirectedAdjacency(csr);

    std::vector<double> sigma(n);
    std::vector<double> delta(n);
    std::vector<int64_t> dist(n);
    std::vector<std::vector<uint64_t>> pred(n);
    std::vector<uint64_t> stack;
    std::vector<uint64_t> queue;
    stack.reserve(n);
    queue.reserve(n);

    for (uint64_t s = 0; s < n; ++s) {
        std::fill(dist.begin(), dist.end(), -1);
        std::fill(sigma.begin(), sigma.end(), 0.0);
        std::fill(delta.begin(), delta.end(), 0.0);
        dist[s] = 0;
        sigma[s] = 1.0;
        queue.clear();
        queue.push_back(s);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto v = queue[head];
            stack.push_back(v);
            for (const auto w : adj[v]) {
                if (dist[w] < 0) {
                    dist[w] = dist[v] + 1;
                    queue.push_back(w);
                }
                if (dist[w] == dist[v] + 1) {
                    sigma[w] += sigma[v];
                    pred[w].push_back(v);
                }
            }
        }
        while (!stack.empty()) {
            const auto w = stack.back();
            stack.pop_back();
            for (const auto v : pred[w]) {
                delta[v] += sigma[v] / sigma[w] * (1.0 + delta[w]);
            }
            if (w != s) {
                scores[w] += delta[w];
            }
        }
        for (auto& p : pred) {
            p.clear();
        }
    }

    // Undirected Brandes counts each unordered pair twice (once per endpoint as
    // source). Divide by the number of unordered pairs not involving the node:
    // (n-1)(n-2)/2 pairs * 2 directions = (n-1)(n-2).
    const auto norm = static_cast<double>(n - 1) * static_cast<double>(n - 2);
    for (auto& score : scores) {
        score /= norm;
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
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
        throw BinderException{"BETWEENNESS currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    std::vector<double> scores;
    computeBetweenness(csr, scores);

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

function_set BetweennessFunction::getFunctionSet() {
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
