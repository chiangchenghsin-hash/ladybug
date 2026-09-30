// SPECTRAL_PARTITIONING - spectral bisection via the Laplacian Fiedler vector.
// P2-10 (NASH×GNN P2 batch): Cluster-GCN/METIS 分区初始化。对标 NetworKit
// `partitioning::SpectralPartitioner`.
//
//   CALL spectral_partitioning('g', k := 2) YIELD node, partition
//
// Undirected, unweighted; k is currently fixed at 2 (bisection, documented).
// Computes the graph Laplacian L = D - A and its full eigendecomposition with a
// dense symmetric Jacobi rotation (O(n^3), fine for moderate test sizes), then
// splits nodes by the sign of the Fiedler vector (2nd-smallest eigenvalue). A
// disconnected graph (2nd-smallest eigenvalue == 0) falls back to a connected-
// component partition.
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

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace lbug::binder;
using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::graph;
using namespace lbug::processor;
using namespace lbug::storage;

namespace lbug {
namespace algo_extension {

static constexpr char PARTITION_COLUMN_NAME[] = "partition";
static constexpr int64_t DEFAULT_K = 2;

struct SpectralPartitioningBindData final : public GDSBindData {
    int64_t k;

    SpectralPartitioningBindData(expression_vector columns, graph::NativeGraphEntry graphEntry,
        expression_vector nodeOutputs, int64_t k)
        : GDSBindData{std::move(columns), std::move(graphEntry), std::move(nodeOutputs)}, k{k} {}

    SpectralPartitioningBindData(const SpectralPartitioningBindData& other)
        : GDSBindData{other}, k{other.k} {}

    std::unique_ptr<TableFuncBindData> copy() const override {
        return std::make_unique<SpectralPartitioningBindData>(*this);
    }
};

static std::unique_ptr<TableFuncBindData> bindFunc(main::ClientContext* context,
    const TableFuncBindInput* input) {
    auto graphName = input->getLiteralVal<std::string>(0);
    auto graphEntry = GDSFunction::bindGraphEntry(*context, graphName);
    auto nodeOutput = GDSFunction::bindNodeOutput(*input, graphEntry.getNodeEntries());

    int64_t k = DEFAULT_K;
    for (auto& optionalParam : input->optionalParamsLegacy) {
        auto paramName = normalizeParamName(optionalParam->getAlias());
        if (paramName == "k") {
            k = ExpressionUtil::evaluateLiteral<int64_t>(context, optionalParam,
                LogicalType::INT64());
        } else {
            throw BinderException{"Unknown optional parameter: " + optionalParam->getAlias()};
        }
    }
    if (k != 2) {
        throw BinderException{
            "SPECTRAL_PARTITIONING currently supports k = 2 only (spectral bisection)."};
    }

    expression_vector columns;
    columns.push_back(nodeOutput->constCast<NodeExpression>().getInternalID());
    columns.push_back(input->binder->createVariable(PARTITION_COLUMN_NAME, LogicalType::INT64()));
    return std::make_unique<SpectralPartitioningBindData>(std::move(columns), std::move(graphEntry),
        expression_vector{nodeOutput}, k);
}

// Symmetric Jacobi eigenvalue decomposition: A (n x n, symmetric) is rotated to
// (numerically) diagonal in place; V accumulates the eigenvectors as columns.
void jacobiEigen(std::vector<std::vector<double>>& A, std::vector<std::vector<double>>& V) {
    const auto n = A.size();
    V.assign(n, std::vector<double>(n, 0.0));
    for (uint64_t i = 0; i < n; ++i) {
        V[i][i] = 1.0;
    }
    if (n <= 1) {
        return;
    }
    constexpr double tolerance = 1e-12;
    for (int sweep = 0; sweep < 128; ++sweep) {
        double offDiag = 0.0;
        for (uint64_t p = 0; p < n; ++p) {
            for (uint64_t q = p + 1; q < n; ++q) {
                offDiag += A[p][q] * A[p][q];
            }
        }
        if (offDiag < tolerance) {
            break;
        }
        for (uint64_t p = 0; p + 1 < n; ++p) {
            for (uint64_t q = p + 1; q < n; ++q) {
                if (std::fabs(A[p][q]) <= 1e-14) {
                    continue;
                }
                const double theta = (A[q][q] - A[p][p]) / (2.0 * A[p][q]);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (uint64_t k = 0; k < n; ++k) {
                    if (k == p || k == q) {
                        continue;
                    }
                    const double akp = A[k][p];
                    const double akq = A[k][q];
                    A[k][p] = A[p][k] = c * akp - s * akq;
                    A[k][q] = A[q][k] = s * akp + c * akq;
                }
                const double app = A[p][p];
                const double aqq = A[q][q];
                A[p][p] = app - t * A[p][q];
                A[q][q] = aqq + t * A[p][q];
                A[p][q] = A[q][p] = 0.0;
                for (uint64_t k = 0; k < n; ++k) {
                    const double vkp = V[k][p];
                    const double vkq = V[k][q];
                    V[k][p] = c * vkp - s * vkq;
                    V[k][q] = s * vkp + c * vkq;
                }
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
    auto bindData = input.bindData->constPtrCast<SpectralPartitioningBindData>();

    const auto maxOffset = graph->getMaxOffsetMap(transaction);
    if (maxOffset.size() != 1) {
        throw BinderException{
            "SPECTRAL_PARTITIONING currently supports single-node-table graphs only."};
    }
    const auto tableID = maxOffset.begin()->first;
    const auto numNodes = maxOffset.begin()->second;
    (void)bindData; // k is validated as 2 at bind time

    auto csr = buildGraphCSR(graph, tableID, numNodes);
    auto adj = buildUndirectedAdjacency(csr);
    for (uint64_t w = 0; w < adj.size(); ++w) {
        auto& nbrs = adj[w];
        nbrs.erase(std::remove(nbrs.begin(), nbrs.end(), w), nbrs.end());
    }

    // Graph Laplacian L = D - A.
    std::vector<std::vector<double>> L(numNodes, std::vector<double>(numNodes, 0.0));
    for (uint64_t u = 0; u < numNodes; ++u) {
        L[u][u] = static_cast<double>(adj[u].size());
        for (const auto v : adj[u]) {
            if (v != u) {
                L[u][v] = -1.0;
            }
        }
    }

    std::vector<int64_t> partition(numNodes, 0);
    if (numNodes <= 1) {
        partition.assign(numNodes, 0);
    } else {
        auto A = L;
        std::vector<std::vector<double>> V;
        jacobiEigen(A, V);
        std::vector<std::pair<double, uint64_t>> eigen;
        for (uint64_t i = 0; i < numNodes; ++i) {
            eigen.emplace_back(A[i][i], i);
        }
        std::sort(eigen.begin(), eigen.end());
        // Jacobi: V's COLUMN j is the eigenvector for eigenvalue A[j][j].
        const auto fiedlerIdx = eigen[1].second;
        if (std::fabs(eigen[1].first) < 1e-9) {
            // Disconnected: Fiedler value is 0 -> fall back to components.
            std::vector<uint64_t> comp(numNodes, UINT64_MAX);
            std::vector<uint64_t> queue;
            queue.reserve(numNodes);
            int64_t nextId = 0;
            for (uint64_t s = 0; s < numNodes; ++s) {
                if (comp[s] != UINT64_MAX) {
                    continue;
                }
                comp[s] = s;
                queue.clear();
                queue.push_back(s);
                for (uint64_t head = 0; head < queue.size(); ++head) {
                    const auto w = queue[head];
                    for (const auto t : adj[w]) {
                        if (comp[t] == UINT64_MAX) {
                            comp[t] = s;
                            queue.push_back(t);
                        }
                    }
                }
                for (uint64_t v = 0; v < numNodes; ++v) {
                    if (comp[v] == s) {
                        partition[v] = nextId;
                    }
                }
                nextId++;
            }
        } else {
            for (uint64_t v = 0; v < numNodes; ++v) {
                partition[v] = V[v][fiedlerIdx] >= 0.0 ? 0 : 1;
            }
        }
    }

    auto nodeIDVector = std::make_unique<ValueVector>(LogicalType::INTERNAL_ID(), mm);
    nodeIDVector->state = DataChunkState::getSingleValueDataChunkState();
    auto partitionVector = std::make_unique<ValueVector>(LogicalType::INT64(), mm);
    partitionVector->state = DataChunkState::getSingleValueDataChunkState();
    std::vector<ValueVector*> vectors{nodeIDVector.get(), partitionVector.get()};

    auto localFT = sharedState->factorizedTablePool.claimLocalTable(mm);
    for (uint64_t i = 0; i < numNodes; ++i) {
        nodeIDVector->setValue<nodeID_t>(0, nodeID_t{static_cast<offset_t>(i), tableID});
        partitionVector->setValue<int64_t>(0, partition[i]);
        localFT->append(vectors);
    }
    sharedState->factorizedTablePool.returnLocalTable(localFT);
    sharedState->factorizedTablePool.mergeLocalTables();
    return 0;
}

function_set SpectralPartitioningFunction::getFunctionSet() {
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
