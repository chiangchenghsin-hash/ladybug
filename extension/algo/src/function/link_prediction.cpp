// LINK_PREDICTION - link-prediction scores over the undirected view.
// P1-14 (NASH×GNN P1 batch): 链接预测 = 潜在策略互动; GNN 自监督预训练任务。
// Metrics follow NetworkX `link_prediction` module exactly:
//   common_neighbors       |N(u) ∩ N(v)|
//   adamic_adar            Σ_{w∈∩} 1/ln(deg w)        (deg<=1 -> contribution 0)
//   resource_allocation    Σ_{w∈∩} 1/deg w
//   preferential_attachment deg(u) * deg(v)
//   jaccard                |∩| / |∪|                 (∪ empty -> 0)
//   katz_index             Σ_{l>=1} alpha^l * (A^l)[u,v]   (walks; alpha in (0,1))
//
// Query shapes (all params are optional named):
//   CALL link_prediction('g', source := 0, target := 2)              -> 1 row
//   CALL link_prediction('g', source := 0, target := 2, metric := 'all') -> 7 rows
//   CALL link_prediction('g', pivot_node := 0)                       -> n-1 rows
// Exactly one of (source AND target) or pivot_node must be given; metric='all'
// requires an explicit source+target pair.
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
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
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
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
static constexpr char METRIC_COLUMN_NAME[] = "metric_name";
static constexpr char SCORE_COLUMN_NAME[] = "score";

static constexpr double DEFAULT_KATZ_ALPHA = 0.05;

struct LinkPredictionBindData final : public GDSBindData {
    bool hasPair;        // explicit source+target
    uint64_t source;
    uint64_t target;
    bool hasPivot;
    uint64_t pivot;
    std::string metric;
    double alpha;

    LinkPredictionBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        bool hasPair, uint64_t source, uint64_t target, bool hasPivot, uint64_t pivot,
        std::string metric, double alpha)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          hasPair{hasPair}, source{source}, target{target}, hasPivot{hasPivot}, pivot{pivot},
          metric{std::move(metric)}, alpha{alpha} {}

    LinkPredictionBindData(const LinkPredictionBindData& other)
        : GDSBindData{other}, hasPair{other.hasPair}, source{other.source}, target{other.target},
          hasPivot{other.hasPivot}, pivot{other.pivot}, metric{other.metric}, alpha{other.alpha} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<LinkPredictionBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    bool hasSource = false, hasTarget = false, hasPivot = false;
    uint64_t source = 0, target = 0, pivot = 0;
    std::string metric = "adamic_adar";
    double alpha = DEFAULT_KATZ_ALPHA;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "source") {
            source = static_cast<uint64_t>(ExpressionUtil::evaluateLiteral<int64_t>(context,
                optionalParam, LogicalType::INT64()));
            hasSource = true;
        } else if (paramName == "target") {
            target = static_cast<uint64_t>(ExpressionUtil::evaluateLiteral<int64_t>(context,
                optionalParam, LogicalType::INT64()));
            hasTarget = true;
        } else if (paramName == "pivotnode") {
            pivot = static_cast<uint64_t>(ExpressionUtil::evaluateLiteral<int64_t>(context,
                optionalParam, LogicalType::INT64()));
            hasPivot = true;
        } else if (paramName == "metric") {
            metric = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else if (paramName == "alpha") {
            alpha = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    const bool hasPair = hasSource && hasTarget;
    if (hasSource != hasTarget) {
        throw BinderException{"LINK_PREDICTION requires both source AND target."};
    }
    if (hasPair && hasPivot) {
        throw BinderException{
            "LINK_PREDICTION accepts either (source, target) or pivot_node, not both."};
    }
    if (!hasPair && !hasPivot) {
        throw BinderException{"LINK_PREDICTION requires (source, target) or pivot_node."};
    }
    if (metric == "all" && !hasPair) {
        throw BinderException{"LINK_PREDICTION metric='all' requires an explicit source+target."};
    }
    if (alpha <= 0.0 || alpha >= 1.0) {
        throw BinderException{"LINK_PREDICTION katz alpha must be in (0, 1)."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(METRIC_COLUMN_NAME, LogicalType::STRING()));
    columns.push_back(input->binder->createVariable(SCORE_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<LinkPredictionBindData>(std::move(columns), std::move(graphEntry),
        hasPair, source, target, hasPivot, pivot, metric, alpha);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

// Undirected adjacency without self-loops.
static std::vector<std::vector<uint64_t>> buildSimpleAdjacency(const DirectedCSR& csr) {
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t u = 0; u < adj.size(); ++u) {
        auto& nbrs = adj[u];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), u), nbrs.end());
    }
    return adj;
}

// |N(u) ∩ N(v)| via two-pointer over the sorted lists.
static uint64_t commonNeighbors(const std::vector<uint64_t>& a, const std::vector<uint64_t>& b) {
    size_t i = 0, j = 0;
    uint64_t count = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] < b[j]) {
            i++;
        } else if (b[j] < a[i]) {
            j++;
        } else {
            count++;
            i++;
            j++;
        }
    }
    return count;
}

// Katz index: K(u,v) = sum_{l>=1} alpha^l * (# walks of length l from u to v).
// Computed by bounded vector propagation from a unit vector at u.
static double katzIndex(const std::vector<std::vector<uint64_t>>& adj, uint64_t u, uint64_t v,
    double alpha) {
    const auto n = adj.size();
    std::vector<double> x(n, 0.0), next(n, 0.0);
    x[u] = 1.0;
    double result = 0.0;
    double power = alpha;
    for (int l = 1; l <= 100; ++l) {
        std::fill(next.begin(), next.end(), 0.0);
        for (uint64_t i = 0; i < n; ++i) {
            if (x[i] == 0.0) {
                continue;
            }
            for (const auto w : adj[i]) {
                next[w] += x[i];
            }
        }
        const double term = power * next[v];
        result += term;
        x.swap(next);
        power *= alpha;
        if (std::fabs(power) < 1e-16) {
            break;
        }
    }
    return result;
}

// Writes a single metric row for (u, v).
static void writeRow(const std::string& metricName, double score, int64_t u, int64_t v,
    std::vector<ValueVector*>& vectors) {
    static_cast<ValueVector*>(vectors[0])->setValue<int64_t>(0, u);
    static_cast<ValueVector*>(vectors[1])->setValue<int64_t>(0, v);
    static_cast<ValueVector*>(vectors[2])->setValue<std::string>(0, metricName);
    static_cast<ValueVector*>(vectors[3])->setValue<double>(0, score);
}

// Computes all requested metrics for one pair and appends rows to the table.
static void emitPair(const std::vector<std::vector<uint64_t>>& adj, uint64_t u, uint64_t v,
    const std::string& metric, double alpha, std::vector<ValueVector*>& vectors,
    processor::FactorizedTable* localFT) {
    const auto& nu = adj[u];
    const auto& nv = adj[v];
    const double degU = static_cast<double>(nu.size());
    const double degV = static_cast<double>(nv.size());
    const uint64_t common = commonNeighbors(nu, nv);
    const uint64_t unionSize = nu.size() + nv.size() - common;

    auto emit = [&](const std::string& name, double score) {
        writeRow(name, score, static_cast<int64_t>(u), static_cast<int64_t>(v), vectors);
        localFT->append(vectors);
    };

    if (metric == "all" || metric == "common_neighbors") {
        emit("common_neighbors", static_cast<double>(common));
    }
    if (metric == "all" || metric == "jaccard") {
        emit("jaccard", unionSize == 0 ? 0.0 : static_cast<double>(common) / unionSize);
    }
    if (metric == "all" || metric == "preferential_attachment") {
        emit("preferential_attachment", degU * degV);
    }
    if (metric == "all" || metric == "adamic_adar") {
        double s = 0.0;
        size_t i = 0, j = 0;
        while (i < nu.size() && j < nv.size()) {
            if (nu[i] < nv[j]) {
                i++;
            } else if (nv[j] < nu[i]) {
                j++;
            } else {
                const auto d = adj[nu[i]].size();
                if (d > 1) {
                    s += 1.0 / std::log(static_cast<double>(d));
                }
                i++;
                j++;
            }
        }
        emit("adamic_adar", s);
    }
    if (metric == "all" || metric == "resource_allocation") {
        double s = 0.0;
        size_t i = 0, j = 0;
        while (i < nu.size() && j < nv.size()) {
            if (nu[i] < nv[j]) {
                i++;
            } else if (nv[j] < nu[i]) {
                j++;
            } else {
                s += 1.0 / static_cast<double>(adj[nu[i]].size());
                i++;
                j++;
            }
        }
        emit("resource_allocation", s);
    }
    if (metric == "all" || metric == "katz_index") {
        emit("katz_index", katzIndex(adj, u, v, alpha));
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<LinkPredictionBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"LINK_PREDICTION supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildSimpleAdjacency(csr);

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto metricVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    metricVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), metricVector.get(),
        scoreVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    if (bindData->hasPair) {
        if (bindData->source >= numNodes || bindData->target >= numNodes) {
            sharedState->factorizedTablePool.returnLocalTable(localFT);
            throw BinderException{"LINK_PREDICTION source/target offset out of range."};
        }
        emitPair(adj, bindData->source, bindData->target, bindData->metric, bindData->alpha,
            vectors, localFT);
    } else {
        if (bindData->pivot >= numNodes) {
            sharedState->factorizedTablePool.returnLocalTable(localFT);
            throw BinderException{"LINK_PREDICTION pivot_node offset out of range."};
        }
        for (uint64_t t = 0; t < numNodes; ++t) {
            if (t == bindData->pivot) {
                continue;
            }
            emitPair(adj, bindData->pivot, t, bindData->metric, bindData->alpha, vectors,
                localFT);
        }
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar output (no node column): bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set LinkPredictionFunction::getFunctionSet() {
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
