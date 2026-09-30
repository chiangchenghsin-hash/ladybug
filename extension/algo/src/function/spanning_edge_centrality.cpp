// SPANNING_EDGE_CENTRALITY - probability that an edge belongs to a uniformly
// random spanning tree (undirected, weighted). P1-06 (NASH×GNN P1 batch):
// 移除后最多生成树断裂的边 = 不可缺失互动关系; GNN 边级特征。
//
// SEC(e) = P(e in UST) = w_e * R_eff(e) (Kirchhoff). Computed by Monte-Carlo
// sampling of spanning trees with Wilson's loop-erased random walk (weighted
// transitions), k = ceil(ln(2m/0.1) / (2*epsilon^2)) samples (delta fixed at
// 0.1, capped at 5000), deterministic seed. Bridges always score exactly 1.
// Weight property defaults to "weight"; missing property / NULL -> 1.0.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/weighted_graph.h"
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

#include <cmath>
#include <cstdint>
#include <random>
#include <unordered_map>
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char SCORE_COLUMN_NAME[] = "score";

static constexpr double DEFAULT_EPSILON = 0.1;
static constexpr double IMPLICIT_DELTA = 0.1;
static constexpr uint64_t MAX_SAMPLES_CAP = 5000;
static constexpr uint64_t SAMPLING_SEED = 0x5711507;

struct SpanningEdgeBindData final : public GDSBindData {
    double epsilon;
    std::string weightProperty;

    SpanningEdgeBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        double epsilon, std::string weightProperty)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          epsilon{epsilon}, weightProperty{std::move(weightProperty)} {}

    SpanningEdgeBindData(const SpanningEdgeBindData& other)
        : GDSBindData{other}, epsilon{other.epsilon}, weightProperty{other.weightProperty} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SpanningEdgeBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    double epsilon = DEFAULT_EPSILON;
    std::string weightProperty = "weight";
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "epsilon") {
            epsilon = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "weightproperty") {
            weightProperty = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (epsilon <= 0.0 || epsilon >= 1.0) {
        throw BinderException{"SPANNING_EDGE_CENTRALITY epsilon must be in (0, 1)."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<SpanningEdgeBindData>(std::move(columns), std::move(graphEntry),
        epsilon, weightProperty);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

// Wilson's algorithm on the weighted undirected view: transition u -> v with
// probability w(u,v) / weighted_degree(u). Tree edges are counted into
// `edgeHits` keyed by min(u,v)*n + max(u,v).
static void sampleOneTree(const std::vector<std::vector<std::pair<uint64_t, double>>>& und,
    std::mt19937_64& rng, std::vector<uint8_t>& inTree, std::vector<uint64_t>& walkStack,
    std::vector<int64_t>& walkPos, std::vector<double>& weightedDegree,
    std::unordered_map<uint64_t, uint64_t>& edgeHits) {
    const auto n = und.size();
    if (n == 0) {
        return;
    }
    std::fill(inTree.begin(), inTree.end(), 0);
    inTree[0] = 1; // arbitrary root
    for (uint64_t s = 1; s < n; ++s) {
        if (inTree[s]) {
            continue;
        }
        // Loop-erased random walk from s until an in-tree node is reached.
        walkStack.clear();
        walkStack.push_back(s);
        walkPos[s] = 0;
        uint64_t cur = s;
        while (!inTree[cur]) {
            const auto& nbrs = und[cur];
            if (nbrs.empty()) {
                break; // isolated node: no walk possible
            }
            double total = weightedDegree[cur];
            double r = std::uniform_real_distribution<double>(0.0, total)(rng);
            double acc = 0.0;
            uint64_t next = nbrs[0].first;
            for (const auto& [v, w] : nbrs) {
                acc += w;
                if (r <= acc) {
                    next = v;
                    break;
                }
            }
            if (walkPos[next] >= 0) {
                // Loop detected: erase the cycle by truncating the walk.
                const auto keep = static_cast<size_t>(walkPos[next]) + 1;
                for (size_t i = keep; i < walkStack.size(); ++i) {
                    walkPos[walkStack[i]] = -1;
                }
                walkStack.resize(keep);
            } else {
                walkPos[next] = static_cast<int64_t>(walkStack.size());
                walkStack.push_back(next);
            }
            cur = next;
        }
        // Splice the walked path into the tree.
        for (size_t i = 0; i + 1 < walkStack.size(); ++i) {
            const auto u = walkStack[i];
            const auto v = walkStack[i + 1];
            edgeHits[std::min(u, v) * n + std::max(u, v)]++;
            inTree[u] = 1;
        }
        if (!walkStack.empty()) {
            inTree[walkStack.back()] = 1;
        }
        for (const auto u : walkStack) {
            walkPos[u] = -1;
        }
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<SpanningEdgeBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "SPANNING_EDGE_CENTRALITY currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto wg = buildWeightedGraph(graph, tableID, numNodes, bindData->weightProperty);
    const auto m = wg.numUndEdges();

    std::vector<ValueVector*> vectors;
    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    vectors = {srcVector.get(), dstVector.get(), scoreVector.get()};

    if (m > 0 && numNodes > 0) {
        uint64_t samples = static_cast<uint64_t>(std::ceil(
            std::log(2.0 * static_cast<double>(m) / IMPLICIT_DELTA) /
            (2.0 * bindData->epsilon * bindData->epsilon)));
        samples = std::min<uint64_t>(std::max<uint64_t>(samples, 1), MAX_SAMPLES_CAP);

        std::vector<double> weightedDegree(numNodes, 0.0);
        for (uint64_t u = 0; u < numNodes; ++u) {
            for (const auto& [v, w] : wg.und[u]) {
                weightedDegree[u] += w;
            }
        }
        std::mt19937_64 rng{SAMPLING_SEED};
        std::vector<uint8_t> inTree(numNodes, 0);
        std::vector<uint64_t> walkStack;
        walkStack.reserve(numNodes);
        std::vector<int64_t> walkPos(numNodes, -1);
        std::unordered_map<uint64_t, uint64_t> edgeHits;
        for (uint64_t i = 0; i < samples; ++i) {
            sampleOneTree(wg.und, rng, inTree, walkStack, walkPos, weightedDegree, edgeHits);
        }

        for (uint64_t u = 0; u < numNodes; ++u) {
            for (const auto& [v, w] : wg.und[u]) {
                if (v < u) {
                    continue; // report each undirected edge once (u < v)
                }
                const auto key = u * numNodes + v;
                const auto it = edgeHits.find(key);
                const double score = it == edgeHits.end()
                                         ? 0.0
                                         : static_cast<double>(it->second) /
                                               static_cast<double>(samples);
                srcVector->setValue<int64_t>(0, static_cast<int64_t>(u));
                dstVector->setValue<int64_t>(0, static_cast<int64_t>(v));
                scoreVector->setValue<double>(0, score);
                localFT->append(vectors);
            }
        }
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar/edge output (no node column): bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set SpanningEdgeCentralityFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
