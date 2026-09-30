// EFFECTIVE_DIAMETER - smallest distance d such that at least `ratio` (90%) of
// all reachable pairs are within d (undirected view). P1-12 (NASH×GNN P1
// batch): 对标 NetWorKit `EffectiveDiameter`. `approx := true` (default)
// samples up to 100 sources (deterministic seed) instead of running all n
// BFS passes; `exact` column reports which mode produced the value.
// No reachable pairs (empty / all-isolated graph) -> -1.
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
#include <cstdint>
#include <random>
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

static constexpr char DIAMETER_COLUMN_NAME[] = "diameter";
static constexpr char EXACT_COLUMN_NAME[] = "exact";

static constexpr double DEFAULT_RATIO = 0.9;
static constexpr bool DEFAULT_APPROX = true;
static constexpr uint64_t APPROX_MAX_SOURCES = 100;
// Fixed seed: sampled sources are reproducible across runs.
static constexpr uint64_t SAMPLING_SEED = 0x5EED5EED;

struct EffectiveDiameterBindData final : public GDSBindData {
    double ratio;
    bool approx;

    EffectiveDiameterBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        double ratio, bool approx)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          ratio{ratio}, approx{approx} {}

    EffectiveDiameterBindData(const EffectiveDiameterBindData& other)
        : GDSBindData{other}, ratio{other.ratio}, approx{other.approx} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<EffectiveDiameterBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    double ratio = DEFAULT_RATIO;
    bool approx = DEFAULT_APPROX;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "ratio") {
            ratio = ExpressionUtil::evaluateLiteral<double>(context, optionalParam,
                LogicalType::DOUBLE());
        } else if (paramName == "approx") {
            approx = ExpressionUtil::evaluateLiteral<bool>(context, optionalParam,
                LogicalType::BOOL());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (ratio <= 0.0 || ratio > 1.0) {
        throw BinderException{"EFFECTIVE_DIAMETER ratio must be in (0, 1]."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(DIAMETER_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(EXACT_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<EffectiveDiameterBindData>(std::move(columns), std::move(graphEntry),
        ratio, approx);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

// Collects the multiplicity of every reachable-pair distance from the given
// sources (unordered pairs, one direction only to avoid double counting).
static void collectDistanceHistogram(const std::vector<std::vector<uint64_t>>& adj,
    const std::vector<uint64_t>& sources, std::vector<uint64_t>& hist) {
    const auto n = adj.size();
    std::vector<int64_t> dist(n);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    for (const auto s : sources) {
        std::fill(dist.begin(), dist.end(), -1);
        dist[s] = 0;
        queue.clear();
        queue.push_back(s);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto u = queue[head];
            for (const auto v : adj[u]) {
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    queue.push_back(v);
                }
            }
        }
        for (uint64_t v = s + 1; v < n; ++v) {
            if (dist[v] > 0) {
                if (hist.size() <= static_cast<size_t>(dist[v])) {
                    hist.resize(static_cast<size_t>(dist[v]) + 1, 0);
                }
                hist[dist[v]]++;
            }
        }
    }
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<EffectiveDiameterBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "EFFECTIVE_DIAMETER currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);

    std::vector<uint64_t> sources;
    if (!bindData->approx || numNodes <= APPROX_MAX_SOURCES) {
        sources.resize(numNodes);
        for (uint64_t i = 0; i < numNodes; ++i) {
            sources[i] = i;
        }
    } else {
        // Deterministic random sample of sources (Fisher-Yates on an index list).
        std::mt19937_64 rng{SAMPLING_SEED};
        std::vector<uint64_t> all(numNodes);
        for (uint64_t i = 0; i < numNodes; ++i) {
            all[i] = i;
        }
        for (uint64_t i = 0; i < APPROX_MAX_SOURCES; ++i) {
            const auto j = i + rng() % (numNodes - i);
            std::swap(all[i], all[j]);
        }
        sources.assign(all.begin(), all.begin() + APPROX_MAX_SOURCES);
        std::sort(sources.begin(), sources.end());
    }

    std::vector<uint64_t> hist;
    collectDistanceHistogram(adj, sources, hist);
    uint64_t totalPairs = 0;
    for (const auto c : hist) {
        totalPairs += c;
    }
    int64_t effectiveDiameter = -1;
    if (totalPairs > 0) {
        uint64_t cumulative = 0;
        const double threshold = bindData->ratio * static_cast<double>(totalPairs);
        for (size_t d = 1; d < hist.size(); ++d) {
            cumulative += hist[d];
            if (static_cast<double>(cumulative) + 1e-12 >= threshold) {
                effectiveDiameter = static_cast<int64_t>(d);
                break;
            }
        }
    }
    const bool exact = (!bindData->approx || numNodes <= APPROX_MAX_SOURCES);

    auto diameterVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    diameterVector->state = DataChunkState::getSingleValueDataChunkState();
    auto exactVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    exactVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{diameterVector.get(), exactVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    diameterVector->setValue<int64_t>(0, effectiveDiameter);
    exactVector->setValue<bool>(0, exact);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar-only output: bare TableFunctionCall plan (same as BRIDGES/GRAPH_DIFF).
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set EffectiveDiameterFunction::getFunctionSet() {
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
