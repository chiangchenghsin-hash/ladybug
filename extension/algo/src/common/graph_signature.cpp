// 图签名：架构漂移检测的特征向量（复用 DirectedCSR + detectDAG）。
#include "common/graph_signature.h"

#include <algorithm>
#include <cmath>

namespace lbug {
namespace algo_extension {

namespace {

// Tarjan SCC（有向图），返回分量数（迭代式 DFS 避免深递归栈溢出）。
uint64_t countSCC(const DirectedCSR& csr) {
    const uint64_t n = csr.numNodes;
    if (n == 0) {
        return 0;
    }
    std::vector<int64_t> index(n, -1), lowlink(n, 0);
    std::vector<bool> onStack(n, false);
    std::vector<uint64_t> stack;
    int64_t nextIndex = 0;
    uint64_t sccCount = 0;
    // 显式栈帧：{node, nextNbrPos}
    for (uint64_t root = 0; root < n; ++root) {
        if (index[root] >= 0) {
            continue;
        }
        std::vector<std::pair<uint64_t, uint64_t>> frameStack;
        frameStack.emplace_back(root, csr.fwdOffsets[root]); // pos = 邻居区间起点！
        index[root] = lowlink[root] = nextIndex++;
        stack.push_back(root);
        onStack[root] = true;
        while (!frameStack.empty()) {
            auto& [u, pos] = frameStack.back();
            const uint64_t end = csr.fwdOffsets[u + 1];
            bool advanced = false;
            while (pos < end) {
                const uint64_t v = csr.fwdNeighbors[pos++];
                if (index[v] < 0) {
                    index[v] = lowlink[v] = nextIndex++;
                    stack.push_back(v);
                    onStack[v] = true;
                    frameStack.emplace_back(v, csr.fwdOffsets[v]);
                    advanced = true;
                    break;
                } else if (onStack[v]) {
                    lowlink[u] = std::min(lowlink[u], static_cast<int64_t>(index[v]));
                }
            }
            if (advanced) {
                continue;
            }
            if (lowlink[u] == index[u]) {
                uint64_t w = 0;
                do {
                    w = stack.back();
                    stack.pop_back();
                    onStack[w] = false;
                } while (w != u);
                ++sccCount;
            }
            frameStack.pop_back();
            if (!frameStack.empty()) {
                const auto parent = frameStack.back().first;
                lowlink[parent] =
                    std::min(lowlink[parent], static_cast<int64_t>(lowlink[u]));
            }
        }
    }
    return sccCount;
}

// WCC（无向连通分量数），BFS。
uint64_t countWCC(const DirectedCSR& csr) {
    const uint64_t n = csr.numNodes;
    std::vector<bool> visited(n, false);
    uint64_t wccCount = 0;
    for (uint64_t root = 0; root < n; ++root) {
        if (visited[root]) {
            continue;
        }
        ++wccCount;
        std::vector<uint64_t> queue{root};
        visited[root] = true;
        size_t head = 0;
        while (head < queue.size()) {
            const uint64_t u = queue[head++];
            for (auto it = csr.fwdNeighbors.begin() +
                     static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
                 it != csr.fwdNeighbors.begin() +
                           static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
                 ++it) {
                if (!visited[*it]) {
                    visited[*it] = true;
                    queue.push_back(*it);
                }
            }
            for (auto it = csr.bwdNeighbors.begin() +
                     static_cast<ptrdiff_t>(csr.bwdOffsets[u]);
                 it != csr.bwdNeighbors.begin() +
                           static_cast<ptrdiff_t>(csr.bwdOffsets[u + 1]);
                 ++it) {
                if (!visited[*it]) {
                    visited[*it] = true;
                    queue.push_back(*it);
                }
            }
        }
    }
    return wccCount;
}

void push(GraphSignature& sig, const char* name, double value) {
    sig.features.emplace_back(name, value);
}

} // namespace

GraphSignature computeSignature(const DirectedCSR& csr) {
    GraphSignature sig;
    const uint64_t n = csr.numNodes;
    const uint64_t e = csr.numEdges;

    push(sig, "num_nodes", static_cast<double>(n));
    push(sig, "num_edges", static_cast<double>(e));
    const double density = n <= 1 ? 0.0 : static_cast<double>(e) / (static_cast<double>(n) *
                                                                    static_cast<double>(n - 1));
    push(sig, "density", density);

    uint64_t inSum = 0, outSum = 0, inMax = 0, outMax = 0;
    uint64_t sources = 0, sinks = 0, selfLoops = 0;
    for (uint64_t u = 0; u < n; ++u) {
        const uint64_t outDeg = csr.fwdOffsets[u + 1] - csr.fwdOffsets[u];
        const uint64_t inDeg = csr.bwdOffsets[u + 1] - csr.bwdOffsets[u];
        inSum += inDeg;
        outSum += outDeg;
        inMax = std::max(inMax, inDeg);
        outMax = std::max(outMax, outDeg);
        if (inDeg == 0) {
            ++sources;
        }
        if (outDeg == 0) {
            ++sinks;
        }
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            if (*it == u) {
                ++selfLoops;
            }
        }
    }
    push(sig, "in_degree_mean", n == 0 ? 0.0 : static_cast<double>(inSum) / n);
    push(sig, "out_degree_mean", n == 0 ? 0.0 : static_cast<double>(outSum) / n);
    push(sig, "in_degree_max", static_cast<double>(inMax));
    push(sig, "out_degree_max", static_cast<double>(outMax));
    push(sig, "sources", static_cast<double>(sources));
    push(sig, "sinks", static_cast<double>(sinks));
    push(sig, "self_loops", static_cast<double>(selfLoops));

    // 双向边对数（multigraph：min(cnt(u->v), cnt(v->u))）。
    uint64_t bidirectional = 0;
    for (uint64_t u = 0; u < n; ++u) {
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            if (*it > u) {
                const uint64_t f = static_cast<uint64_t>(
                    csr.fwdOffsets[*it + 1] - csr.fwdOffsets[*it]);
                uint64_t back = 0;
                for (auto jt = csr.fwdNeighbors.begin() +
                         static_cast<ptrdiff_t>(csr.fwdOffsets[*it]);
                     jt != csr.fwdNeighbors.begin() +
                               static_cast<ptrdiff_t>(csr.fwdOffsets[*it + 1]);
                     ++jt) {
                    if (*jt == u) {
                        ++back;
                    }
                }
                bidirectional += std::min(f, back);
            }
        }
    }
    push(sig, "bidirectional_edges", static_cast<double>(bidirectional));

    push(sig, "scc_count", static_cast<double>(countSCC(csr)));
    push(sig, "wcc_count", static_cast<double>(countWCC(csr)));

    // DAG 判定 + 最长路径深度（复用 detectDAG，Kahn）。
    DirectedCSR dagCsr = csr;
    detectDAG(dagCsr);
    push(sig, "is_dag", dagCsr.isDAG ? 1.0 : 0.0);
    double maxDepth = 0.0;
    if (dagCsr.isDAG) {
        for (const auto d : dagCsr.depth) {
            maxDepth = std::max(maxDepth, static_cast<double>(d));
        }
    }
    push(sig, "max_depth", maxDepth);
    return sig;
}

} // namespace algo_extension
} // namespace lbug
