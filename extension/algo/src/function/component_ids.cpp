#include "function/component_ids.h"

#include "binder/binder.h"
#include "function/algo_function.h"
#include "function/gds/gds_utils.h"
#include "function/table/bind_input.h"
#include "processor/execution_context.h"
#include "transaction/transaction.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::processor;
using namespace lbug::storage;
using namespace lbug::graph;
using namespace lbug::function;

namespace lbug {
namespace algo_extension {

OffsetManager::OffsetManager(const table_id_map_t<offset_t>& maxOffsetMap) {
    std::vector<table_id_t> tableIDVector;
    for (auto [tableID, maxOffset] : maxOffsetMap) {
        tableIDVector.push_back(tableID);
    }
    std::sort(tableIDVector.begin(), tableIDVector.end());
    auto offset = 0u;
    for (auto tableID : tableIDVector) {
        tableIDToStartOffset.insert({tableID, offset});
        offset += maxOffsetMap.at(tableID);
    }
}

ComponentIDs ComponentIDs::getSequenceComponentIDs(const table_id_map_t<offset_t>& maxOffsetMap,
    const OffsetManager& offsetManager, storage::MemoryManager* mm) {
    auto result = ComponentIDs();
    for (auto [tableID, maxOffset] : maxOffsetMap) {
        result.denseObjects.allocate(tableID, maxOffset, mm);
        result.pinTableID(tableID);
        auto startOffset = offsetManager.getStartOffset(tableID);
        for (auto i = 0u; i < maxOffset; i++) {
            result.setComponentID(i, startOffset + i);
        }
    }
    return result;
}

ComponentIDs ComponentIDs::getUnvisitedComponentIDs(const table_id_map_t<offset_t>& maxOffsetMap,
    storage::MemoryManager* mm) {
    auto result = ComponentIDs();
    for (auto [tableID, maxOffset] : maxOffsetMap) {
        result.denseObjects.allocate(tableID, maxOffset, mm);
        result.pinTableID(tableID);
        for (auto i = 0u; i < maxOffset; i++) {
            result.setComponentID(i, INVALID_COMPONENT_ID);
        }
    }
    return result;
}

bool ComponentIDsPair::update(offset_t boundOffset, offset_t nbrOffset) {
    auto boundValue = curData[boundOffset].load(std::memory_order_relaxed);
    auto tmp = nextData[nbrOffset].load(std::memory_order_relaxed);
    while (tmp > boundValue) {
        if (nextData[nbrOffset].compare_exchange_strong(tmp, boundValue)) {
            return true;
        }
    }
    return false;
}

void ComponentIDsOutputVertexCompute::vertexCompute(offset_t startOffset, offset_t endOffset,
    table_id_t tableID) {
    for (auto i = startOffset; i < endOffset; ++i) {
        if (skip(i)) {
            continue;
        }
        auto nodeID = nodeID_t{i, tableID};
        nodeIDVector->setValue<nodeID_t>(0, nodeID);
        componentIDVector->setValue<uint64_t>(0, componentIDs.getComponentID(i));
        localFT->append(vectors);
    }
}

// --- COMPONENT_IDS table function (mirrors WCC; runs the frontier BFS to convergence) ---

static constexpr char COMPONENT_ID_COLUMN_NAME[] = "component_id";

class ComponentIDsAuxiliaryState : public GDSAuxiliaryState {
public:
    explicit ComponentIDsAuxiliaryState(ComponentIDsPair& componentIDsPair)
        : componentIDsPair{componentIDsPair} {}

    void beginFrontierCompute(table_id_t fromTableID, table_id_t toTableID) override {
        componentIDsPair.pinCurTableID(fromTableID);
        componentIDsPair.pinNextTableID(toTableID);
    }

    void switchToDense(ExecutionContext*, Graph*) override {}

private:
    ComponentIDsPair& componentIDsPair;
};

class ComponentIDsEdgeCompute : public EdgeCompute {
public:
    explicit ComponentIDsEdgeCompute(ComponentIDsPair& componentIDsPair)
        : componentIDsPair{componentIDsPair} {}

    std::vector<nodeID_t> edgeCompute(nodeID_t boundNodeID, NbrScanState::Chunk& chunk,
        bool) override {
        std::vector<nodeID_t> result;
        chunk.forEach([&](auto neighbors, auto, auto i) {
            auto nbrNodeID = neighbors[i];
            if (componentIDsPair.update(boundNodeID.offset, nbrNodeID.offset)) {
                result.push_back(nbrNodeID);
            }
        });
        return result;
    }

    std::unique_ptr<EdgeCompute> copy() override {
        return std::make_unique<ComponentIDsEdgeCompute>(componentIDsPair);
    }

private:
    ComponentIDsPair& componentIDsPair;
};

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto currentFrontier = DenseFrontier::getUnvisitedFrontier(input.context, graph);
    auto nextFrontier =
        DenseFrontier::getVisitedFrontier(input.context, graph, sharedState->getGraphNodeMaskMap());
    auto frontierPair =
        std::make_unique<DenseFrontierPair>(std::move(currentFrontier), std::move(nextFrontier));
    frontierPair->setActiveNodesForNextIter();
    auto maxOffsetMap = graph->getMaxOffsetMap(transaction::Transaction::Get(*clientContext));
    auto offsetManager = OffsetManager(maxOffsetMap);
    auto mm = MemoryManager::Get(*clientContext);
    auto componentIDs = ComponentIDs::getSequenceComponentIDs(maxOffsetMap, offsetManager, mm);
    auto componentIDsPair = ComponentIDsPair(componentIDs);
    auto auxiliaryState = std::make_unique<ComponentIDsAuxiliaryState>(componentIDsPair);
    auto edgeCompute = std::make_unique<ComponentIDsEdgeCompute>(componentIDsPair);
    auto vertexCompute =
        std::make_unique<ComponentIDsOutputVertexCompute>(mm, sharedState, componentIDs);
    auto computeState =
        GDSComputeState(std::move(frontierPair), std::move(edgeCompute), std::move(auxiliaryState));
    // No iteration cap: keep labeling until the frontier is empty (converged).
    GDSUtils::runAlgorithmEdgeCompute(input.context, computeState, graph, ExtendDirection::BOTH,
        std::numeric_limits<uint16_t>::max());
    GDSUtils::runVertexCompute(input.context, GDSDensityState::DENSE, graph, *vertexCompute);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(COMPONENT_ID_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<GDSBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput});
}

function_set ComponentIDsFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    func->bindFunc = bindFunc;
    func->tableFunc = tableFunc;
    func->initSharedStateFunc = GDSFunction::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    func->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    func->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
