// GENERATE_WATTS_STROGATZ - Watts–Strogatz small-world generator.
// P3-02 (NASH×GNN P3 batch): 小世界(局部聚类+捷径)/ GNN 训练数据增强.
// 对标 NetworkX `generators.random_graphs.watts_strogatz_graph` (无权重).
//
//   CALL generate_watts_strogatz(n := 1000, k := 6, p := 0.1, seed := 42)
//     YIELD node1, node2
//
// Semantics (aligned with NetworkX):
//   - ring over n nodes; each node joins its k nearest neighbours (k//2 circles);
//   - k == n  -> complete graph (short-circuit, no rewiring);
//   - rewire every ring edge in (circle, node) order: with probability p replace
//     (u, v) by (u, w), w uniformly random, rejecting self-loops and existing
//     edges; if u is already saturated (degree >= n-1) the rewiring is skipped;
//   - total edges stay n*(k//2); no self-loops, no multi-edges.
//
// Plain table function (no projected graph). Edges materialized at bind time,
// streamed out by SimpleTableFunc. Deterministic: std::mt19937(seed).
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
#include <unordered_set>
#include <vector>

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::main;

namespace lbug {
namespace algo_extension {

static constexpr char NODE1_COLUMN_NAME[] = "node1";
static constexpr char NODE2_COLUMN_NAME[] = "node2";

static constexpr int64_t DEFAULT_SEED = 42;

struct WattsStrogatzBindData final : TableFuncBindData {
    struct Edge {
        int64_t u;
        int64_t v;
    };
    std::vector<Edge> edges;

    WattsStrogatzBindData(binder::expression_vector columns, common::row_idx_t numRows,
        std::vector<Edge> edges)
        : TableFuncBindData{std::move(columns), numRows}, edges{std::move(edges)} {}

    WattsStrogatzBindData(const WattsStrogatzBindData& other)
        : TableFuncBindData{other}, edges{other.edges} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<WattsStrogatzBindData>(*this);
    }
};

static int64_t getIntParam(const TableFuncBindInput* input, const std::string& name,
    bool required, int64_t dflt) {
    auto it = input->optionalParams.find(name);
    if (it == input->optionalParams.end()) {
        if (!required) {
            return dflt;
        }
        throw BinderException{std::string("generate_watts_strogatz requires the ") + name +
            " parameter."};
    }
    return it->second.getValue<int64_t>();
}

static double getDoubleParam(const TableFuncBindInput* input, const std::string& name,
    bool required, double dflt) {
    auto it = input->optionalParams.find(name);
    if (it == input->optionalParams.end()) {
        if (!required) {
            return dflt;
        }
        throw BinderException{std::string("generate_watts_strogatz requires the ") + name +
            " parameter."};
    }
    return it->second.getValue<double>();
}

// Undirected edge key: min*scale + max (scale = n).
static inline uint64_t edgeKey(uint64_t a, uint64_t b, uint64_t scale) {
    return (a < b ? a * scale + b : b * scale + a);
}

static void generateWS(int64_t n, int64_t k, double p, uint64_t seed,
    std::vector<WattsStrogatzBindData::Edge>& edges) {
    const auto scale = static_cast<uint64_t>(n);
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> prob(0.0, 1.0);
    std::uniform_int_distribution<int64_t> nodeDraw(0, n - 1);

    std::unordered_set<uint64_t> edgeSet;
    std::vector<int64_t> degree(static_cast<size_t>(n), 0);

    const auto addEdge = [&](int64_t u, int64_t v) {
        edges.push_back({u, v});
        edgeSet.insert(edgeKey(static_cast<uint64_t>(u), static_cast<uint64_t>(v), scale));
        degree[static_cast<size_t>(u)]++;
        degree[static_cast<size_t>(v)]++;
    };

    if (k == n) {
        // Complete graph short-circuit (NetworkX: k == n -> complete graph).
        for (int64_t u = 0; u < n; ++u) {
            for (int64_t v = u + 1; v < n; ++v) {
                addEdge(u, v);
            }
        }
        return;
    }

    // Ring: k//2 circles, each node u connected to (u + j) % n.
    for (int64_t j = 1; j <= k / 2; ++j) {
        for (int64_t u = 0; u < n; ++u) {
            addEdge(u, (u + j) % n);
        }
    }

    // Rewire, in the same (circle, node) order the ring was created.
    size_t edgeIdx = 0;
    for (int64_t j = 1; j <= k / 2; ++j) {
        for (int64_t u = 0; u < n; ++u, ++edgeIdx) {
            const auto v = (u + j) % n;
            if (prob(rng) >= p) {
                continue;
            }
            while (true) {
                const auto w = nodeDraw(rng);
                if (w == u ||
                    edgeSet.contains(edgeKey(static_cast<uint64_t>(u),
                        static_cast<uint64_t>(w), scale))) {
                    if (degree[static_cast<size_t>(u)] >= n - 1) {
                        break; // skip this rewiring (node saturated)
                    }
                    continue;
                }
                // Valid target: replace (u, v) by (u, w) in place.
                const auto oldKey = edgeKey(static_cast<uint64_t>(u),
                    static_cast<uint64_t>(v), scale);
                edgeSet.erase(oldKey);
                degree[static_cast<size_t>(u)]--;
                degree[static_cast<size_t>(v)]--;
                edges[edgeIdx] = {u, w};
                edgeSet.insert(edgeKey(static_cast<uint64_t>(u), static_cast<uint64_t>(w),
                    scale));
                degree[static_cast<size_t>(u)]++;
                degree[static_cast<size_t>(w)]++;
                break;
            }
        }
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
    const TableFuncBindInput* input) {
    const auto n = getIntParam(input, "n", true, 0);
    const auto k = getIntParam(input, "k", true, 0);
    const auto p = getDoubleParam(input, "p", true, 0.0);
    const auto seed = getIntParam(input, "seed", false, DEFAULT_SEED);

    if (k < 1) {
        throw BinderException{"generate_watts_strogatz requires k >= 1."};
    }
    if (k > n) {
        throw BinderException{"generate_watts_strogatz requires k <= n."};
    }
    if (p < 0.0 || p > 1.0) {
        throw BinderException{"generate_watts_strogatz requires 0 <= p <= 1."};
    }

    std::vector<WattsStrogatzBindData::Edge> edges;
    edges.reserve(static_cast<size_t>(n) * static_cast<size_t>(k / 2));
    generateWS(n, k, p, static_cast<uint64_t>(seed), edges);

    std::vector<std::string> returnColumnNames{NODE1_COLUMN_NAME, NODE2_COLUMN_NAME};
    std::vector<LogicalType> returnTypes;
    returnTypes.emplace_back(LogicalType::INT64());
    returnTypes.emplace_back(LogicalType::INT64());
    returnColumnNames = TableFunction::extractYieldVariables(returnColumnNames,
        input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<WattsStrogatzBindData>(std::move(columns), edges.size(),
        std::move(edges));
}

static offset_t internalTableFunc(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    auto bindData = input.bindData->constPtrCast<WattsStrogatzBindData>();
    const auto size = morsel.getMorselSize();
    for (auto i = 0u; i < size; i++) {
        const auto& edge = bindData->edges[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, edge.u);
        output.getValueVectorMutable(1).setValue(i, edge.v);
    }
    return size;
}

function_set WattsStrogatzFunction::getFunctionSet() {
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
