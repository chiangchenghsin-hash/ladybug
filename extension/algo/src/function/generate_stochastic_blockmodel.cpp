// GENERATE_STOCHASTIC_BLOCKMODEL - stochastic block model generator.
// P3-03 (NASH×GNN P3 batch): 已知社区结构 → GNN 基准.
// 对标 NetworkX `generators.community.stochastic_block_model` (undirected,
// no self-loops; unweighted -> weight = 1.0, per user decision 2026-08-29).
//
//   CALL generate_stochastic_blockmodel(blocks := [100, 100, 100],
//       P := [[0.5, 0.1, 0.1], [0.1, 0.5, 0.1], [0.1, 0.1, 0.5]],
//       seed := 42) YIELD node1, node2, weight
//
// Semantics (aligned with NetworkX):
//   - node labels 0..sum(blocks)-1; blocks occupy contiguous ranges;
//   - every unordered node pair (x, y) in block pair (i, j), i <= j, is an
//     edge independently with probability P[i][j] (in-block pairs are
//     combinations x < y, cross-block pairs are the full product);
//   - P must be square, symmetric (undirected), entries in [0, 1];
//   - p == 0 / p == 1 make the output fully deterministic (test-friendly);
//   - no self-loops, no multi-edges.
//
// Plain table function: named LIST literals read from `optionalParams` via
// NestedVal. Edges materialized at bind time, streamed by SimpleTableFunc.
#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/types/value/nested.h"
#include "common/types/value/value.h"
#include "function/algo_function.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/table/table_function.h"

#include <cmath>
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

struct StochasticBlockmodelBindData final : TableFuncBindData {
    struct Edge {
        int64_t u;
        int64_t v;
        double weight;
    };
    std::vector<Edge> edges;

    StochasticBlockmodelBindData(binder::expression_vector columns, common::row_idx_t numRows,
        std::vector<Edge> edges)
        : TableFuncBindData{std::move(columns), numRows}, edges{std::move(edges)} {}

    StochasticBlockmodelBindData(const StochasticBlockmodelBindData& other)
        : TableFuncBindData{other}, edges{other.edges} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<StochasticBlockmodelBindData>(*this);
    }
};

// Fetches a named list parameter from the literal-Value map.
static const Value& getListParam(const TableFuncBindInput* input, const std::string& name) {
    auto it = input->optionalParams.find(name);
    if (it == input->optionalParams.end()) {
        throw BinderException{std::string("generate_stochastic_blockmodel requires the ") + name +
            " parameter."};
    }
    return it->second;
}

static int64_t getSeedParam(const TableFuncBindInput* input) {
    auto it = input->optionalParams.find("seed");
    if (it == input->optionalParams.end()) {
        return DEFAULT_SEED;
    }
    return it->second.getValue<int64_t>();
}

static void generateSBM(const std::vector<int64_t>& blocks,
    const std::vector<std::vector<double>>& probs, uint64_t seed,
    std::vector<StochasticBlockmodelBindData::Edge>& edges) {
    const auto numBlocks = blocks.size();

    // Contiguous block ranges over node labels 0..n-1.
    std::vector<std::pair<int64_t, int64_t>> ranges(numBlocks); // [lo, hi)
    int64_t lo = 0;
    for (size_t b = 0; b < numBlocks; ++b) {
        ranges[b] = {lo, lo + blocks[b]};
        lo += blocks[b];
    }

    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> prob(0.0, 1.0);

    // Block pairs (i, j) with i <= j, in NetworkX combinations-with-replacement
    // order: (0,0), (0,1), ..., (0,B-1), (1,1), ...
    for (size_t i = 0; i < numBlocks; ++i) {
        for (size_t j = i; j < numBlocks; ++j) {
            const auto p = probs[i][j];
            const auto [loI, hiI] = ranges[i];
            const auto [loJ, hiJ] = ranges[j];
            if (i == j) {
                for (int64_t x = loI; x < hiI; ++x) {
                    for (int64_t y = x + 1; y < hiI; ++y) {
                        if (prob(rng) < p) {
                            edges.push_back({x, y, 1.0});
                        }
                    }
                }
            } else {
                for (int64_t x = loI; x < hiI; ++x) {
                    for (int64_t y = loJ; y < hiJ; ++y) {
                        if (prob(rng) < p) {
                            edges.push_back({x, y, 1.0});
                        }
                    }
                }
            }
        }
    }
}

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* /*context*/,
    const TableFuncBindInput* input) {
    const auto seed = getSeedParam(input);

    const auto& blocksVal = getListParam(input, "blocks");
    const auto& probsVal = getListParam(input, "P");

    const auto numBlocks = NestedVal::getChildrenSize(&blocksVal);
    std::vector<int64_t> blocks(numBlocks);
    for (uint32_t i = 0; i < numBlocks; ++i) {
        blocks[i] = NestedVal::getChildVal(&blocksVal, i)->getValue<int64_t>();
        if (blocks[i] < 1) {
            throw BinderException{"generate_stochastic_blockmodel requires block sizes >= 1."};
        }
    }

    if (NestedVal::getChildrenSize(&probsVal) != numBlocks) {
        throw BinderException{"generate_stochastic_blockmodel requires len(blocks) == len(P)."};
    }
    std::vector<std::vector<double>> probs(numBlocks, std::vector<double>(numBlocks));
    for (uint32_t i = 0; i < numBlocks; ++i) {
        const auto* row = NestedVal::getChildVal(&probsVal, i);
        if (NestedVal::getChildrenSize(row) != numBlocks) {
            throw BinderException{"generate_stochastic_blockmodel requires a square P matrix."};
        }
        for (uint32_t j = 0; j < numBlocks; ++j) {
            probs[i][j] = NestedVal::getChildVal(row, j)->getValue<double>();
            if (probs[i][j] < 0.0 || probs[i][j] > 1.0) {
                throw BinderException{"Entries of 'P' must be in [0, 1]."};
            }
        }
    }
    for (uint32_t i = 0; i < numBlocks; ++i) {
        for (uint32_t j = 0; j < numBlocks; ++j) {
            if (std::fabs(probs[i][j] - probs[j][i]) > 1e-8) {
                throw BinderException{
                    "generate_stochastic_blockmodel requires a symmetric P (undirected)."};
            }
        }
    }

    std::vector<StochasticBlockmodelBindData::Edge> edges;
    int64_t totalNodes = 0;
    for (const auto b : blocks) {
        totalNodes += b;
    }
    edges.reserve(static_cast<size_t>(totalNodes));
    generateSBM(blocks, probs, static_cast<uint64_t>(seed), edges);

    std::vector<std::string> returnColumnNames{NODE1_COLUMN_NAME, NODE2_COLUMN_NAME,
        WEIGHT_COLUMN_NAME};
    std::vector<LogicalType> returnTypes;
    returnTypes.emplace_back(LogicalType::INT64());
    returnTypes.emplace_back(LogicalType::INT64());
    returnTypes.emplace_back(LogicalType::DOUBLE());
    returnColumnNames = TableFunction::extractYieldVariables(returnColumnNames,
        input->yieldVariables);
    auto columns = input->binder->createVariables(returnColumnNames, returnTypes);
    return std::make_unique<StochasticBlockmodelBindData>(std::move(columns), edges.size(),
        std::move(edges));
}

static offset_t internalTableFunc(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    auto bindData = input.bindData->constPtrCast<StochasticBlockmodelBindData>();
    const auto size = morsel.getMorselSize();
    for (auto i = 0u; i < size; i++) {
        const auto& edge = bindData->edges[morsel.startOffset + i];
        output.getValueVectorMutable(0).setValue(i, edge.u);
        output.getValueVectorMutable(1).setValue(i, edge.v);
        output.getValueVectorMutable(2).setValue(i, edge.weight);
    }
    return size;
}

function_set StochasticBlockmodelFunction::getFunctionSet() {
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
