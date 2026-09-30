// ALL_PAIRS_SHORTEST_PATH - unweighted BFS from every source (directed).
// P1-11 (NASH×GNN P1 batch): 「策略影响半径」距离矩阵, GNN 位置编码输入。
// 对标 NetworkX `floyd_warshall`/`all_pairs_shortest_path_length` 的无权语义
// (O(n(n+m))). One row per (source, target) ordered pair; unreachable pairs
// are reported with distance = -1 so the full matrix can be reconstructed.
//
// Optional named param `source_nodes` (LIST<INT64> of internal offsets, default
// NULL = all nodes) restricts the sources, e.g.
//   CALL all_pairs_shortest_path('g', source_nodes := [0, 3, 7])
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "binder/expression/expression_util.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/types/value/nested.h"
#include "common/types/value/value.h"
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

#include <cstdint>
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
static constexpr char DIST_COLUMN_NAME[] = "distance";

struct APSPBindData final : public GDSBindData {
    std::vector<uint64_t> sourceNodes; // empty = all nodes

    APSPBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        std::vector<uint64_t> sourceNodes)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          sourceNodes{std::move(sourceNodes)} {}

    APSPBindData(const APSPBindData& other)
        : GDSBindData{other}, sourceNodes{other.sourceNodes} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<APSPBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    std::vector<uint64_t> sourceNodes;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName != "sourcenodes") {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
        auto value = ExpressionUtil::evaluateAsLiteralValue(*optionalParam);
        if (value.isNull()) {
            continue; // NULL (the documented default) = all nodes
        }
        auto typeID = value.getDataType().getLogicalTypeID();
        if (typeID != LogicalTypeID::LIST && typeID != LogicalTypeID::ARRAY) {
            throw BinderException{"source_nodes must be a LIST of node offsets."};
        }
        for (auto i = 0u; i < value.getChildrenSize(); ++i) {
            auto* child = NestedVal::getChildVal(&value, i);
            if (child->isNull()) {
                throw BinderException{"source_nodes must not contain NULL offsets."};
            }
            if (!LogicalTypeUtils::isIntegral(child->getDataType())) {
                throw BinderException{"source_nodes must be a LIST of integer node offsets."};
            }
            auto offset = child->getValue<int64_t>();
            if (offset < 0) {
                throw BinderException{"source_nodes offsets must be non-negative."};
            }
            sourceNodes.push_back(static_cast<uint64_t>(offset));
        }
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DIST_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<APSPBindData>(std::move(columns), std::move(graphEntry),
        std::move(sourceNodes));
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
    auto bindData = input.bindData->constPtrCast<APSPBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "ALL_PAIRS_SHORTEST_PATH currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);

    std::vector<uint64_t> sources = bindData->sourceNodes;
    if (sources.empty()) {
        sources.resize(numNodes);
        for (uint64_t i = 0; i < numNodes; ++i) {
            sources[i] = i;
        }
    } else {
        for (const auto s : sources) {
            if (s >= numNodes) {
                throw BinderException{"ALL_PAIRS_SHORTEST_PATH source offset out of range: " +
                                      std::to_string(s)};
            }
        }
    }

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto distVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    distVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), distVector.get()};

    std::vector<int64_t> dist(numNodes);
    std::vector<uint64_t> queue;
    queue.reserve(numNodes);
    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto s : sources) {
        std::fill(dist.begin(), dist.end(), -1);
        dist[s] = 0;
        queue.clear();
        queue.push_back(s);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto u = queue[head];
            for (auto it = csr.fwdNeighbors.begin() +
                 static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
                 it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
                 ++it) {
                const auto v = *it;
                if (dist[v] == -1) {
                    dist[v] = dist[u] + 1;
                    queue.push_back(v);
                }
            }
        }
        for (uint64_t t = 0; t < numNodes; ++t) {
            srcVector->setValue<int64_t>(0, static_cast<int64_t>(s));
            dstVector->setValue<int64_t>(0, static_cast<int64_t>(t));
            distVector->setValue<int64_t>(0, dist[t]);
            localFT->append(vectors);
        }
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar-only output (no node column): use the GRAPH_DIFF/BRIDGES-style plan
// (a bare TableFunctionCall without node-property scan).
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set AllPairsShortestPathFunction::getFunctionSet() {
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
