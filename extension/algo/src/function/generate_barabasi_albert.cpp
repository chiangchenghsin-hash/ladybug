// GENERATE_BARABASI_ALBERT - Barabási–Albert preferential attachment generator.
// P3-01 (NASH×GNN P3 batch): 「富者愈富」市场博弈结构 / GNN 训练数据增强.
// 对标 NetworkX `generators.random_graphs.barabasi_albert_graph` (无权重).
//
//   CALL generate_barabasi_albert(n := 1000, k := 4, seed := 42)
//     YIELD node1, node2, weight
//
// Semantics (aligned with NetworkX):
//   - starts from a star graph on the first k+1 nodes (node 0 center);
//   - each new node source ∈ [k+1, n) picks k UNIQUE existing nodes by
//     preferential attachment (uniform draw from the degree-expanded
//     `repeated_nodes` list, deduplicated, first-selection order);
//   - edge count = k + k*(n-k-1) = k*(n-k); no self-loops, no multi-edges;
//   - all edges are unweighted (weight = 1.0), per user decision 2026-08-29.
//
// This is a PLAIN table function (no projected graph): all parameters are
// named literals read from the case-insensitive `optionalParams` Value map.
// Edges are materialized once at bind time; `SimpleTableFunc` streams them
// out in morsels. Deterministic: std::mt19937(seed), default 42.
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/types/value/value.h"
#include "function/algo_function.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/table/table_function.h"

#include <cstdint>
#include <random>
#include <string>
#include <vector>

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::main;

namespace lbug {
namespace algo_extension {

static constexpr char NODE1_COLUMN_NAME[] = "node1";
static constexpr char NODE2_COLUMN_NAME[] = "node2";
static constexpr char WEIGHT_COLUMN_NAME[] = "weight";

static constexpr int64_t DEFAULT_SEED = 42;

struct BarabasiAlbertBindData final : TableFuncBindData {
    struct Edge {
        int64_t u;
        int64_t v;
        double weight;
    };
    std::vector<Edge> edges;

    BarabasiAlbertBindData(binder::expression_vector columns, common::row_idx_t numRows,
        std::vector<Edge> edges)
        : TableFuncBindData{std::move(columns), numRows}, edges{std::move(edges)} {}

    BarabasiAlbertBindData(const BarabasiAlbertBindData& other)
        : TableFuncBindData{other}, edges{other.edges} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<BarabasiAlbertBindData>(*this);
    }
};

// Reads a named integer parameter from the literal-Value map. Required
// parameters throw if absent; optional ones fall back to `dflt`.
static int64_t getIntParam(const TableFuncBindInput* input, const std::string& name,
    bool required, int64_t dflt) {
    auto it = input->optionalParams.find(name);
    if (it == input->optionalParams.end()) {
        if (!required) {
            return dflt;
        }
        throw BinderException{std::string("generate_barabasi_albert requires the ") + name +
            " parameter."};
    }
    return it->second.getValue<int64_t>();
}

// Builds the Barabási–Albert edge list (NetworkX `barabasi_albert_graph`).
// `repeated_nodes` starts as node 0 repeated k times then 1..k (star degree
// sequence, NetworkX insertion order), and grows as edges are added.
static void generateBA(int64_t n, int64_t k, uint64_t seed,
    std::vector<BarabasiAlbertBindData::Edge>& edges) {
    std::mt19937 rng(seed);
    std::vector<uint64_t> repeatedNodes;
    repeatedNodes.reserve(static_cast<size_t>(2 * k * n));
    for (int64_t s = 0; s < k; ++s) {
        repeatedNodes.push_back(0); // center node 0 has degree k
    }
    for (int64_t i = 1; i <= k; ++i) {
        edges.push_back({0, i, 1.0}); // star leaves 1..k
        repeatedNodes.push_back(static_cast<uint64_t>(i));
    }
    std::uniform_int_distribution<size_t> draw;
    for (int64_t source = k + 1; source < n; ++source) {
        // NetworkX `_random_subset`: draw from repeated_nodes until k unique
        // targets are collected (dedup via linear scan keeps selection order).
        std::vector<uint64_t> selected;
        selected.reserve(static_cast<size_t>(k));
        while (selected.size() < static_cast<size_t>(k)) {
            draw.param(std::uniform_int_distribution<size_t>::param_type(
                0, repeatedNodes.size() - 1));
            const auto target = repeatedNodes[draw(rng)];
            if (std::find(selected.begin(), selected.end(), target) == selected.end()) {
                selected.push_back(target);
            }
        }
        for (const auto t : selected) {
            edges.push_back({source, static_cast<int64_t>(t), 1.0});
        }
        for (const auto t : selected) {
            repeatedNodes.push_back(t); // one entry per new edge
        }
        for (int64_t s = 0; s < k; ++s) {
            repeatedNodes.push_back(static_cast<uint64_t>(source));
        }
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
    const TableFuncBindInput* input) {
    const auto n = getIntParam(input, "n", true, 0);
    const auto k = getIntParam(input, "k", true, 0);
    const auto seed = getIntParam(input, "seed", false, DEFAULT_SEED);

    if (k < 1) {
        throw BinderException{"generate_barabasi_albert requires k >= 1."};
    }
    if (k >= n) {
        throw BinderException{"generate_barabasi_albert requires k < n (k is the number of "
                              "edges attached per new node)."};
    }

    std::vector<BarabasiAlbertBindData::Edge> edges;
    edges.reserve(static_cast<size_t>(k) * static_cast<size_t>(n - k));
    generateBA(n, k, static_cast<uint64_t>(seed), edges);

    std::vector<std::string> returnColumnNames{NODE1_COLUMN_NAME, NODE2_COLUMN_NAME,
        WEIGHT_COLUMN_NAME};
    std::vector<LogicalType> returnTypes;
    returnTypes.emplace_back(LogicalType::INT64());
    returnTypes.emplace_back(LogicalType::INT64());
    returnTypes.emplace_back(LogicalType::DOUBLE());
    returnColumnNames = TableFunction::extractYieldVariables(returnColumnNames,
        input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<BarabasiAlbertBindData>(std::move(columns), edges.size(),
        std::move(edges));
}

static offset_t internalTableFunc(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    auto bindData = input.bindData->constPtrCast<BarabasiAlbertBindData>();
    const auto size = morsel.getMorselSize();
    for (auto i = 0u; i < size; i++) {
        const auto& edge = bindData->edges[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, edge.u);
        output.getValueVectorMutable(1).setValue(i, edge.v);
        output.getValueVectorMutable(2).setValue(i, edge.weight);
    }
    return size;
}

function_set BarabasiAlbertFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name, std::vector<LogicalTypeID>{});
    func->bindFunc = bindFunc;
    func->tableFunc = SimpleTableFunc::getTableFunc(internalTableFunc);
    func->initSharedStateFunc = SimpleTableFunc::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    result.push_back(std::move(func));
    return result;
}

} // namespace algo_extension
} // namespace lbug
