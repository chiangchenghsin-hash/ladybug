// GRAPH_EDIT_DISTANCE — Sanfeliu & Fu 1983 图编辑距离的精确计算（DFS-BB，Abu-Aisheh 2015）。
// 架构治理用途：SHOULD-BE（设计图）vs AS-IS（实现图）的结构差异量化。
//   - 结构 GED（MVP 无属性代价）：节点/边替换 cost 0，删除/插入 cost 1
//   - 有向：边替换仅当方向一致且端点配对一致（NetworkX 标记 TODO 的有向分支，我们天然按有向设计）
//   - 快速路径：平凡上界为 0 ⟺ multigraph 同构 → distance = 0（不用 VF2++ 判定同构
//     ——子图同构的存在性语义对平行边分布不同的图会误报；对称差计数更准确）
//   - 初始上界：边集对称差 + 节点数差（GRAPH_DIFF 的平凡上界）
//   - 近似模式：approximate := true 走 bipartite（Riesen-Bunke 2009，大图 fallback）
// 复用：双图绑定 = GRAPH_DIFF 模式；输出为纯值列（无 node 输出）→ 自定义 getLogicalPlan。
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/ged_core.h"
#include "common/types/types.h"
#include "common/vf2pp_core.h"
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

static constexpr char DISTANCE_COLUMN_NAME[] = "distance";
static constexpr char NODE_EDITS_COLUMN_NAME[] = "node_edits";
static constexpr char EDGE_EDITS_COLUMN_NAME[] = "edge_edits";
static constexpr char APPROXIMATE_COLUMN_NAME[] = "approximate";

// 可选参数：approximate（大图 fallback，bipartite 近似上界，默认精确 DFS-BB）。
struct ApproximateParam {
    static constexpr const char* NAME = "approximate";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::BOOL;
    static constexpr bool DEFAULT_VALUE = false;

    static void validate(bool) {}
};

struct GraphEditDistanceOptionalParams final : public function::OptionalParams {
    function::OptionalParam<ApproximateParam> approximate;

    GraphEditDistanceOptionalParams() = default;

    explicit GraphEditDistanceOptionalParams(const binder::expression_vector& optionalParams) {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == ApproximateParam::NAME) {
                approximate = function::OptionalParam<ApproximateParam>(optionalParam);
            } else {
                throw BinderException{
                    "Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    void evaluateParams(main::ClientContext* context) override {
        approximate.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        auto result = std::make_unique<GraphEditDistanceOptionalParams>();
        result->approximate = approximate;
        return result;
    }
};

// 双图 bind data：GDSBindData 持 AS-IS 图，shouldBeGraphEntry 持 SHOULD-BE 图
// （与 GRAPH_DIFF 相同的双图绑定模式）。
struct GraphEditDistanceBindData final : public GDSBindData {
    graph::NativeGraphEntry shouldBeGraphEntry;
    std::unique_ptr<function::OptionalParams> optionalParams;

    GraphEditDistanceBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        graph::NativeGraphEntry shouldBeGraphEntry,
        std::unique_ptr<GraphEditDistanceOptionalParams> optionalParams)
        : GDSBindData{std::move(columns), std::move(graphEntry), {}},
          shouldBeGraphEntry{std::move(shouldBeGraphEntry)},
          optionalParams{std::move(optionalParams)} {}

    GraphEditDistanceBindData(const GraphEditDistanceBindData& other)
        : GDSBindData{other}, shouldBeGraphEntry{other.shouldBeGraphEntry.copy()} {
        optionalParams = other.optionalParams == nullptr ? nullptr : other.optionalParams->copy();
    }

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GraphEditDistanceBindData>(*this);
    }
};

// 双图 shared state。
struct GraphEditDistanceSharedState : public TableFuncSharedState {
    std::unique_ptr<graph::Graph> shouldBeGraph;
    std::unique_ptr<graph::Graph> asIsGraph;
    processor::FactorizedTablePool factorizedTablePool;

    GraphEditDistanceSharedState(std::unique_ptr<graph::Graph> shouldBeGraph,
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
    columns.push_back(input->binder->createVariable(DISTANCE_COLUMN_NAME, LogicalType::DOUBLE()));
    columns.push_back(
        input->binder->createVariable(NODE_EDITS_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(
        input->binder->createVariable(EDGE_EDITS_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(
        input->binder->createVariable(APPROXIMATE_COLUMN_NAME, LogicalType::BOOL()));
    return std::make_unique<GraphEditDistanceBindData>(std::move(columns),
        std::move(asIsGraphEntry), std::move(shouldBeGraphEntry),
        std::make_unique<GraphEditDistanceOptionalParams>(input->optionalParamsLegacy));
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GraphEditDistanceBindData>();
    auto shouldBeGraph = std::make_unique<OnDiskGraph>(input.context->clientContext,
        bindData->shouldBeGraphEntry.copy());
    auto asIsGraph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GraphEditDistanceSharedState>(std::move(shouldBeGraph),
        std::move(asIsGraph), bindData->getResultTable());
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
    auto sharedState = input.sharedState->ptrCast<GraphEditDistanceSharedState>();
    auto mm = MemoryManager::Get(*clientContext);

    const auto shouldBeMaxOffset = sharedState->shouldBeGraph->getMaxOffsetMap(transaction);
    const auto asIsMaxOffset = sharedState->asIsGraph->getMaxOffsetMap(transaction);
    if (shouldBeMaxOffset.size() != 1 || asIsMaxOffset.size() != 1) {
        throw BinderException{
            "GRAPH_EDIT_DISTANCE currently supports single-node-table graphs only."};
    }
    const auto shouldBeTableID = shouldBeMaxOffset.begin()->first;
    const auto shouldBeNumNodes = shouldBeMaxOffset.begin()->second;
    const auto asIsTableID = asIsMaxOffset.begin()->first;
    const auto asIsNumNodes = asIsMaxOffset.begin()->second;

    // 1. Projected graphs -> directed CSRs, then exact GED (DFS-BB) or
    //    bipartite approximation (large graphs).
    auto shouldBe = buildCSR(shouldBeTableID, shouldBeNumNodes, sharedState->shouldBeGraph.get());
    auto asIs = buildCSR(asIsTableID, asIsNumNodes, sharedState->asIsGraph.get());

    auto bindData = input.bindData->constPtrCast<GraphEditDistanceBindData>();
    const bool approximateMode =
        bindData->optionalParams &&
        bindData->optionalParams->constCast<GraphEditDistanceOptionalParams>()
            .approximate.getParamVal();
    GEDParams params; // 默认权重（节点/边删插 = 1），无超时/上界参数。
    auto result =
        approximateMode ? approximateGED(shouldBe, asIs, params) : computeGED(shouldBe, asIs, params);

    // 2. Stream a single (distance, node_edits, edge_edits, approximate) row.
    auto distanceVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    distanceVector->state = DataChunkState::getSingleValueDataChunkState();
    auto nodeEditsVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    nodeEditsVector->state = DataChunkState::getSingleValueDataChunkState();
    auto edgeEditsVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    edgeEditsVector->state = DataChunkState::getSingleValueDataChunkState();
    auto approximateVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    approximateVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{distanceVector.get(), nodeEditsVector.get(),
        edgeEditsVector.get(), approximateVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    distanceVector->setValue<double>(0, result.distance);
    nodeEditsVector->setValue<int64_t>(0, static_cast<int64_t>(result.nodeEdits));
    edgeEditsVector->setValue<int64_t>(0, static_cast<int64_t>(result.edgeEdits));
    approximateVector->setValue<bool>(0, approximateMode || result.timedOut);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// GRAPH_EDIT_DISTANCE has no node output (pure value columns), so the stock GDS logical plan hook
// (which unconditionally dereferences output[0] as a node expression) must not be used.
// Same pattern as GRAPH_DIFF.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GraphEditDistanceBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set GraphEditDistanceFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(GraphEditDistanceFunction::name,
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
