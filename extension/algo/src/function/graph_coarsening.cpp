// GRAPH_COARSENING - greedy matching coarsening of an undirected graph.
// P2-04 (NASH×GNN P2 batch): 粗化博弈 → Mean Field 近似; Hierarchical GNN.
// 对标 NetworKit `coarsening::MatchingCoarsening`.
//
//   CALL graph_coarsening('g', strategy := 'matching')
//     YIELD super_node, members, weight
//
// strategy='matching': scan nodes 0..n-1; each unmatched node pairs with its
// first unmatched neighbor (offsets ascending), an unmatched tail stays a
// singleton. strategy='partition': group by label_propagation communities.
// One row per super node:
//   super_node = smallest node offset in the group
//   members    = the node offsets in the group (LIST<INT64>)
//   weight     = number of undirected edges fully inside the group
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "binder/expression/expression_util.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
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

static constexpr char SUPER_NODE_COLUMN_NAME[] = "super_node";
static constexpr char MEMBERS_COLUMN_NAME[] = "members";
static constexpr char WEIGHT_COLUMN_NAME[] = "weight";

static constexpr char DEFAULT_STRATEGY[] = "matching";

struct GraphCoarseningBindData final : public GDSBindData {
    bool usePartition;

    GraphCoarseningBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        bool usePartition)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          usePartition{usePartition} {}

    GraphCoarseningBindData(const GraphCoarseningBindData& other)
        : GDSBindData{other}, usePartition{other.usePartition} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GraphCoarseningBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    std::string strategy = DEFAULT_STRATEGY;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "strategy") {
            strategy = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (strategy != "matching" && strategy != "partition") {
        throw BinderException{"GRAPH_COARSENING strategy must be 'matching' or 'partition'."};
    }

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SUPER_NODE_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(
        input->binder->createVariable(MEMBERS_COLUMN_NAME, LogicalType::LIST(LogicalType::INT64())));
    columns.push_back(input->binder->createVariable(WEIGHT_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<GraphCoarseningBindData>(std::move(columns), std::move(graphEntry),
        strategy == "partition");
}

// Label propagation communities (same core as LABEL_PROPAGATION, update_seed).
void computeLpa(const std::vector<std::vector<uint64_t>>& adj, std::vector<uint64_t>& labels) {
    const auto n = adj.size();
    labels.resize(n);
    for (uint64_t v = 0; v < n; ++v) {
        labels[v] = v;
    }
    if (n <= 1) {
        return;
    }
    std::vector<uint64_t> previous = labels;
    std::vector<uint64_t> twoAgo = labels;
    std::vector<uint64_t> freq(n, 0);
    const int64_t maxIterations = 100;
    for (int64_t iter = 0; iter < maxIterations; ++iter) {
        twoAgo = previous;
        previous = labels;
        bool changed = false;
        for (uint64_t v = 0; v < n; ++v) {
            if (adj[v].empty()) {
                continue;
            }
            freq[labels[v]] = 0;
            for (const auto u : adj[v]) {
                freq[labels[u]] = 0;
            }
            for (const auto u : adj[v]) {
                freq[labels[u]]++;
            }
            uint64_t bestLabel = labels[v];
            uint64_t bestCount = 0;
            for (const auto u : adj[v]) {
                const auto l = labels[u];
                const auto c = freq[l];
                if (c > bestCount || (c == bestCount && l < bestLabel)) {
                    bestCount = c;
                    bestLabel = l;
                }
            }
            if (bestLabel == labels[v]) {
                continue;
            }
            labels[v] = bestLabel;
            changed = true;
        }
        if (!changed || labels == twoAgo) {
            break;
        }
    }
    std::vector<int64_t> renumber(n, -1);
    std::vector<uint64_t> out(n, 0);
    uint64_t nextId = 0;
    for (uint64_t v = 0; v < n; ++v) {
        if (renumber[labels[v]] < 0) {
            renumber[labels[v]] = static_cast<int64_t>(nextId++);
        }
        out[v] = static_cast<uint64_t>(renumber[labels[v]]);
    }
    labels = std::move(out);
}

// Group the nodes (per-node group id) into ascending lists of offsets.
std::vector<std::vector<uint64_t>> groupNodes(uint64_t n, const std::vector<uint64_t>& groupId) {
    uint64_t maxId = 0;
    for (const auto g : groupId) {
        maxId = std::max(maxId, g + 1);
    }
    std::vector<std::vector<uint64_t>> groups(maxId);
    for (uint64_t v = 0; v < n; ++v) {
        groups[groupId[v]].push_back(v);
    }
    // Sort each group ascending and order groups by their smallest member.
    for (auto& g : groups) {
        std::sort(g.begin(), g.end());
    }
    std::sort(groups.begin(), groups.end(), [](const auto& a, const auto& b) {
        return a.empty() ? false : (b.empty() ? true : a.front() < b.front());
    });
    groups.erase(std::remove_if(groups.begin(), groups.end(),
                    [](const auto& g) { return g.empty(); }),
        groups.end());
    return groups;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GraphCoarseningBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"GRAPH_COARSENING currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t w = 0; w < adj.size(); ++w) {
        auto& nbrs = adj[w];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), w), nbrs.end());
    }

    std::vector<std::vector<uint64_t>> groups;
    if (bindData->usePartition) {
        std::vector<uint64_t> labels;
        computeLpa(adj, labels);
        groups = groupNodes(numNodes, labels);
    } else {
        // Greedy matching: pair each unmatched node with its first unmatched
        // neighbor; unmatched tail stays a singleton group.
        std::vector<uint8_t> matched(numNodes, 0);
        for (uint64_t v = 0; v < numNodes; ++v) {
            if (matched[v]) {
                continue;
            }
            uint64_t partner = UINT64_MAX;
            for (const auto u : adj[v]) {
                if (!matched[u]) {
                    partner = u;
                    break;
                }
            }
            if (partner != UINT64_MAX) {
                matched[v] = 1;
                matched[partner] = 1;
                const auto lo = std::min(v, partner);
                const auto hi = std::max(v, partner);
                groups.push_back({lo, hi});
            } else {
                matched[v] = 1;
                groups.push_back({v});
            }
        }
        std::sort(groups.begin(), groups.end(),
            [](const auto& a, const auto& b) { return a.front() < b.front(); });
    }

    auto superVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    superVector->state = DataChunkState::getSingleValueDataChunkState();
    auto membersVector =
        std::make_unique<ValueVector>(LogicalType::LIST(LogicalType::INT64()), mm);
    membersVector->state = DataChunkState::getSingleValueDataChunkState();
    auto weightVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    weightVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{superVector.get(), membersVector.get(), weightVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (const auto& g : groups) {
        // Internal edge weight: undirected edges with both endpoints in g.
        double internalEdges = 0.0;
        for (const auto a : g) {
            for (const auto b : adj[a]) {
                if (b > a && std::binary_search(g.begin(), g.end(), b)) {
                    internalEdges += 1.0;
                }
            }
        }
        superVector->setValue<int64_t>(0, static_cast<int64_t>(g.front()));
        const auto listEntry = ListVector::addList(membersVector.get(), g.size());
        membersVector->setValue<list_entry_t>(0, listEntry);
        auto dataVector = ListVector::getDataVector(membersVector.get());
        for (size_t i = 0; i < g.size(); ++i) {
            dataVector->setValue<int64_t>(listEntry.offset + i, static_cast<int64_t>(g[i]));
        }
        weightVector->setValue<double>(0, internalEdges);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Edge output (no node column): bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set GraphCoarseningFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
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

} // namespace algo_extension
} // namespace lbug
