#include "hyper_project.h"

#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "catalog/catalog.h"
#include "common/exception/binder.h"
#include "common/types/value/nested.h"
#include "function/gds/gds.h"
#include "hyper_graph.h"
#include "function/table/bind_input.h"
#include "graph/graph_entry_set.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace hyperalgo_extension {

namespace {

// 解析表名参数(LIST of STRING;与 PROJECT_GRAPH 同口径)
std::vector<graph::ParsedNativeGraphTableInfo> extractTableInfos(const Value& value) {
    std::vector<graph::ParsedNativeGraphTableInfo> infos;
    switch (value.getDataType().getLogicalTypeID()) {
    case LogicalTypeID::LIST: {
        for (auto i = 0u; i < NestedVal::getChildrenSize(&value); ++i) {
            auto& child = *NestedVal::getChildVal(&value, i);
            infos.emplace_back(child.toString(), "" /* empty predicate */);
        }
    } break;
    default:
        throw BinderException(std::format(
            "Argument {} has data type {}. LIST was expected.", value.toString(),
            value.getDataType().toString()));
    }
    return infos;
}

struct HyperProjectBindData final : GDSBindData {
    std::string graphName;

    HyperProjectBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector output, std::string graphName)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          graphName{std::move(graphName)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<HyperProjectBindData>(columns, graphEntry.copy(), output, graphName);
    }
};

static constexpr char N_COLUMNS[][32] = {
    "graph", "n_nodes", "n_edges", "isolated_stations", "degenerate_edges", "fingerprint"};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto nodeInfos = extractTableInfos(input->getValue(1));
    auto relInfos = extractTableInfos(input->getValue(2));
    graph::ParsedNativeGraphEntry parsed(std::move(nodeInfos), std::move(relInfos));
    auto graphEntry = GDSFunction::bindGraphEntry(*context, parsed);
    expression_vector columns;
    columns.push_back(input->binder->createVariable(N_COLUMNS[0], LogicalType::STRING()));
    columns.push_back(input->binder->createVariable(N_COLUMNS[1], LogicalType::UINT64()));
    columns.push_back(input->binder->createVariable(N_COLUMNS[2], LogicalType::UINT64()));
    columns.push_back(input->binder->createVariable(N_COLUMNS[3], LogicalType::UINT64()));
    columns.push_back(input->binder->createVariable(N_COLUMNS[4], LogicalType::UINT64()));
    columns.push_back(input->binder->createVariable(N_COLUMNS[5], LogicalType::STRING()));
    return std::make_unique<HyperProjectBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{}, graphName);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto tx = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto bindData = input.bindData->constPtrCast<HyperProjectBindData>();

    // 构建 + 缓存(FRZ-02;契约 §3.1 缓存句柄)
    auto handle = buildHandleFromGraph(graph, tx);
    HyperGraphRegistry::instance().put(bindData->graphName, std::move(handle));
    const auto& h = HyperGraphRegistry::instance().get(bindData->graphName);
    const std::string fingerprint = hyperalgo::fingerprint(h.csr, h.idmap);

    auto mm = MemoryManager::Get(*clientContext);
    auto nameVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    nameVector->state = DataChunkState::getSingleValueDataChunkState();
    auto nNodesVector = std::make_unique<ValueVector>(LogicalType::UINT64(), mm);
    nNodesVector->state = DataChunkState::getSingleValueDataChunkState();
    auto nEdgesVector = std::make_unique<ValueVector>(LogicalType::UINT64(), mm);
    nEdgesVector->state = DataChunkState::getSingleValueDataChunkState();
    auto isoVector = std::make_unique<ValueVector>(LogicalType::UINT64(), mm);
    isoVector->state = DataChunkState::getSingleValueDataChunkState();
    auto degVector = std::make_unique<ValueVector>(LogicalType::UINT64(), mm);
    degVector->state = DataChunkState::getSingleValueDataChunkState();
    auto fpVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    fpVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{
        nameVector.get(), nNodesVector.get(), nEdgesVector.get(), isoVector.get(),
        degVector.get(), fpVector.get()};

    nameVector->copyFromValue(0, Value::createValue(bindData->graphName));
    nNodesVector->setValue<uint64_t>(0, h.csr.n_nodes);
    nEdgesVector->setValue<uint64_t>(0, h.csr.n_edges);
    isoVector->setValue<uint64_t>(0, h.isolated_stations);
    degVector->setValue<uint64_t>(0, h.degenerate_edges);
    fpVector->copyFromValue(0, Value::createValue(fingerprint));

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static void getLogicalPlan(planner::Planner* planner,
    const binder::BoundReadingClause& readingClause, binder::expression_vector predicates,
    planner::LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<HyperProjectBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

} // namespace

function_set HyperProjectFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(
        name, std::vector{LogicalTypeID::STRING, LogicalTypeID::ANY, LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace hyperalgo_extension
} // namespace lbug
