// GROUP_BETWEENNESS + GROUP_CLOSENESS - greedy group centrality (undirected,
// unweighted). P1-07 (NASH×GNN P1 batch): 「最小干预集」— 调整哪些玩家子集能
// 最大改变全局均衡; GNN Pooling/Readout 选代表性节点 (Anchor Set)。
//
// Both are NP-hard to optimize exactly, so a greedy is used: at each step the
// node maximizing the exact marginal gain is added (O(k n (n+m))).
//
//   CALL group_betweenness('S', 3)                        -> (node, group_score, rank_in_group)
//   CALL group_betweenness('S', 3, output := 'group_summary')
//                                                         -> (group_id, group_score, member_count, members)
//   CALL group_closeness('S', 3, variant := 'harmonic')   -> (node, group_score)
//
// group_score is the CUMULATIVE group score after adding that member (greedy
// trace). Group betweenness uses the same normalization as BETWEENNESS
// (star center = 1); group closeness follows NetworkX group_closeness_centrality
// (standard) / harmonic group closeness (default, handles disconnected graphs).
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "binder/expression/expression_util.h"
#include "common/directed_csr.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/types/value/value.h"
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

#include <cstdint>
#include <string>
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

namespace {

// ---- shared greedy infrastructure -------------------------------------------------

// Exact group-betweenness score of `group` and its per-node marginal gains.
// On output: `marginal[v]` = gain of adding v (for v not in group).
// Normalization matches BETWEENNESS (NetworkX normalized, undirected).
struct GroupBetweennessCore {
    const std::vector<std::vector<uint64_t>>& adj;
    const std::vector<uint8_t>& inGroup;
    uint64_t n;

    // Per-source scratch.
    std::vector<int64_t> dist;
    std::vector<double> sigma;
    std::vector<double> h;
    std::vector<double> g;
    std::vector<std::vector<uint64_t>> pred;
    std::vector<uint64_t> queue;

    GroupBetweennessCore(const std::vector<std::vector<uint64_t>>& a,
        const std::vector<uint8_t>& grp)
        : adj{a}, inGroup{grp}, n{a.size()}, dist(n), sigma(n), h(n), g(n), pred(n) {
        queue.reserve(n);
    }

    // Runs one all-source pass, adding each source's contribution to `marginal`
    // and to `totalScore` (the exact B(group)).
    void accumulate(std::vector<double>& marginal, double& totalScore) {
        for (uint64_t s = 0; s < n; ++s) {
            if (inGroup[s]) {
                continue;
            }
            std::fill(dist.begin(), dist.end(), -1);
            std::fill(sigma.begin(), sigma.end(), 0.0);
            std::fill(h.begin(), h.end(), 0.0);
            std::fill(g.begin(), g.end(), 0.0);
            for (auto& p : pred) {
                p.clear();
            }
            dist[s] = 0;
            sigma[s] = 1.0;
            queue.clear();
            queue.push_back(s);
            for (uint64_t head = 0; head < queue.size(); ++head) {
                const auto u = queue[head];
                for (const auto v : adj[u]) {
                    if (dist[v] == -1) {
                        dist[v] = dist[u] + 1;
                        sigma[v] = sigma[u];
                        pred[v].push_back(u);
                        queue.push_back(v);
                    } else if (dist[v] == dist[u] + 1) {
                        sigma[v] += sigma[u];
                        pred[v].push_back(u);
                    }
                }
            }
            // Forward: h[t] = # s->t shortest paths with an S-member strictly
            // between s and t.
            for (const auto t : queue) {
                if (t == s) {
                    continue;
                }
                double sum = 0.0;
                for (const auto u : pred[t]) {
                    sum += inGroup[u] ? sigma[u] : h[u];
                }
                h[t] = sum;
            }
            // Group score contribution: sum over t not in group, t != s.
            for (const auto t : queue) {
                if (t == s || inGroup[t]) {
                    continue;
                }
                totalScore += h[t] / sigma[t];
            }
            // Reverse: g[v] = sum over descendants t (t != v, t not in group,
            // t != s) of sigma_vt / sigma_s(t). Accumulate into predecessors.
            for (size_t i = queue.size(); i-- > 0;) {
                const auto v = queue[i];
                for (const auto u : pred[v]) {
                    double extra = (v != s && !inGroup[v]) ? 1.0 / sigma[v] : 0.0;
                    g[u] += g[v] + extra;
                }
                // Marginal gain of candidate v: paths whose first S'-hit is v.
                if (v != s && !inGroup[v]) {
                    const double avoidS = sigma[v] - h[v];
                    if (avoidS > 0.0) {
                        marginal[v] += avoidS * g[v];
                    }
                }
            }
        }
    }

    // Normalize both the total score and each marginal gain.
    static double normalize(double raw, uint64_t n) {
        if (n < 3) {
            return 0.0;
        }
        return raw / (static_cast<double>(n - 1) * static_cast<double>(n - 2));
    }
};

// Greedy group-betweenness: returns members in selection order and the
// cumulative group score after each addition.
void greedyGroupBetweenness(const std::vector<std::vector<uint64_t>>& adj, uint64_t k,
    std::vector<uint64_t>& members, std::vector<double>& scores) {
    const auto n = adj.size();
    members.clear();
    scores.clear();
    if (k > n) {
        k = n;
    }
    std::vector<uint8_t> inGroup(n, 0);
    GroupBetweennessCore core{adj, inGroup};
    std::vector<double> marginal(n, 0.0);
    double totalRaw = 0.0; // exact raw B(S), S starts empty
    for (uint64_t step = 0; step < k; ++step) {
        std::fill(marginal.begin(), marginal.end(), 0.0);
        double passTotal = 0.0;
        core.accumulate(marginal, passTotal);
        totalRaw = passTotal;

        // Pick the best candidate (ties -> smallest offset).
        uint64_t best = UINT64_MAX;
        double bestGain = -1.0;
        for (uint64_t v = 0; v < n; ++v) {
            if (inGroup[v]) {
                continue;
            }
            if (best == UINT64_MAX || marginal[v] > bestGain) {
                best = v;
                bestGain = marginal[v];
            }
        }
        if (best == UINT64_MAX) {
            break;
        }
        inGroup[best] = 1;
        members.push_back(best);
        // The marginal gain is exactly B(S u {best}) - B(S).
        totalRaw += marginal[best];
        scores.push_back(GroupBetweennessCore::normalize(totalRaw, n));
    }
}

// Group closeness (harmonic/standard). Returns members and cumulative scores.
void greedyGroupCloseness(const std::vector<std::vector<uint64_t>>& adj, uint64_t k,
    bool harmonic, std::vector<uint64_t>& members, std::vector<double>& scores) {
    const auto n = adj.size();
    members.clear();
    scores.clear();
    if (k > n) {
        k = n;
    }
    if (n == 0) {
        return;
    }
    std::vector<uint8_t> inGroup(n, 0);
    std::vector<int64_t> dS(n, -1); // group distance, -1 = unreachable
    std::vector<int64_t> dist(n);
    std::vector<uint64_t> queue;
    queue.reserve(n);
    std::vector<int64_t> candidateDist(n);

    for (uint64_t step = 0; step < k; ++step) {
        uint64_t best = UINT64_MAX;
        double bestGain = -1.0;
        // For each candidate, BFS and evaluate the resulting score.
        for (uint64_t v = 0; v < n; ++v) {
            if (inGroup[v]) {
                continue;
            }
            std::fill(dist.begin(), dist.end(), -1);
            dist[v] = 0;
            queue.clear();
            queue.push_back(v);
            for (uint64_t head = 0; head < queue.size(); ++head) {
                const auto u = queue[head];
                for (const auto w : adj[u]) {
                    if (dist[w] == -1) {
                        dist[w] = dist[u] + 1;
                        queue.push_back(w);
                    }
                }
            }
            double gain = 0.0;
            if (harmonic) {
                for (uint64_t u = 0; u < n; ++u) {
                    if (inGroup[u] || u == v) {
                        continue;
                    }
                    int64_t dNew = dist[u];
                    if (dNew < 0 || (dS[u] >= 0 && dS[u] < dNew)) {
                        dNew = dS[u];
                    }
                    if (dNew > 0) {
                        const double before = (dS[u] > 0) ? 1.0 / dS[u] : 0.0;
                        gain += 1.0 / dNew - before;
                    }
                }
            } else {
                double sum = 0.0;
                uint64_t unreachable = 0;
                for (uint64_t u = 0; u < n; ++u) {
                    if (inGroup[u] || u == v) {
                        continue;
                    }
                    int64_t dNew = dist[u];
                    if (dNew < 0 || (dS[u] >= 0 && dS[u] < dNew)) {
                        dNew = dS[u];
                    }
                    if (dNew > 0) {
                        sum += static_cast<double>(dNew);
                    } else {
                        unreachable++;
                    }
                }
                // Higher score = lower total distance (standard closeness).
                gain = (sum > 0) ? -sum : 0.0;
                (void)unreachable;
            }
            if (best == UINT64_MAX || gain > bestGain) {
                best = v;
                bestGain = gain;
            }
        }
        if (best == UINT64_MAX) {
            break;
        }
        inGroup[best] = 1;
        members.push_back(best);
        // Update group distances with the chosen node's distances.
        std::fill(dist.begin(), dist.end(), -1);
        dist[best] = 0;
        queue.clear();
        queue.push_back(best);
        for (uint64_t head = 0; head < queue.size(); ++head) {
            const auto u = queue[head];
            for (const auto w : adj[u]) {
                if (dist[w] == -1) {
                    dist[w] = dist[u] + 1;
                    queue.push_back(w);
                }
            }
        }
        for (uint64_t u = 0; u < n; ++u) {
            if (dist[u] >= 0 && (dS[u] < 0 || dist[u] < dS[u])) {
                dS[u] = dist[u];
            }
        }
        // Cumulative score.
        double score = 0.0;
        if (harmonic) {
            for (uint64_t u = 0; u < n; ++u) {
                if (!inGroup[u] && dS[u] > 0) {
                    score += 1.0 / static_cast<double>(dS[u]);
                }
            }
        } else {
            double sum = 0.0;
            uint64_t unreachable = 0;
            for (uint64_t u = 0; u < n; ++u) {
                if (inGroup[u]) {
                    continue;
                }
                if (dS[u] > 0) {
                    sum += static_cast<double>(dS[u]);
                } else {
                    unreachable++;
                }
            }
            const double reachableCount = static_cast<double>(n - (step + 1) - unreachable);
            score = (sum > 0) ? reachableCount / sum : 0.0;
        }
        scores.push_back(score);
    }
}

} // namespace

// ---- GROUP_BETWEENNESS -------------------------------------------------------------

struct GroupBetweennessBindData final : public GDSBindData {
    int64_t k;
    bool summary;

    GroupBetweennessBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t k, bool summary)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          k{k}, summary{summary} {}

    GroupBetweennessBindData(const GroupBetweennessBindData& other)
        : GDSBindData{other}, k{other.k}, summary{other.summary} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GroupBetweennessBindData>(*this);
    }
};

static void parseCommonParams(main::ClientContext* context, const TableFuncBindInput* input,
    int64_t& k, std::string& mode, std::string& output) {
    if (input->params.size() >= 2) {
        k = input->getLiteralVal<int64_t>(1);
    }
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "k") {
            k = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else if (paramName == "mode") {
            mode = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else if (paramName == "output") {
            output = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (k <= 0) {
        throw BinderException{"GROUP_BETWEENNESS/GROUP_CLOSENESS require k >= 1."};
    }
}

static std::unique_ptr<TableFuncBindData> bindGroupBetweenness(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);

    int64_t k = -1;
    std::string mode = "greedy";
    std::string output = "members";
    parseCommonParams(context, input, k, mode, output);
    if (mode != "greedy") {
        throw BinderException{"GROUP_BETWEENNESS mode must be 'greedy' (only mode supported)."};
    }
    const bool summary = (output == "group_summary");
    if (!summary && output != "members") {
        throw BinderException{
            "GROUP_BETWEENNESS output must be 'members' or 'group_summary'."};
    }

    expression_vector columns;
    expression_vector nodeOutputs;
    if (summary) {
        columns.push_back(input->binder->createVariable("group_id", LogicalType::INT64()));
        columns.push_back(input->binder->createVariable("group_score", LogicalType::DOUBLE()));
        columns.push_back(input->binder->createVariable("member_count", LogicalType::INT64()));
        columns.push_back(
            input->binder->createVariable("members", LogicalType::LIST(LogicalType::INT64())));
    } else {
        auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());
        nodeOutputs = {nodeOutput};
        columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
        columns.push_back(input->binder->createVariable("group_score", LogicalType::DOUBLE()));
        columns.push_back(input->binder->createVariable("rank_in_group", LogicalType::INT64()));
    }
    return std::make_unique<GroupBetweennessBindData>(std::move(columns), std::move(graphEntry),
        std::move(nodeOutputs), k, summary);
}

static std::unique_ptr<TableFuncSharedState> initSharedState(
    const TableFuncInitSharedStateInput& input) {
    auto bindData = input.bindData->constPtrCast<GDSBindData>();
    auto graph =
        std::make_unique<OnDiskGraph>(input.context->clientContext, bindData->graphEntry.copy());
    return std::make_unique<GDSFuncSharedState>(bindData->getResultTable(), std::move(graph));
}

static offset_t tableFuncGroupBetweenness(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GroupBetweennessBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"GROUP_BETWEENNESS supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    std::vector<uint64_t> members;
    std::vector<double> scores;
    greedyGroupBetweenness(adj, static_cast<uint64_t>(bindData->k), members, scores);

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    if (bindData->summary) {
        auto groupID = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
        groupID->state = DataChunkState::getSingleValueDataChunkState();
        auto score = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
        score->state = DataChunkState::getSingleValueDataChunkState();
        auto count = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
        count->state = DataChunkState::getSingleValueDataChunkState();
        auto membersVec =
            std::make_unique<ValueVector>(LogicalType::LIST(LogicalType::INT64()), mm);
        membersVec->state = DataChunkState::getSingleValueDataChunkState();

        groupID->setValue<int64_t>(0, 0);
        score->setValue<double>(0, scores.empty() ? 0.0 : scores.back());
        count->setValue<int64_t>(0, static_cast<int64_t>(members.size()));
        auto listEntry = ListVector::addList(membersVec.get(), members.size());
        membersVec->setValue<list_entry_t>(0, listEntry);
        auto dataVector = ListVector::getDataVector(membersVec.get());
        for (size_t i = 0; i < members.size(); ++i) {
            dataVector->setValue<int64_t>(listEntry.offset + i,
                static_cast<int64_t>(members[i]));
        }
        std::vector<ValueVector*> vectors{groupID.get(), score.get(), count.get(),
            membersVec.get()};
        localFT->append(vectors);
    } else {
        auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
        nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
        auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
        scoreVector->state = DataChunkState::getSingleValueDataChunkState();
        auto rankVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
        rankVector->state = DataChunkState::getSingleValueDataChunkState();
        std::vector<ValueVector*> vectors{nodeIDVector.get(), scoreVector.get(),
            rankVector.get()};
        for (size_t i = 0; i < members.size(); ++i) {
            nodeIDVector->setValue<nodeID_t>(0,
                nodeID_t{static_cast<offset_t>(members[i]), tableID});
            scoreVector->setValue<double>(0, scores[i]);
            rankVector->setValue<int64_t>(0, static_cast<int64_t>(i + 1));
            localFT->append(vectors);
        }
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Group-betweenness has two output shapes chosen at bind time:
//   members        -> node column (NodeExpression) + group_score + rank_in_group
//   group_summary  -> scalar-only columns (group_id, group_score, member_count, members)
// GDSFunction::getLogicalPlan assumes output[0] is a NodeExpression (it dereferences it to
// build the node-property hash join), which is only true for the members shape. Route the
// scalar-only shape through the bare TableFunctionCall plan instead (same as DIAMETER etc.).
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GroupBetweennessBindData>();
    if (bindData->summary) {
        auto op =
            std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
        op->computeFactorizedSchema();
        planner->planReadOp(std::move(op), predicates, plan);
        return;
    }
    GDSFunction::getLogicalPlan(planner, readingClause, std::move(predicates), plan);
}

function_set GroupBetweennessFunction::getFunctionSet() {
    function_set result;
    auto positional = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64});
    positional->bindFunc = bindGroupBetweenness;
    positional->tableFunc = tableFuncGroupBetweenness;
    positional->initSharedStateFunc = initSharedState;
    positional->initLocalStateFunc = TableFunction::initEmptyLocalState;
    positional->canParallelFunc = [] { return false; };
    positional->getLogicalPlanFunc = getLogicalPlan;
    positional->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(positional));

    auto named = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    named->bindFunc = bindGroupBetweenness;
    named->tableFunc = tableFuncGroupBetweenness;
    named->initSharedStateFunc = initSharedState;
    named->initLocalStateFunc = TableFunction::initEmptyLocalState;
    named->canParallelFunc = [] { return false; };
    named->getLogicalPlanFunc = getLogicalPlan;
    named->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(named));
    return result;
}

// ---- GROUP_CLOSENESS ---------------------------------------------------------------

struct GroupClosenessBindData final : public GDSBindData {
    int64_t k;
    bool harmonic;

    GroupClosenessBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t k, bool harmonic)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)},
          k{k}, harmonic{harmonic} {}

    GroupClosenessBindData(const GroupClosenessBindData& other)
        : GDSBindData{other}, k{other.k}, harmonic{other.harmonic} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<GroupClosenessBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindGroupCloseness(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    int64_t k = -1;
    std::string mode = "greedy";
    std::string output = "members";
    std::string variant = "harmonic";
    parseCommonParams(context, input, k, mode, output);
    for (auto& optionalParam : input->optionalParamsLegacy) {
        if (normalizeParamName(optionalParam->getAlias()) == "variant") {
            variant = ExpressionUtil::evaluateLiteral<std::string>(context, optionalParam,
                LogicalType::STRING());
        }
    }
    const bool harmonic = (variant == "harmonic");
    if (!harmonic && variant != "standard") {
        throw BinderException{
            "GROUP_CLOSENESS variant must be 'harmonic' or 'standard'."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable("group_score", LogicalType::DOUBLE()));
    return std::make_unique<GroupClosenessBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, k, harmonic);
}

static offset_t tableFuncGroupCloseness(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<GroupClosenessBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{"GROUP_CLOSENESS supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    const auto adj = buildUndirectedAdjacency(csr);
    std::vector<uint64_t> members;
    std::vector<double> scores;
    greedyGroupCloseness(adj, static_cast<uint64_t>(bindData->k), bindData->harmonic, members,
        scores);

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto scoreVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    scoreVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), scoreVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (size_t i = 0; i < members.size(); ++i) {
        nodeIDVector->setValue<nodeID_t>(0,
            nodeID_t{static_cast<offset_t>(members[i]), tableID});
        scoreVector->setValue<double>(0, scores[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set GroupClosenessFunction::getFunctionSet() {
    function_set result;
    auto positional = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64});
    positional->bindFunc = bindGroupCloseness;
    positional->tableFunc = tableFuncGroupCloseness;
    positional->initSharedStateFunc = initSharedState;
    positional->initLocalStateFunc = TableFunction::initEmptyLocalState;
    positional->canParallelFunc = [] { return false; };
    positional->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    positional->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(positional));

    auto named = std::make_unique<TableFunction>(name, std::vector{LogicalTypeID::ANY});
    named->bindFunc = bindGroupCloseness;
    named->tableFunc = tableFuncGroupCloseness;
    named->initSharedStateFunc = initSharedState;
    named->initLocalStateFunc = TableFunction::initEmptyLocalState;
    named->canParallelFunc = [] { return false; };
    named->getLogicalPlanFunc = GDSFunction::getLogicalPlan;
    named->getPhysicalPlanFunc = GDSFunction::getPhysicalPlan;
    result.push_back(std::move(named));
    return result;
}

} // namespace algo_extension
} // namespace lbug
