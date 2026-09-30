// 判定层 v0(单站直接上游布尔齐套;FRZ-11 / P-1.5 §3.3)
// 实现形态(v1.1b 决策):扩展内 CALL——图结构走 hyperalgo-core CSR,库存走 RoundState 扫描;
//   纯 Cypher 金本(FRZ-11)保留为对照 oracle,二者同图互证(见 E2E HyperalgoGoldenJudge)。
#include "hyper_kitting_bool.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "catalog/catalog.h"
#include "catalog/catalog_entry/table_catalog_entry.h"
#include "common/exception/binder.h"
#include "common/types/value/nested.h"
#include "function/gds/gds.h"
#include "function/table/bind_input.h"
#include "graph/graph_entry_set.h"
#include "hyper_graph.h"
#include "main/database.h"
#include "planner/operator/logical_table_function_call.h"
#include "planner/planner.h"
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "transaction/transaction.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace hyperalgo_extension {

namespace {

// 解析表名参数(LIST of STRING;与 PROJECT_GRAPH 同口径)
std::vector<ParsedNativeGraphTableInfo> extractTableInfos(const Value& value) {
    std::vector<ParsedNativeGraphTableInfo> infos;
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

struct HyperKittingBoolBindData final : GDSBindData {
    std::string graphName;
    std::string stateTableName;
    int64_t round;

    HyperKittingBoolBindData(expression_vector columns, NativeGraphEntry graphEntry,
        expression_vector output, std::string graphName, std::string stateTableName, int64_t round)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          graphName{std::move(graphName)}, stateTableName{std::move(stateTableName)},
          round{round} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<HyperKittingBoolBindData>(columns, graphEntry.copy(), output,
            graphName, stateTableName, round);
    }
};

// ext id = "<tableID>:<offset>"(hyper_graph.h 冻结格式)
std::pair<uint32_t, uint32_t> parseExtId(const std::string& ext) {
    const auto pos = ext.find(':');
    if (pos == std::string::npos)
        throw BinderException("hyper_kitting_bool: invalid ext id '" + ext + "'");
    return {(uint32_t)std::stoul(ext.substr(0, pos)), (uint32_t)std::stoul(ext.substr(pos + 1))};
}

uint64_t nodeKey(uint32_t tableID, uint32_t offset) {
    return (uint64_t(tableID) << 32) | offset;
}

int64_t readIntProp(const VertexScanState::Chunk& chunk, size_t propIdx, size_t i,
    LogicalTypeID type) {
    switch (type) {
    case LogicalTypeID::INT64:
        return chunk.getProperties<int64_t>(propIdx)[i];
    case LogicalTypeID::INT32:
        return chunk.getProperties<int32_t>(propIdx)[i];
    case LogicalTypeID::INT16:
        return chunk.getProperties<int16_t>(propIdx)[i];
    case LogicalTypeID::INT8:
        return chunk.getProperties<int8_t>(propIdx)[i];
    case LogicalTypeID::UINT64:
        return (int64_t)chunk.getProperties<uint64_t>(propIdx)[i];
    case LogicalTypeID::UINT32:
        return (int64_t)chunk.getProperties<uint32_t>(propIdx)[i];
    case LogicalTypeID::UINT16:
        return (int64_t)chunk.getProperties<uint16_t>(propIdx)[i];
    case LogicalTypeID::UINT8:
        return (int64_t)chunk.getProperties<uint8_t>(propIdx)[i];
    default:
        throw BinderException("hyper_kitting_bool: 整数属性类型不支持(type id " +
                              std::to_string((int)type) + ")");
    }
}

double readDoubleProp(const VertexScanState::Chunk& chunk, size_t propIdx, size_t i,
    LogicalTypeID type) {
    switch (type) {
    case LogicalTypeID::DOUBLE:
        return chunk.getProperties<double>(propIdx)[i];
    case LogicalTypeID::FLOAT:
        return (double)chunk.getProperties<float>(propIdx)[i];
    default:
        return (double)readIntProp(chunk, propIdx, i, type);
    }
}

struct AgentNode {
    uint32_t tableID;
    uint32_t offset;
    int64_t bizId;
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto nodeInfos = extractTableInfos(input->getValue(1));
    auto relInfos = extractTableInfos(input->getValue(2));
    auto stateTableName = input->getLiteralVal<std::string>(3);
    auto round = input->getLiteralVal<int64_t>(4);
    ParsedNativeGraphEntry parsed(std::move(nodeInfos), std::move(relInfos));
    auto graphEntry = GDSFunction::bindGraphEntry(*context, parsed);
    expression_vector columns;
    columns.push_back(input->binder->createVariable("station", LogicalType::INT64()));
    columns.push_back(input->binder->createVariable("no_supplier", LogicalType::BOOL()));
    columns.push_back(input->binder->createVariable("kitting", LogicalType::BOOL()));
    columns.push_back(input->binder->createVariable("missing", LogicalType::STRING()));
    return std::make_unique<HyperKittingBoolBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{}, graphName, stateTableName, round);
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto tx = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto bindData = input.bindData->constPtrCast<HyperKittingBoolBindData>();
    auto* catalog = clientContext->getDatabase()->getCatalog();

    // 1) CSR(与分析层同源;FLOWS → 超边)
    auto handle = buildHandleFromGraph(graph, tx);
    const auto& csr = handle.csr;
    const auto& idmap = handle.idmap;
    std::unordered_map<std::string, uint32_t> internalOf;
    for (uint64_t u = 0; u < csr.n_nodes; ++u)
        internalOf[idmap.ext_of_int((uint32_t)u)] = (uint32_t)u;
    std::unordered_map<uint32_t, uint64_t> edgeOfStation; // S 内部 id → edge_pos
    for (uint64_t e = 0; e < csr.n_edges; ++e)
        edgeOfStation[csr.edge_business_ids[e]] = e;
    std::unordered_set<uint32_t> memberNodes;
    for (uint64_t j = 0; j < csr.edge_nodes.size(); ++j)
        memberNodes.insert(csr.edge_nodes[j]);

    // 2) 库存地图:RoundState(round/agent_id/inventory)→ 目标轮 agent_id → inventory
    auto* stateEntry = catalog->getTableCatalogEntry(tx, bindData->stateTableName);
    if (stateEntry == nullptr)
        throw BinderException("hyper_kitting_bool: state table '" + bindData->stateTableName +
                              "' not found");
    const std::string roundProp = "round";
    const std::string agentProp = "agent_id";
    const std::string invProp = "inventory";
    if (!stateEntry->containsProperty(roundProp) || !stateEntry->containsProperty(agentProp) ||
        !stateEntry->containsProperty(invProp))
        throw BinderException("hyper_kitting_bool: state table '" + bindData->stateTableName +
                              "' 需含 round/agent_id/inventory 列(附录 A 列名契约)");
    std::unordered_map<int64_t, double> inventory;
    {
        auto scan = graph->prepareVertexScan(
            stateEntry, std::vector<std::string>{roundProp, agentProp, invProp});
        const auto tRound = stateEntry->getProperty(roundProp).getType().getLogicalTypeID();
        const auto tAgent = stateEntry->getProperty(agentProp).getType().getLogicalTypeID();
        const auto tInv = stateEntry->getProperty(invProp).getType().getLogicalTypeID();
        const auto maxOffset = StorageManager::Get(*clientContext)->getTable(stateEntry->getTableID())->getNumTotalRows(tx);
        for (auto chunk : graph->scanVertices(0, maxOffset, *scan)) {
            for (size_t i = 0; i < chunk.size(); ++i) {
                const auto r = readIntProp(chunk, 0, i, tRound);
                if (r != bindData->round) continue;
                inventory[readIntProp(chunk, 1, i, tAgent)] = readDoubleProp(chunk, 2, i, tInv);
            }
        }
    }

    // 3) 节点业务 id 映射(全节点)+ 站集合 = 有入边节点 ∪ 无入边且非任何超边成员的节点(孤立站)
    std::unordered_map<uint64_t, int64_t> bizIdOf; // (tableID,offset) → 业务 id
    std::vector<AgentNode> stations;
    for (const auto tableID : graph->getNodeTableIDs()) {
        if (tableID == stateEntry->getTableID()) continue;
        auto* entry = catalog->getTableCatalogEntry(tx, tableID);
        const bool hasId = entry->containsProperty("id");
        auto scan = graph->prepareVertexScan(
            entry, hasId ? std::vector<std::string>{"id"} : std::vector<std::string>{});
        const auto tId = hasId
            ? entry->getProperty("id").getType().getLogicalTypeID()
            : LogicalTypeID::INT64;
        const auto maxOffset = StorageManager::Get(*clientContext)->getTable(tableID)->getNumTotalRows(tx);
        for (auto chunk : graph->scanVertices(0, maxOffset, *scan)) {
            for (size_t i = 0; i < chunk.size(); ++i) {
                const auto& nid = chunk.getNodeIDs()[i];
                const std::string ext =
                    std::to_string(nid.tableID) + ":" + std::to_string(nid.offset);
                const int64_t bizId =
                    hasId ? readIntProp(chunk, 0, i, tId) : (int64_t)nid.offset;
                bizIdOf[nodeKey((uint32_t)nid.tableID, (uint32_t)nid.offset)] = bizId;
                // 过滤纯成员节点(料号/中间站作为成员):只保留 S(有入边)或孤立站
                const auto it = internalOf.find(ext);
                const bool isMember = it != internalOf.end() && memberNodes.count(it->second) > 0;
                const bool isStationS = it != internalOf.end() &&
                                        edgeOfStation.count(it->second) > 0;
                if (isMember && !isStationS) continue;
                stations.push_back({(uint32_t)nid.tableID, (uint32_t)nid.offset, bizId});
            }
        }
    }

    // 4) 逐站判定(FRZ-11 真值表)
    auto mm = MemoryManager::Get(*clientContext);
    auto stationVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    stationVector->state = DataChunkState::getSingleValueDataChunkState();
    auto noSupplierVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    noSupplierVector->state = DataChunkState::getSingleValueDataChunkState();
    auto kittingVector = std::make_unique<ValueVector>(LogicalType::BOOL(), mm);
    kittingVector->state = DataChunkState::getSingleValueDataChunkState();
    auto missingVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    missingVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{stationVector.get(), noSupplierVector.get(),
        kittingVector.get(), missingVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& st : stations) {
        bool noSupplier = true;
        bool kitting = true;
        std::vector<std::string> missing;
        const std::string ext = std::to_string(st.tableID) + ":" + std::to_string(st.offset);
        const auto it = internalOf.find(ext);
        if (it != internalOf.end()) {
            const auto eit = edgeOfStation.find(it->second);
            if (eit != edgeOfStation.end()) {
                const uint64_t e = eit->second;
                noSupplier = (csr.edge_ptr[e] == csr.edge_ptr[e + 1]);
                for (uint64_t j = csr.edge_ptr[e]; j < csr.edge_ptr[e + 1]; ++j) {
                    const auto [t, o] = parseExtId(idmap.ext_of_int(csr.edge_nodes[j]));
                    const auto bit = bizIdOf.find(nodeKey(t, o));
                    const int64_t bizId = (bit == bizIdOf.end()) ? (int64_t)o : bit->second;
                    const auto vit = inventory.find(bizId);
                    const double inv = (vit == inventory.end()) ? 0.0 : vit->second;
                    if (!(inv > 0.0)) {
                        kitting = false;
                        missing.push_back(std::to_string(bizId));
                    }
                }
            }
        }
        std::string missingStr = "[";
        for (size_t i = 0; i < missing.size(); ++i) {
            if (i) missingStr += ",";
            missingStr += missing[i];
        }
        missingStr += "]";
        stationVector->setValue<int64_t>(0, st.bizId);
        noSupplierVector->setValue<bool>(0, noSupplier);
        kittingVector->setValue<bool>(0, kitting);
        missingVector->copyFromValue(0, Value::createValue(missingStr));
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<HyperKittingBoolBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

} // namespace

function_set HyperKittingBoolFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::ANY, LogicalTypeID::ANY,
            LogicalTypeID::STRING, LogicalTypeID::INT64});
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
