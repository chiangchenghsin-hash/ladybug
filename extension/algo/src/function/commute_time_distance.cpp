// COMMUTE_TIME_DISTANCE - effective-resistance-based commute time between two
// nodes. P2-12 (NASH×GNN P2 batch): 随机游走策略亲密度; 图核。对标 NetworKit
// `distance::CommuteTimeDistance`.
//
//   CALL commute_time_distance('g', source, target)
//     YIELD source, target, distance
//
// Definition (documented): commute time = m_total * R_eff(u, v), where m_total
// is the total edge conductance (sum of weights on the undirected view; weight
// default 1.0) and R_eff is the effective resistance, computed by solving the
// weighted graph Laplacian with a reference-potential (target held at 0)
// dense Gaussian elimination. Requires an undirected connected graph between
// source and target (offsets); disconnected -> BinderException.
//
// Known answer: path P3 (0-1-2, all weights 1): m_total=2, R_eff(0,2)=2,
// CT(0,2)=4. Weighted chain 0-(1)-1-(3)-2: CT(0,2)=m_total*R_eff=4*(4/3).
#include "binder/binder.h"
#include "binder/query/reading_clause/bound_table_function_call.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "common/weighted_graph.h"
#include "function/algo_function.h"
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

#include <cmath>
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

static constexpr char SRC_COLUMN_NAME[] = "source";
static constexpr char DST_COLUMN_NAME[] = "target";
static constexpr char DISTANCE_COLUMN_NAME[] = "distance";

static constexpr char WEIGHT_PROPERTY[] = "weight";

struct CommuteTimeDistanceBindData final : public GDSBindData {
    uint64_t source;
    uint64_t target;

    CommuteTimeDistanceBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        uint64_t source, uint64_t target)
        : GDSBindData{std::move(columns), std::move(graphEntry), expression_vector{}},
          source{source}, target{target} {}

    CommuteTimeDistanceBindData(const CommuteTimeDistanceBindData& other)
        : GDSBindData{other}, source{other.source}, target{other.target} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<CommuteTimeDistanceBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    const auto source = static_cast<uint64_t>(input->getLiteralVal<int64_t>(1));
    const auto target = static_cast<uint64_t>(input->getLiteralVal<int64_t>(2));

    expression_vector columns;
    columns.push_back(input->binder->createVariable(SRC_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DST_COLUMN_NAME, LogicalType::INT64()));
    columns.push_back(input->binder->createVariable(DISTANCE_COLUMN_NAME, LogicalType::DOUBLE()));
    return std::make_unique<CommuteTimeDistanceBindData>(std::move(columns), std::move(graphEntry),
        source, target);
}

// Solves a dense linear system by Gaussian elimination with partial pivoting.
// Returns false when the matrix is (numerically) singular.
bool solveLinearSystem(std::vector<std::vector<double>> A, std::vector<double> b,
    std::vector<double>& x) {
    const auto n = A.size();
    if (n == 0) {
        return true;
    }
    for (size_t col = 0; col < n; ++col) {
        size_t pivot = col;
        for (size_t row = col + 1; row < n; ++row) {
            if (std::fabs(A[row][col]) > std::fabs(A[pivot][col])) {
                pivot = row;
            }
        }
        if (std::fabs(A[pivot][col]) < 1e-12) {
            return false;
        }
        if (pivot != col) {
            std::swap(A[pivot], A[col]);
            std::swap(b[pivot], b[col]);
        }
        for (size_t row = col + 1; row < n; ++row) {
            const auto factor = A[row][col] / A[col][col];
            for (size_t j = col; j < n; ++j) {
                A[row][j] -= factor * A[col][j];
            }
            b[row] -= factor * b[col];
        }
    }
    x.assign(n, 0.0);
    for (size_t i = n; i-- > 0;) {
        double s = b[i];
        for (size_t j = i + 1; j < n; ++j) {
            s -= A[i][j] * x[j];
        }
        x[i] = s / A[i][i];
    }
    return true;
}

static offset_t tableFunc(const TableFuncInput& input, TableFuncOutput&) {
    auto clientContext = input.context->clientContext;
    auto transaction = transaction::Transaction::Get(*clientContext);
    auto sharedState = input.sharedState->ptrCast<GDSFuncSharedState>();
    auto graph = sharedState->graph.get();
    auto mm = MemoryManager::Get(*clientContext);
    auto bindData = input.bindData->constPtrCast<CommuteTimeDistanceBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "COMMUTE_TIME_DISTANCE currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;
    const auto source = bindData->source;
    const auto target = bindData->target;
    if (source >= numNodes || target >= numNodes) {
        throw BinderException{"COMMUTE_TIME_DISTANCE source/target out of range."};
    }

    auto wg = buildWeightedGraph(graph, tableID, numNodes, WEIGHT_PROPERTY);

    // Build the weighted Laplacian (conductance = weight).
    std::vector<std::vector<double>> L(numNodes, std::vector<double>(numNodes, 0.0));
    double mTotal = 0.0;
    for (uint64_t u = 0; u < wg.und.size(); ++u) {
        for (const auto& [v, w] : wg.und[u]) {
            if (v <= u) {
                continue;
            }
            L[u][v] -= w;
            L[v][u] -= w;
            L[u][u] += w;
            L[v][v] += w;
            mTotal += w;
        }
    }

    double distance = 0.0;
    if (source != target) {
        // Solve the reduced system with `target` held at potential 0, injecting
        // 1 A from `source` (remove row/col `target`).
        const uint64_t n = numNodes - 1;
        std::vector<uint64_t> idx(n);
        uint64_t srcIdx = 0;
        for (uint64_t v = 0, k = 0; v < numNodes; ++v) {
            if (v == target) {
                continue;
            }
            idx[k] = v;
            if (v == source) {
                srcIdx = k;
            }
            k++;
        }
        std::vector<std::vector<double>> A(n, std::vector<double>(n, 0.0));
        std::vector<double> b(n, 0.0);
        for (uint64_t i = 0; i < n; ++i) {
            for (uint64_t j = 0; j < n; ++j) {
                A[i][j] = L[idx[i]][idx[j]];
            }
        }
        b[srcIdx] = 1.0;

        std::vector<double> x;
        if (!solveLinearSystem(std::move(A), b, x)) {
            throw BinderException{
                "COMMUTE_TIME_DISTANCE requires source and target in the same connected "
                "component (graph is disconnected)."};
        }
        // R_eff = potential difference between source and target (= x[srcIdx]).
        const double rEff = x[srcIdx];
        distance = mTotal * rEff;
        if (!std::isfinite(distance) || distance < 0.0) {
            throw BinderException{
                "COMMUTE_TIME_DISTANCE produced a non-finite/negative value; check the graph."};
        }
    }

    auto srcVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    srcVector->state = DataChunkState::getSingleValueDataChunkState();
    auto dstVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    dstVector->state = DataChunkState::getSingleValueDataChunkState();
    auto distVector = std::make_unique<ValueVector>(LogicalType::DOUBLE(), mm);
    distVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{srcVector.get(), dstVector.get(), distVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    srcVector->setValue<int64_t>(0, static_cast<int64_t>(source));
    dstVector->setValue<int64_t>(0, static_cast<int64_t>(target));
    distVector->setValue<double>(0, distance);
    localFT->append(vectors);
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

// Scalar/edge output (no node column): bare TableFunctionCall plan.
static void getLogicalPlan(Planner* planner, const BoundReadingClause& readingClause,
    expression_vector predicates, LogicalPlan& plan) {
    auto& call = readingClause.constCast<BoundTableFunctionCall>();
    auto bindData = call.getBindData()->constPtrCast<GDSBindData>();
    auto op = std::make_shared<LogicalTableFunctionCall>(call.getTableFunc(), bindData->copy());
    op->computeFactorizedSchema();
    planner->planReadOp(std::move(op), predicates, plan);
}

function_set CommuteTimeDistanceFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<TableFunction>(name,
        std::vector{LogicalTypeID::ANY, LogicalTypeID::INT64, LogicalTypeID::INT64});
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
