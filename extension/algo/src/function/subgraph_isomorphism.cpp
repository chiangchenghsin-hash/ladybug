// SUBGRAPH_ISOMORPHISM — VF2++ subgraph isomorphism (pattern DAG vs arbitrary directed target).
// Dual-node GDS output: `RETURN match_id, pattern_node.<prop>, target_node.<prop>` resolves via
// two property scans + two hash joins (see getLogicalPlan below).
//
// Semantics: non-induced subgraph isomorphism (monomorphism). Every pattern edge must map to a
// target edge; the target may hold extra edges — the right fit for workflow-pattern matching
// (ComfyUI templates, NX construction trees).
#include "binder/binder.h"
#include "binder/expression/node_expression.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
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

using namespace lbug::catalog;
using namespace lbug::common;
using namespace lbug::binder;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::planner;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char MATCH_ID_COLUMN_NAME[] = "match_id";
static constexpr char PATTERN_NODE_COLUMN_NAME[] = "pattern_node";
static constexpr char TARGET_NODE_COLUMN_NAME[] = "target_node";

// Maximum number of complete matches to return (0 = unlimited). Guards against the
// exponential worst case of subgraph isomorphism on large target graphs.
struct MaxMatches {
    static constexpr const char* NAME = "max_matches";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::INT64;
    static constexpr int64_t DEFAULT_VALUE = 0;

    static void validate(int64_t maxMatches) {
        if (maxMatches < 0) {
            throw BinderException{"max_matches must be non-negative (0 = unlimited)."};
        }
    }
};

struct SubgraphIsomorphismOptionalParams final : public function::OptionalParams {
    function::OptionalParam<MaxMatches> maxMatches;

    SubgraphIsomorphismOptionalParams() = default;

    explicit SubgraphIsomorphismOptionalParams(const binder::expression_vector& optionalParams) {
        for (auto& optionalParam : optionalParams) {
            auto paramName = StringUtils::getLower(optionalParam->getAlias());
            if (paramName == MaxMatches::NAME) {
                maxMatches = function::OptionalParam<MaxMatches>(optionalParam);
            } else {
                throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
            }
        }
    }

    void evaluateParams(main::ClientContext* context) override {
        maxMatches.evaluateParam(context);
    }

    std::unique_ptr<function::OptionalParams> copy() override {
        auto result = std::make_unique<SubgraphIsomorphismOptionalParams>();
        result->maxMatches = maxMatches;
        return result;
    }
};

// GDSBindData holds the target graph; patternGraphEntry holds the pattern (DAG) graph.
struct SubgraphIsomorphismBindData final : public GDSBindData {
    graph::NativeGraphEntry patternGraphEntry;

    SubgraphIsomorphismBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        graph::NativeGraphEntry patternGraphEntry, expression_vector output,
        std::unique_ptr<SubgraphIsomorphismOptionalParams> optionalParams)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(output)},
          patternGraphEntry{std::move(patternGraphEntry)} {
        this->optionalParams = std::move(optionalParams);
    }

    SubgraphIsomorphismBindData(const SubgraphIsomorphismBindData& other)
        : GDSBindData{other}, patternGraphEntry{other.patternGraphEntry.copy()} {
        this->optionalParams = other.optionalParams == nullptr ? nullptr
                                                               : other.optionalParams->copy();
    }

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SubgraphIsomorphismBindData>(*this);
    }
};

// Dual-graph shared state: the stock GDSFuncSharedState holds a single graph only.
struct SubgraphIsomorphismSharedState : public TableFuncSharedState {
    std::unique_ptr<graph::Graph> patternGraph;
    std::unique_ptr<graph::Graph> targetGraph;
    processor::FactorizedTablePool factorizedTablePool;

    SubgraphIsomorphismSharedState(std::unique_ptr<graph::Graph> patternGraph,
        std::unique_ptr<graph::Graph> targetGraph,
        std::shared_ptr<processor::FactorizedTable> fTable)
        : TableFuncSharedState{}, patternGraph{std::move(patternGraph)},
          targetGraph{std::move(targetGraph)}, factorizedTablePool{std::move(fTable)} {}
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto patternGraphName = input->getLiteralVal<std::string>(0);
    auto targetGraphName = input->getLiteralVal<std::string>(1);
    auto patternGraphEntry = GDSFunction::bindGraphEntry(*context, patternGraphName);
    auto targetGraphEntry = GDSFunction::bindGraphEntry(*context, targetGraphName);

    auto patternNode = GDSFunction::bindNodeOutput(*input, patternGraphEntry.getNodeEntries(),
        PATTERN_NODE_COLUMN_NAME, 0 /*yieldVariableIdx*/);
    auto targetNode = GDSFunction::bindNodeOutput(*input, targetGraphEntry.getNodeEntries(),
        TARGET_NODE_COLUMN_NAME, 1 /*yieldVariableIdx*/);

    expression_vector columns;
    columns.push_back(input->binder->createVariable(MATCH_ID_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(patternNode->constCast<NodeExpression>().getInternalID());
    columns.push_back(targetNode->constCast<NodeExpression>().getInternalID());

    expression_vector output;
    output.push_back(std::move(patternNode));
    output.push_back(std::move(targetNode));
    return std::make_unique<SubgraphIsomorphismBindData>(std::move(columns),
        std::move(targetGraphEntry), std::move(patternGraphEntry), std::move(output),
        std::make_unique<SubgraphIsomorphismOptionalParams>(input->optionalParamsLegacy));
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<SubgraphIsomorphismBindData>();
    auto patternGraph = std::make_unique<OnDiskGraph>(input.context->clientContext,
        bindData->patternGraphEntry.copy());
    auto targetGraph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<SubgraphIsomorphismSharedState>(std::move(patternGraph),
        std::move(targetGraph), bindData->getResultTable());
}

// Dual-node variant of GDSFunction::getLogicalPlan: plan a property scan + hash join for BOTH
// node outputs (pattern_node and target_node), so `RETURN pattern_node.type, target_node.name`
// resolves through the standard node-property machinery.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<SubgraphIsomorphismBindData>();
    auto op =
        std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);

    // Pass 1: pattern_node property scan + hash join on its internalID.
    auto patternNode = bindData->output[0]->ptrCast<NodeExpression>();
    DASSERT(patternNode != nullptr);
    planner->getCardinliatyEstimatorUnsafe().init(*patternNode);
    auto patternScanPlan = planner->getNodePropertyScanPlan(*patternNode);
    if (!patternScanPlan.isEmpty()) {
        planner->appendHashJoin({patternNode->getInternalID()}, JoinType::INNER, plan,
            patternScanPlan, plan);
    }

    // Pass 2: target_node property scan + hash join on its internalID.
    auto targetNode = bindData->output[1]->ptrCast<NodeExpression>();
    DASSERT(targetNode != nullptr);
    planner->getCardinliatyEstimatorUnsafe().init(*targetNode);
    auto targetScanPlan = planner->getNodePropertyScanPlan(*targetNode);
    if (!targetScanPlan.isEmpty()) {
        planner->appendHashJoin({targetNode->getInternalID()}, JoinType::INNER, plan,
            targetScanPlan, plan);
    }
}

// Materializes the projected graph's directed adjacency as a DirectedCSR via scanFwd
// (out-edges). In-edges are derived by the CSR builder from the same edge set, which is
// equivalent to scanBwd since the graph is a directed store.
static DirectedCSR buildDirectedCSRFromGraph(table_id_t tableID, offset_t numNodes,
    Graph* graph) {
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
    auto csr = buildDirectedCSR(numNodes, edges);
    detectDAG(csr); // pattern-side DAG detection powers the node ordering + depth filter
    return csr;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<SubgraphIsomorphismSharedState>();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<SubgraphIsomorphismBindData>();
    auto& config = bindData->optionalParams->constCast<SubgraphIsomorphismOptionalParams>();
    const auto maxMatches = static_cast<uint64_t>(config.maxMatches.getParamVal());

    const auto patternMaxOffset = sharedState->patternGraph->getMaxOffsetMap(transaction);
    const auto targetMaxOffset = sharedState->targetGraph->getMaxOffsetMap(transaction);
    if (patternMaxOffset.size() != 1 || targetMaxOffset.size() != 1) {
        throw BinderException{
            "SUBGRAPH_ISOMORPHISM currently supports single-node-table graphs only."};
    }
    const auto patternTableID = patternMaxOffset.begin()->first;
    const auto patternNumNodes = patternMaxOffset.begin()->second;
    const auto targetTableID = targetMaxOffset.begin()->first;
    const auto targetNumNodes = targetMaxOffset.begin()->second;

    // 1. Projected graphs -> directed CSRs.
    auto patternCSR =
        buildDirectedCSRFromGraph(patternTableID, patternNumNodes, sharedState->patternGraph.get());
    auto targetCSR =
        buildDirectedCSRFromGraph(targetTableID, targetNumNodes, sharedState->targetGraph.get());

    // 2. VF2++ search (pure C++, no engine dependencies).
    std::vector<std::vector<uint64_t>> matches;
    vf2ppAllMatches(patternCSR, targetCSR, maxMatches, matches);

    // 3. Stream (match_id, pattern_node, target_node) rows.
    auto matchIdVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    matchIdVector->state = DataChunkState::getSingleValueDataChunkState();
    auto patternVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    patternVector->state = DataChunkState::getSingleValueDataChunkState();
    auto targetVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    targetVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{
        matchIdVector.get(), patternVector.get(), targetVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    int64_t matchId = 0;
    for (const auto& match : matches) {
        for (uint64_t pu = 0; pu < match.size(); ++pu) {
            matchIdVector->setValue<int64_t>(0, matchId);
            patternVector->setValue<nodeID_t>(0, nodeID_t{pu, patternTableID});
            targetVector->setValue<nodeID_t>(0, nodeID_t{match[pu], targetTableID});
            localFT->append(vectors);
        }
        ++matchId;
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set SubgraphIsomorphismFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(SubgraphIsomorphismFunction::name,
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
