// GRAPH_DIFF — Reflexion Model 三分类（Murphy 1995）的图级实现。
// 比较两个投影图（SHOULD-BE vs AS-IS）的边集合，输出：
//   convergence — 两边都有（一致）
//   divergence  — AS-IS 有但 SHOULD-BE 没有（幽灵边）
//   absence     — SHOULD-BE 有但 AS-IS 没有（缺失边）
// 节点按 offset 对齐（MVP：两图各单节点表且节点数相等）。
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/types.h"
#include "function/algo_function.h"
#include "function/gds/gds.h"
#include "function/table/bind_input.h"
#include "function/table/table_function.h"
#include "graph/on_disk_graph.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "processor/operator/table_function_call.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <format>
#include <set>
#include <utility>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char SRC_COLUMN_NAME[] = "src";
static constexpr char DST_COLUMN_NAME[] = "dst";
static constexpr char STATUS_COLUMN_NAME[] = "status";

// 双图 bind data：GDSBindData 持 AS-IS 图，shouldBeGraphEntry 持 SHOULD-BE 图。
struct GraphDiffBindData final : public GDSBindData {
    graph::NativeGraphEntry shouldBeGraphEntry;

    GraphDiffBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        graph::NativeGraphEntry shouldBeGraphEntry)
        : GDSBindData{std::move(columns), std::move(graphEntry), {}},
          shouldBeGraphEntry{std::move(shouldBeGraphEntry)} {}

    GraphDiffBindData(const GraphDiffBindData& other)
        : GDSBindData{other}, shouldBeGraphEntry{other.shouldBeGraphEntry.copy()} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GraphDiffBindData>(*this);
    }
};

// 双图 shared state。
struct GraphDiffSharedState : public TableFuncSharedState {
    std::unique_ptr<graph::Graph> shouldBeGraph;
    std::unique_ptr<graph::Graph> asIsGraph;
    processor::FactorizedTablePool factorizedTablePool;

    GraphDiffSharedState(std::unique_ptr<graph::Graph> shouldBeGraph,
        std::unique_ptr<graph::Graph> asIsGraph, std::shared_ptr<processor::FactorizedTable> fTable)
        : TableFuncSharedState{}, shouldBeGraph{std::move(shouldBeGraph)},
          asIsGraph{std::move(asIsGraph)}, factorizedTablePool{std::move(fTable)} {}
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto shouldBeName = input->getLiteralVal<std::string>(0);
    auto asIsName = input->getLiteralVal<std::string>(1);
    auto shouldBeGraphEntry = GDSFunction::bindGraphEntry(*context, shouldBeName);
    auto asIsGraphEntry = GDSFunction::bindGraphEntry(*context, asIsName);

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(STATUS_COLUMN_NAME, LogicalType::STRING()));
    return std::make_unique<GraphDiffBindData>(std::move(columns), std::move(asIsGraphEntry),
        std::move(shouldBeGraphEntry));
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GraphDiffBindData>();
    auto shouldBeGraph = std::make_unique<OnDiskGraph>(input.context->clientContext,
        bindData->shouldBeGraphEntry.copy());
    auto asIsGraph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GraphDiffSharedState>(std::move(shouldBeGraph), std::move(asIsGraph),
        bindData->getResultTable());
}

static DirectedCSR buildCSR(table_id_t tableID, offset_t numNodes, Graph* graph) {
    std::vector<std::pair<uint64_t, uint64_t>> edges;
    const auto nbrTables = graph->getRelInfos(tableID);
    const auto nbrInfo = nbrTables[0];
    const auto scanState = graph->prepareRelScan(*nbrInfo.relGroupEntry, nbrInfo.relTableID,
        nbrInfo.dstTableID, {}, false /*randomLookup*/);
    for (offset_t nodeId = 0; nodeId < numNodes; ++nodeId) {
        const nodeID_t nid = {nodeId, tableID};
        for (auto chunk : graph->scanFwd(nid, *scanState)) {
            chunk.forEach([&](auto neighbors, auto, auto i) {
                edges.emplace_back(nodeId, neighbors[i].offset);
            });
        }
    }
    return buildDirectedCSR(numNodes, edges);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GraphDiffSharedState>();
    auto mm = MemoryManager::Get(*clientContext);

    const auto shouldBeMaxOffset = sharedState->shouldBeGraph->getMaxOffsetMap(transaction);
    const auto asIsMaxOffset = sharedState->asIsGraph->getMaxOffsetMap(transaction);
    if (shouldBeMaxOffset.size() != 1 || asIsMaxOffset.size() != 1) {
        throw BinderException{"GRAPH_DIFF currently supports single-node-table graphs only."};
    }
    const auto shouldBeNumNodes = shouldBeMaxOffset.begin()->second;
    const auto asIsNumNodes = asIsMaxOffset.begin()->second;
    if (shouldBeNumNodes != asIsNumNodes) {
        throw BinderException{std::format(
            "GRAPH_DIFF requires equal node counts for offset alignment "
            "(SHOULD-BE: {} nodes, AS-IS: {} nodes).",
            shouldBeNumNodes, asIsNumNodes)};
    }

    // 1. Projected graphs -> directed CSRs, then edge sets.
    auto shouldBe = buildCSR(shouldBeMaxOffset.begin()->first, shouldBeNumNodes,
        sharedState->shouldBeGraph.get());
    auto asIs = buildCSR(asIsMaxOffset.begin()->first, asIsNumNodes,
        sharedState->asIsGraph.get());

    std::set<std::pair<uint64_t, uint64_t>> shouldBeEdges;
    for (uint64_t u = 0; u < shouldBe.numNodes; ++u) {
        for (auto it = shouldBe.fwdNeighbors.begin() +
                           static_cast<ptrdiff_t>(shouldBe.fwdOffsets[u]);
             it != shouldBe.fwdNeighbors.begin() +
                       static_cast<ptrdiff_t>(shouldBe.fwdOffsets[u + 1]);
             ++it) {
            shouldBeEdges.emplace(u, *it);
        }
    }
    std::set<std::pair<uint64_t, uint64_t>> asIsEdges;
    for (uint64_t u = 0; u < asIs.numNodes; ++u) {
        for (auto it = asIs.fwdNeighbors.begin() + static_cast<ptrdiff_t>(asIs.fwdOffsets[u]);
             it != asIs.fwdNeighbors.begin() + static_cast<ptrdiff_t>(asIs.fwdOffsets[u + 1]);
             ++it) {
            asIsEdges.emplace(u, *it);
        }
    }

    // 2. Reflexion 三分类。
    struct Row {
        int64_t src;
        int64_t dst;
        std::string status;
    };
    std::vector<Row> rows;
    rows.reserve(shouldBeEdges.size() + asIsEdges.size());
    for (const auto& e : shouldBeEdges) {
        rows.push_back({static_cast<int64_t>(e.first), static_cast<int64_t>(e.second),
            asIsEdges.contains(e) ? "convergence" : "absence"});
    }
    for (const auto& e : asIsEdges) {
        if (!shouldBeEdges.contains(e)) {
            rows.push_back({static_cast<int64_t>(e.first), static_cast<int64_t>(e.second),
                "divergence"});
        }
    }

    // 3. Stream (src, dst, status) rows.
    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto statusVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    statusVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), statusVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& row : rows) {
        srcVector->setValue<int64_t>(0, row.src);
        dstVector->setValue<int64_t>(0, row.dst);
        statusVector->setValue(0, row.status);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// GRAPH_DIFF has no node output (pure value columns), so the stock GDS logical plan hook
// (which unconditionally dereferences output[0] as a node expression) must not be used.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GraphDiffBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set GraphDiffFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GraphDiffFunction::name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY, LogicalTypeID::ANY});
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
