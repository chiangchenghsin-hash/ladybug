// SIR_SIMULATOR - stochastic Susceptible-Infected-Recovered propagation on an
// undirected graph. P2-15 (NASH×GNN P2 batch): 策略/行为传染动力学; 完全图高 p
// 全感染康复, 孤立点恒 Susceptible。对标 NetworKit `dynamics::SIRSimulator`.
//
//   CALL sir_simulator('S', num_seeds, p_infect, p_recover, steps, seed := 42)
//     YIELD node, step, final_state, infection_time
//
// Determinism: the RNG is a per-call `std::mt19937(seed)` (default 42) and the
// initial infected set is the `num_seeds` lowest node offsets, so runs are
// reproducible. One row per node:
//   step / infection_time = the round the node was infected (-1 if never;
//                           seeds are infected at round 0)
//   final_state           = 'S' | 'I' | 'R' at termination
// 无向无权; O(steps · m).
#include "binder/binder.h"
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
#include "processor/execution_context.h"
#include "storage/buffer_manager/memory_manager.h"
#include "transaction/transaction.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char STEP_COLUMN_NAME[] = "step";
static constexpr char FINAL_STATE_COLUMN_NAME[] = "final_state";
static constexpr char INFECTION_TIME_COLUMN_NAME[] = "infection_time";

static constexpr int64_t DEFAULT_SEED = 42;

enum class SirState : uint8_t { Susceptible = 0, Infected = 1, Recovered = 2 };

struct SirBindData final : public GDSBindData {
    int64_t numSeeds;
    double pInfect;
    double pRecover;
    int64_t steps;
    uint64_t seed;

    SirBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t numSeeds, double pInfect, double pRecover,
        int64_t steps, uint64_t seed)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          numSeeds{numSeeds}, pInfect{pInfect}, pRecover{pRecover}, steps{steps}, seed{seed} {}

    SirBindData(const SirBindData& other)
        : GDSBindData{other}, numSeeds{other.numSeeds}, pInfect{other.pInfect},
          pRecover{other.pRecover}, steps{other.steps}, seed{other.seed} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SirBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    const auto numSeeds = input->getLiteralVal<int64_t>(1);
    // Note: getLiteralVal has no double specialization; evaluate the position
    // parameter as a literal expression.
    const auto pInfect = ExpressionUtil::evaluateLiteral<double>(context, input->params[2],
        LogicalType::DOUBLE());
    const auto pRecover = ExpressionUtil::evaluateLiteral<double>(context, input->params[3],
        LogicalType::DOUBLE());
    const auto steps = input->getLiteralVal<int64_t>(4);

    int64_t seed = DEFAULT_SEED;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "seed") {
            seed = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (numSeeds < 1) {
        throw BinderException{"SIR_SIMULATOR num_seeds must be >= 1."};
    }
    if (pInfect < 0.0 || pInfect > 1.0 || pRecover < 0.0 || pRecover > 1.0) {
        throw BinderException{"SIR_SIMULATOR p_infect / p_recover must be in [0, 1]."};
    }
    if (steps < 0) {
        throw BinderException{"SIR_SIMULATOR steps must be >= 0."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(STEP_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(
        input->binder->createVariable(FINAL_STATE_COLUMN_NAME, LogicalType::STRING()));
    columns.push_back(input->binder->createVariable(INFECTION_TIME_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<SirBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, numSeeds, pInfect, pRecover, steps, seed);
}

// Runs one deterministic SIR trace. Seeds = the `numSeeds` lowest offsets.
// On output: `finalState[i]` and `infectionTime[i]` (round of infection, -1 if
// never infected; seeds infected at round 0).
void runSir(const std::vector<std::vector<uint64_t>>& adj, int64_t numSeeds, double pInfect,
    double pRecover, int64_t steps, uint64_t seed, std::vector<SirState>& finalState,
    std::vector<int64_t>& infectionTime) {
    const auto n = adj.size();
    finalState.assign(n, SirState::Susceptible);
    infectionTime.assign(n, -1);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> draw(0.0, 1.0);

    std::vector<uint64_t> infected;
    const auto actualSeeds = std::min<uint64_t>(static_cast<uint64_t>(numSeeds), n);
    for (uint64_t i = 0; i < actualSeeds; ++i) {
        finalState[i] = SirState::Infected;
        infectionTime[i] = 0;
        infected.push_back(i);
    }

    for (int64_t t = 1; t <= steps; ++t) {
        const auto current = infected; // snapshot so round-1 infections wait a round
        infected.clear();
        for (const auto u : current) {
            // Infect first, then possibly recover this round.
            for (const auto v : adj[u]) {
                if (finalState[v] == SirState::Susceptible && draw(rng) < pInfect) {
                    finalState[v] = SirState::Infected;
                    infectionTime[v] = t;
                    infected.push_back(v);
                }
            }
            if (draw(rng) < pRecover) {
                finalState[u] = SirState::Recovered;
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
    auto bindData = input.bindData->constPtrCast<SirBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"SIR_SIMULATOR currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    std::vector<SirState> finalState;
    std::vector<int64_t> infectionTime;
    runSir(adj, bindData->numSeeds, bindData->pInfect, bindData->pRecover, bindData->steps,
        bindData->seed, finalState, infectionTime);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto stepVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    stepVector->state = DataChunkState::getSingleValueDataChunkState();
    auto stateVector = std::make_unique<ValueVector>(LogicalType::STRING(), mm);
    stateVector->state = DataChunkState::getSingleValueDataChunkState();
    auto timeVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    timeVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), stepVector.get(), stateVector.get(),
        timeVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        stepVector->setValue<int64_t>(0, infectionTime[i]);
        const char* stateStr = finalState[i] == SirState::Susceptible
                                   ? "S"
                                   : (finalState[i] == SirState::Infected ? "I" : "R");
        stateVector->setValue(0, std::string_view(stateStr));
        timeVector->setValue<int64_t>(0, infectionTime[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set SirSimulatorFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64, LogicalTypeID::DOUBLE,
            LogicalTypeID::DOUBLE, LogicalTypeID::INT64});
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
