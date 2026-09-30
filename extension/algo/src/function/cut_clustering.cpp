// CUT_CLUSTERING - communities as equivalence classes of alpha-edge-connectivity
// (undirected, weighted). P1-09 (NASH×GNN P1 batch): 最小割聚类 = 可达成相关
// 均衡的玩家群体; GNN 割边作社区间注意力。
//
// Theory (Flake et al. 2000, "Efficient Identification of Web Communities"):
// for a parameter alpha, define u ~ v iff the edge connectivity (minimum cut)
// between u and v is >= alpha. This is an equivalence relation (a single cut
// separating u from v also separates one of them from any third node), and its
// classes form a dendrogram as alpha varies. All-pairs min-cut is computed
// with the Gusfield-simplified Gomory-Hu tree (n-1 max-flow runs, Dinic), then
// tree edges of weight < alpha are removed; the resulting connected components
// are the communities. alpha large -> singletons; alpha <= global min-cut ->
// one community. Weights default to 1.0 (missing/NULL property).
#include "binder/binder.h"
#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/max_flow.h"
#include "common/types/types.h"
#include "common/weighted_graph.h"
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
#include <queue>
#include <string>
#include <utility>
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

static constexpr double DEFAULT_ALPHA = 1.0;

struct CutClusteringBindData final : public GDSBindData {
    double alpha;
    std::string weightProperty;

    CutClusteringBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, double alpha, std::string weightProperty)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          alpha{alpha}, weightProperty{std::move(weightProperty)} {}

    CutClusteringBindData(const CutClusteringBindData& other)
        : GDSBindData{other}, alpha{other.alpha}, weightProperty{other.weightProperty} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<CutClusteringBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    double alpha = DEFAULT_ALPHA;
    std::string weightProperty = "weight";
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "alpha") {
            alpha = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "weightproperty") {
            weightProperty = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (alpha < 0.0) {
        throw BinderException{"CUT_CLUSTERING alpha must be >= 0."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(COMMUNITY_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<CutClusteringBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, alpha, weightProperty);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

static void computeCutClustering(const WeightedGraph& wg, double alpha,
    std::vector<uint64_t>& communities) {
    const auto n = wg.numNodes;
    communities.assign(n, 0);
    if (n <= 1) {
        return;
    }

    // Undirected arcs for max-flow (two antiparallel arcs per undirected edge).
    std::vector<std::pair<std::pair<uint64_t, uint64_t>, double>> arcs;
    for (uint64_t u = 0; u < n; ++u) {
        for (const auto& [v, w] : wg.und[u]) {
            if (u < v) {
                arcs.emplace_back(std::make_pair(u, v), w);
                arcs.emplace_back(std::make_pair(v, u), w);
            }
        }
    }

    // Gusfield-simplified Gomory-Hu tree.
    std::vector<uint64_t> parent(n, 0);
    std::vector<double> treeWeight(n, -1.0); // weight of edge (v, parent[v])
    for (uint64_t s = 1; s < n; ++s) {
        const auto t = parent[s];
        std::vector<uint8_t> sourceSide;
        const double f = computeMaxFlow(n, arcs, s, t, &sourceSide);
        treeWeight[s] = f;
        for (uint64_t v = 0; v < n; ++v) {
            if (v != s && parent[v] == t && sourceSide[v]) {
                parent[v] = s;
            }
        }
        if (sourceSide[parent[t]]) {
            parent[s] = parent[t];
            parent[t] = s;
            treeWeight[s] = treeWeight[t];
            treeWeight[t] = f;
        }
    }

    // Remove tree edges with weight < alpha; connected components = communities.
    std::vector<std::vector<uint64_t>> treeAdj(n);
    for (uint64_t v = 1; v < n; ++v) {
        if (treeWeight[v] < alpha) {
            continue;
        }
        const auto p = parent[v];
        treeAdj[v].push_back(p);
        treeAdj[p].push_back(v);
    }
    std::fill(communities.begin(), communities.end(), UINT64_MAX);
    uint64_t nextId = 0;
    for (uint64_t root = 0; root < n; ++root) {
        if (communities[root] != UINT64_MAX) {
            continue;
        }
        std::queue<uint64_t> q;
        communities[root] = nextId;
        q.push(root);
        while (!q.empty()) {
            const auto u = q.front();
            q.pop();
            for (const auto v : treeAdj[u]) {
                if (communities[v] == UINT64_MAX) {
                    communities[v] = nextId;
                    q.push(v);
                }
            }
        }
        nextId++;
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<CutClusteringBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"CUT_CLUSTERING currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto wg = buildWeightedGraph(graph, tableID, numNodes, bindData->weightProperty);
    std::vector<uint64_t> communities;
    computeCutClustering(wg, bindData->alpha, communities);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto communityVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    communityVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), communityVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        communityVector->setValue<int64_t>(0, static_cast<int64_t>(communities[i]));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set CutClusteringFunction::getFunctionSet() {
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
