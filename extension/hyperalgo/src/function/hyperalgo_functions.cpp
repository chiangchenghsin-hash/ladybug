#include "hyperalgo_functions.h"

#include "binder/binder.h"
#include "common/exception/binder.h"
#include "common/types/value/value.h"
#include "hyper_graph.h"
#include "function/table/bind_data.h"
#include "function/table/bind_input.h"
#include "function/table/simple_table_function.h"
#include "function/table/table_function.h"
#include "processor/execution_context.h"

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::processor;
using namespace hyperalgo; // NodeState/EdgeState/HypergraphCSR 等

namespace lbug {
namespace hyperalgo_extension {

namespace {

// 通用绑定数据:结果行在 bind 期由注册表句柄计算(分析批处理语义;无 DB 依赖)
struct HyperAlgoBindData final : TableFuncBindData {
    std::string graphName;
    std::vector<std::vector<Value>> rows; // 每行各列

    HyperAlgoBindData(expression_vector columns, std::string graphName,
        std::vector<std::vector<Value>> rows)
        : TableFuncBindData{std::move(columns), rows.size()},
          graphName{std::move(graphName)}, rows{std::move(rows)} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<HyperAlgoBindData>(columns, graphName, rows);
    }
};

// 按 morsel 分块输出行
static offset_t emitRows(const TableFuncMorsel& morsel, const TableFuncInput& input,
    DataChunk& output) {
    auto bindData = input.bindData->constPtrCast<HyperAlgoBindData>();
    const uint64_t total = bindData->rows.size();
    if (morsel.startOffset >= total) return 0;
    const uint64_t count = std::min<uint64_t>(morsel.getMorselSize(), total - morsel.startOffset);
    for (uint64_t i = 0; i < count; ++i) {
        const auto& row = bindData->rows[morsel.startOffset + i];
        for (size_t c = 0; c < row.size(); ++c)
            output.getValueVectorMutable(c).copyFromValue(i, row[c]);
    }
    output.state->getSelVectorUnsafe().setSelSize(count);
    return (offset_t)count;
}

// 构造 SimpleTableFunc 通用骨架(名称 + 参数类型 + bind)
std::unique_ptr<TableFunction> makeTableFunc(const char* fnName,
    std::vector<LogicalTypeID> argTypes, table_func_bind_t bindFunc) {
    auto func = std::make_unique<TableFunction>(fnName, std::move(argTypes));
    func->bindFunc = std::move(bindFunc);
    func->tableFunc = SimpleTableFunc::getTableFunc(emitRows);
    func->initSharedStateFunc = SimpleTableFunc::initSharedState;
    func->initLocalStateFunc = TableFunction::initEmptyLocalState;
    func->canParallelFunc = [] { return false; };
    return func;
}

// 辅助:取句柄 + 全活状态
const HyperGraphHandle& requireHandle(const std::string& name) {
    return HyperGraphRegistry::instance().get(name);
}

// LogicalType 构造器私有且 move-only:经静态工厂函数指针折叠构建(免拷贝)
template<typename... FACTORIES>
std::vector<LogicalType> makeTypes(FACTORIES... factories) {
    std::vector<LogicalType> types;
    (types.emplace_back(factories()), ...);
    return types;
}

// 绑定期统一骨架:读图名 + 参数 → 算行 → 建列
template<typename ROWBUILDER>
std::unique_ptr<TableFuncBindData> bindAlgo(main::ClientContext* context,
    const TableFuncBindInput* input, const std::vector<LogicalType>& types,
    const std::vector<std::string>& columnNames, ROWBUILDER&& build) {
    auto graphName = input->getLiteralVal<std::string>(0);
    const auto& handle = requireHandle(graphName);
    auto [ns, es] = allAliveState(handle);
    auto rows = build(handle, ns, es, input);
    auto columnNamesOut = TableFunction::extractYieldVariables(columnNames, input->yieldVariables);
    auto columns = input->binder->createVariables(columnNamesOut, types);
    return std::make_unique<HyperAlgoBindData>(std::move(columns), graphName, std::move(rows));
}

// ---- P0-1a ----
static std::unique_ptr<TableFuncBindData> bindSCC(main::ClientContext* context,
    const TableFuncBindInput* input) {
    const auto s = (uint32_t)input->getLiteralVal<int64_t>(1);
    return bindAlgo(context, input,
        makeTypes(LogicalType::STRING, LogicalType::INT64), {"node", "component_id"},
        [s](const HyperGraphHandle& h, const NodeState&, const EdgeState&, const TableFuncBindInput*) {
            auto cc = hyperalgo::s_connected_components(h.csr, s);
            std::vector<std::vector<Value>> rows;
            rows.reserve(h.csr.n_nodes);
            for (uint64_t u = 0; u < h.csr.n_nodes; ++u)
                rows.push_back({Value::createValue(h.idmap.ext_of_int(u)),
                    Value::createValue<int64_t>((int64_t)cc[u])});
            return rows;
        });
}

// ---- P0-1b ----
static std::unique_ptr<TableFuncBindData> bindKittingCC(main::ClientContext* context,
    const TableFuncBindInput* input) {
    return bindAlgo(context, input,
        makeTypes(LogicalType::STRING, LogicalType::INT64), {"node", "block_id"},
        [](const HyperGraphHandle& h, const NodeState& ns, const EdgeState& es,
            const TableFuncBindInput*) {
            auto blocks = hyperalgo::kitting_components(h.csr, ns, es);
            std::vector<std::vector<Value>> rows;
            rows.reserve(h.csr.n_nodes);
            for (uint64_t u = 0; u < h.csr.n_nodes; ++u) {
                const auto b = blocks[u];
                if (b == std::numeric_limits<uint32_t>::max()) continue; // 未覆盖节点不出行
                rows.push_back({Value::createValue(h.idmap.ext_of_int(u)),
                    Value::createValue<int64_t>((int64_t)b)});
            }
            return rows;
        });
}

// ---- P1a ----
static std::unique_ptr<TableFuncBindData> bindHitSet(main::ClientContext* context,
    const TableFuncBindInput* input) {
    return bindAlgo(context, input, makeTypes(LogicalType::STRING), {"node"},
        [](const HyperGraphHandle& h, const NodeState& ns, const EdgeState& es,
            const TableFuncBindInput*) {
            auto hit = hyperalgo::hitting_set_greedy(h.csr, ns, es);
            std::vector<std::vector<Value>> rows;
            rows.reserve(hit.size());
            for (uint32_t u : hit)
                rows.push_back({Value::createValue(h.idmap.ext_of_int(u))});
            return rows;
        });
}

// ---- P1b ----
static std::unique_ptr<TableFuncBindData> bindKSCore(main::ClientContext* context,
    const TableFuncBindInput* input) {
    const auto k = (uint32_t)input->getLiteralVal<int64_t>(1);
    const auto s = (uint32_t)input->getLiteralVal<int64_t>(2);
    return bindAlgo(context, input,
        makeTypes(LogicalType::STRING, LogicalType::INT64), {"node", "layer"},
        [k, s](const HyperGraphHandle& h, const NodeState&, const EdgeState&,
            const TableFuncBindInput*) {
            auto layers = hyperalgo::ks_core_layers(h.csr, k, s);
            std::vector<std::vector<Value>> rows;
            for (uint64_t layer = 0; layer < layers.size(); ++layer)
                for (uint32_t u : layers[layer])
                    rows.push_back({Value::createValue(h.idmap.ext_of_int(u)),
                        Value::createValue<int64_t>((int64_t)layer)});
            return rows;
        });
}

// ---- P0-2 ----
static std::unique_ptr<TableFuncBindData> bindBCycles(main::ClientContext* context,
    const TableFuncBindInput* input) {
    return bindAlgo(context, input,
        makeTypes(LogicalType::INT64, LogicalType::STRING), {"cycle_id", "edge"},
        [](const HyperGraphHandle& h, const NodeState&, const EdgeState&,
            const TableFuncBindInput*) {
            auto cycles = hyperalgo::b_cycles(h.csr);
            std::vector<std::vector<Value>> rows;
            for (uint64_t c = 0; c < cycles.size(); ++c)
                for (uint32_t e : cycles[c]) {
                    const uint32_t s = h.csr.edge_business_ids[e]; // S 内部 id
                    rows.push_back({Value::createValue<int64_t>((int64_t)c),
                        Value::createValue(h.idmap.ext_of_int(s))});
                }
            return rows;
        });
}

// ---- P2 ----
static std::unique_ptr<TableFuncBindData> bindPRWalk(main::ClientContext* context,
    const TableFuncBindInput* input) {
    // getLiteralVal 仅显式实例化整数/字符串;double 经 Value::getValue<double>(内联)
    const auto alpha = input->getValue(1).getValue<double>();
    const auto maxIter = (uint32_t)input->getLiteralVal<int64_t>(2);
    const auto tol = input->getValue(3).getValue<double>();
    return bindAlgo(context, input,
        makeTypes(LogicalType::STRING, LogicalType::DOUBLE), {"node", "score"},
        [alpha, maxIter, tol](const HyperGraphHandle& h, const NodeState&, const EdgeState&,
            const TableFuncBindInput*) {
            auto pi = hyperalgo::pagerank_two_step(h.csr, alpha, maxIter, tol);
            std::vector<std::vector<Value>> rows;
            rows.reserve(h.csr.n_nodes);
            for (uint64_t u = 0; u < h.csr.n_nodes; ++u)
                rows.push_back({Value::createValue(h.idmap.ext_of_int(u)),
                    Value::createValue<double>(pi[u])});
            return rows;
        });
}

// ---- P1c ----
static std::unique_ptr<TableFuncBindData> bindFiedler(main::ClientContext* context,
    const TableFuncBindInput* input) {
    return bindAlgo(context, input,
        makeTypes(LogicalType::STRING, LogicalType::DOUBLE), {"node", "score"},
        [](const HyperGraphHandle& h, const NodeState&, const EdgeState&,
            const TableFuncBindInput*) {
            auto f = hyperalgo::fiedler_vector(h.csr);
            std::vector<std::vector<Value>> rows;
            rows.reserve(h.csr.n_nodes);
            for (uint64_t u = 0; u < h.csr.n_nodes; ++u)
                rows.push_back({Value::createValue(h.idmap.ext_of_int(u)),
                    Value::createValue<double>(f[u])});
            return rows;
        });
}

} // namespace

function_set HyperSCCFunction::getFunctionSet() {
    function_set result;
    result.push_back(
        makeTableFunc(name, std::vector{LogicalTypeID::STRING, LogicalTypeID::INT64}, bindSCC));
    return result;
}

function_set HyperKittingCCFunction::getFunctionSet() {
    function_set result;
    result.push_back(makeTableFunc(
        name, std::vector{LogicalTypeID::STRING}, bindKittingCC));
    return result;
}

function_set HyperHitSetFunction::getFunctionSet() {
    function_set result;
    result.push_back(
        makeTableFunc(name, std::vector{LogicalTypeID::STRING}, bindHitSet));
    return result;
}

function_set HyperKSCoreFunction::getFunctionSet() {
    function_set result;
    result.push_back(makeTableFunc(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::INT64, LogicalTypeID::INT64},
        bindKSCore));
    return result;
}

function_set HyperBCyclesFunction::getFunctionSet() {
    function_set result;
    result.push_back(
        makeTableFunc(name, std::vector{LogicalTypeID::STRING}, bindBCycles));
    return result;
}

function_set HyperPRWalkFunction::getFunctionSet() {
    function_set result;
    result.push_back(makeTableFunc(name,
        std::vector{LogicalTypeID::STRING, LogicalTypeID::DOUBLE, LogicalTypeID::INT64,
            LogicalTypeID::DOUBLE},
        bindPRWalk));
    return result;
}

function_set HyperFiedlerFunction::getFunctionSet() {
    function_set result;
    result.push_back(
        makeTableFunc(name, std::vector{LogicalTypeID::STRING}, bindFiedler));
    return result;
}

} // namespace hyperalgo_extension
} // namespace lbug
