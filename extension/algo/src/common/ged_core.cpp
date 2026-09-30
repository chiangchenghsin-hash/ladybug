// GED 核心引擎：Hungarian (LSAP) + DFS-BB 精确图编辑距离。
// 机制移植自 NetworkX optimize_edit_paths（BSD-3，local networkx-networkx-3.6.1），
// 算法来源：Abu-Aisheh et al. 2015（DFS-BB）+ Riesen & Bunke 2009（bipartite 矩阵构造）。
#include "common/ged_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace lbug {
namespace algo_extension {

namespace {

constexpr double INF = std::numeric_limits<double>::infinity();

// 大数 M：所有真实成本之和 + 1（与 NetworkX 相同思路——禁止项替换为大数，
// 避免 Hungarian 算法对 inf 的处理，且保证最优解永远避开禁止项）。
double bigM(int64_t m, int64_t n, double delCost, double insCost) {
    return static_cast<double>(m) * delCost + static_cast<double>(n) * insCost + 1.0;
}

// CSR -> 有向边列表。
std::vector<std::pair<int64_t, int64_t>> toEdgeList(const DirectedCSR& csr) {
    std::vector<std::pair<int64_t, int64_t>> edges;
    edges.reserve(csr.numEdges);
    for (uint64_t u = 0; u < csr.numNodes; ++u) {
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            edges.emplace_back(static_cast<int64_t>(u), static_cast<int64_t>(*it));
        }
    }
    return edges;
}

// LSAP 状态：方阵 (m+n)^2 成本矩阵 + Hungarian 解。
// 行 = m 个 G1 元素 + n 个插入槽（对应 G2 元素）；列 = n 个 G2 元素 + m 个删除槽。
struct LsapState {
    std::vector<std::vector<double>> C;
    std::vector<int64_t> rowAss; // Hungarian 行分配
    double ls = 0.0;             // LSAP 值（松弛下界）
    int64_t m = 0;               // 未分配 G1 元素数
    int64_t n = 0;               // 未分配 G2 元素数
};

// 构造 (m+n)^2 成本矩阵并求解：subst 块 0、del/ins 对角、dummy-dummy 区 0、其余 M。
LsapState makeLsap(int64_t m, int64_t n, double delCost, double insCost) {
    const int64_t dim = m + n;
    const double M = bigM(m, n, delCost, insCost);
    LsapState st;
    st.m = m;
    st.n = n;
    st.C.assign(dim, std::vector<double>(dim, M));
    for (int64_t i = 0; i < m; ++i) {
        for (int64_t j = 0; j < n; ++j) {
            st.C[i][j] = 0.0; // 替换（结构 GED：任意配对 cost 0）
        }
    }
    for (int64_t i = 0; i < m; ++i) {
        st.C[i][n + i] = delCost; // 删除 G1 元素 i（只能用自己的槽）
    }
    for (int64_t j = 0; j < n; ++j) {
        st.C[m + j][j] = insCost; // 插入 G2 元素 j
    }
    // dummy-dummy 区必须为 0（NetworkX 初始化为 0）：LSAP 的自由配对位，
    // 保证松弛下界是"最少删插"而不是强制全删全插。
    for (int64_t j = 0; j < n; ++j) {
        for (int64_t i = 0; i < m; ++i) {
            st.C[m + j][n + i] = 0.0;
        }
    }
    st.rowAss = solveHungarian(st.C, st.ls);
    return st;
}

// 从当前矩阵构造删除配对 (i, j) 后的子矩阵。
// 删除的行/列：行 i 和行 m+j（若 j<n），列 j 和列 n+i（若 i<m）。
// solve=false 时仅构造矩阵（主分支用增量下界，见 SearchContext::getEditOps）。
LsapState makeReduced(const LsapState& prev, int64_t i, int64_t j, int64_t newM, int64_t newN,
    bool solve = true) {
    const int64_t dim = static_cast<int64_t>(prev.C.size());
    LsapState st;
    st.m = newM;
    st.n = newN;
    std::vector<bool> keepRow(dim, true), keepCol(dim, true);
    const int64_t delRow2 = j < prev.n ? prev.m + j : dim; // 越界 = 不删
    const int64_t delCol2 = i < prev.m ? prev.n + i : dim;
    if (i >= 0 && i < dim) {
        keepRow[i] = false;
    }
    if (delRow2 < dim) {
        keepRow[delRow2] = false;
    }
    if (j >= 0 && j < dim) {
        keepCol[j] = false;
    }
    if (delCol2 < dim) {
        keepCol[delCol2] = false;
    }
    const int64_t newDim = newM + newN;
    st.C.assign(newDim, std::vector<double>(newDim, 0.0));
    int64_t r = 0;
    for (int64_t k = 0; k < dim; ++k) {
        if (!keepRow[k]) {
            continue;
        }
        int64_t c = 0;
        for (int64_t l = 0; l < dim; ++l) {
            if (!keepCol[l]) {
                continue;
            }
            st.C[r][c] = prev.C[k][l];
            ++c;
        }
        ++r;
    }
    if (solve) {
        st.rowAss = solveHungarian(st.C, st.ls);
    }
    return st;
}

// 一个候选编辑操作。
struct EditOp {
    int64_t i, j; // 当前 Cv 矩阵中的配对（行, 列）
    double nodeCost; // 节点配对成本 Cv.C[i][j]（预读，避免状态矩阵赋值后的引用悬垂）
    LsapState cvIj;
    LsapState ceXy;
    double edgeCost; // 本次节点配对触发的边处理成本（localCe.ls）
    std::vector<std::pair<int64_t, int64_t>> xy; // 边操作（gIdx>=M 插入 h；hIdx>=N 删除 g）
};

// 编辑搜索状态。
struct State {
    std::vector<int64_t> pendingU; // 未分配 G1 节点（矩阵行 0..m-1）
    std::vector<int64_t> pendingV; // 未分配 G2 节点
    std::vector<int64_t> pendingG; // 未分配 G1 边索引
    std::vector<int64_t> pendingH; // 未分配 G2 边索引
    std::vector<std::pair<int64_t, int64_t>> matchedUv; // 已配对（-1 = 删/插）
    LsapState Cv;
    LsapState Ce;
};

struct SearchContext {
    const std::vector<std::pair<int64_t, int64_t>>& e1; // G1 边列表
    const std::vector<std::pair<int64_t, int64_t>>& e2;
    const GEDParams& params;
    double best = INF;
    uint64_t bestNodeOps = 0;
    uint64_t bestEdgeOps = 0;
    bool bestReached = false; // DFS 是否找到过完整解（区分"初始上界"与"搜索到的解"）
    bool timedOut = false;
    std::chrono::steady_clock::time_point startT{};

    // 边匹配：只处理端点涉及当前配对节点 u/v 或已配对节点的边。
    // 返回 (xy 配对列表, 局部 LSAP 状态)。xy 元素 = (g 索引, h 索引)，
    // g>=M 表示插入 h、h>=N 表示删除 g（NetworkX 的 ij 编码）。
    void matchEdges(int64_t u, int64_t v, State& st, std::vector<std::pair<int64_t, int64_t>>& xy,
        LsapState& localCe) {
        const int64_t M = static_cast<int64_t>(st.pendingG.size());
        const int64_t N = static_cast<int64_t>(st.pendingH.size());
        const bool substitutionPossible = M > 0 && N > 0;
        const bool firstNodeMatch = st.matchedUv.empty();

        std::vector<int64_t> gInd, hInd;
        if (firstNodeMatch && substitutionPossible) {
            // 第一个节点配对前不处理边（避免自环被过早删除/误匹配）。
        } else {
            for (int64_t k = 0; k < M; ++k) {
                const auto& g = e1[st.pendingG[k]];
                bool in = u >= 0 && g.first == u && g.second == u; // (u,u) 自环
                for (const auto& [p, q] : st.matchedUv) {
                    if (p >= 0 && ((g.first == p && g.second == u) ||
                                      (g.first == u && g.second == p) ||
                                      (g.first == p && g.second == p))) {
                        in = true;
                        break;
                    }
                }
                if (in) {
                    gInd.push_back(k);
                }
            }
            for (int64_t l = 0; l < N; ++l) {
                const auto& h = e2[st.pendingH[l]];
                bool in = v >= 0 && h.first == v && h.second == v;
                for (const auto& [p, q] : st.matchedUv) {
                    if (q >= 0 && ((h.first == q && h.second == v) ||
                                      (h.first == v && h.second == q) ||
                                      (h.first == q && h.second == q))) {
                        in = true;
                        break;
                    }
                }
                if (in) {
                    hInd.push_back(l);
                }
            }
        }

        const int64_t m = static_cast<int64_t>(gInd.size());
        const int64_t n = static_cast<int64_t>(hInd.size());
        const double Mbig = bigM(m, n, params.edgeWeight, params.edgeWeight);
        localCe.m = m;
        localCe.n = n;
        localCe.C.assign(m + n, std::vector<double>(m + n, Mbig));
        for (int64_t k = 0; k < m; ++k) {
            const auto& g = e1[st.pendingG[gInd[k]]];
            for (int64_t l = 0; l < n; ++l) {
                const auto& h = e2[st.pendingH[hInd[l]]];
                // 结构有效性（有向）：方向一致且端点配对一致才可替换。
                bool valid = false;
                for (const auto& [p, q] : st.matchedUv) {
                    if (p >= 0 && q >= 0 &&
                        ((g.first == p && g.second == u && h.first == q && h.second == v) ||
                            (g.first == u && g.second == p && h.first == v && h.second == q) ||
                            // 已匹配配对的自环替换 (p,p)<->(q,q)（NetworkX 缺失的形式）
                            (g.first == p && g.second == p && h.first == q && h.second == q))) {
                        valid = true;
                        break;
                    }
                }
                // 自环替换 (u,u)<->(v,v) 依赖"当前配对" (u,v) 本身（自环的两个端点
                // 是同一个节点，matchedUv 不含当前配对，故这里特判）。NetworkX 禁掉
                // 自环替换（实现简化，且其完成分支忽略未处理边会低估），标准 GED
                // 语义允许替换，我们放开以与 brute-force 语义一致。
                if (!valid && u >= 0 && v >= 0 && g.first == u && g.second == u &&
                    h.first == v && h.second == v) {
                    valid = true;
                }
                if (!valid) {
                    localCe.C[k][l] = Mbig; // 禁止替换（只能删/插）
                } else {
                    localCe.C[k][l] = 0.0; // 结构 GED：替换 cost 0
                }
            }
        }
        for (int64_t k = 0; k < m; ++k) {
            localCe.C[k][n + k] = params.edgeWeight; // 删除 g 边
        }
        for (int64_t l = 0; l < n; ++l) {
            localCe.C[m + l][l] = params.edgeWeight; // 插入 h 边
        }
        for (int64_t l = 0; l < n; ++l) {
            for (int64_t k = 0; k < m; ++k) {
                localCe.C[m + l][n + k] = 0.0; // dummy-dummy
            }
        }
        localCe.rowAss = solveHungarian(localCe.C, localCe.ls);
        // 提取非 dummy-dummy 配对为操作列表。
        xy.clear();
        for (int64_t k = 0; k < m + n; ++k) {
            const int64_t col = localCe.rowAss[k];
            if (k < m || col < n) {
                const int64_t gIdx = k < m ? gInd[k] : M + hInd[col];
                const int64_t hIdx = col < n ? hInd[col] : N + gInd[k];
                xy.emplace_back(gIdx, hIdx);
            }
        }
    }

    // 从当前 Ce 状态消耗 xy 中已匹配的边，返回新状态（重解 Hungarian）。
    LsapState reduceCe(const LsapState& ce, const std::vector<std::pair<int64_t, int64_t>>& xy) {
        const int64_t dim = static_cast<int64_t>(ce.C.size());
        std::vector<bool> keepRow(dim, true), keepCol(dim, true);
        int64_t removedG = 0, removedH = 0;
        for (const auto& [gIdx, hIdx] : xy) {
            if (gIdx < ce.m) {
                keepRow[gIdx] = false; // g 边被替换或删除
                ++removedG;
                keepCol[ce.n + gIdx] = false; // 其删除槽列
            }
            if (hIdx < ce.n) {
                keepCol[hIdx] = false; // h 边被替换或插入
                ++removedH;
                keepRow[ce.m + hIdx] = false; // 其插入槽行
            }
        }
        LsapState st;
        st.m = ce.m - removedG;
        st.n = ce.n - removedH;
        const int64_t newDim = st.m + st.n;
        st.C.assign(newDim, std::vector<double>(newDim, 0.0));
        int64_t r = 0;
        for (int64_t k = 0; k < dim; ++k) {
            if (!keepRow[k]) {
                continue;
            }
            int64_t c = 0;
            for (int64_t l = 0; l < dim; ++l) {
                if (!keepCol[l]) {
                    continue;
                }
                st.C[r][c] = ce.C[k][l];
                ++c;
            }
            ++r;
        }
        st.rowAss = solveHungarian(st.C, st.ls);
        return st;
    }

    // 剪枝：下界超过当前最优 / upperBound。
    bool prune(double cost) const {
        if (cost > best) {
            return true;
        }
        if (params.upperBound >= 0.0 && cost > params.upperBound) {
            return true;
        }
        return false;
    }

    // 候选编辑操作：主分支（LSAP 最优配对，增量下界）+ 其余候选（按列/行固定枚举）。
    // 所有 prune 判断都含 matchedCost（总成本下界）。
    void getEditOps(State& st, double matchedCost, std::vector<EditOp>& ops) {
        const int64_t m = static_cast<int64_t>(st.pendingU.size());
        const int64_t n = static_cast<int64_t>(st.pendingV.size());
        const int64_t dim = m + n;
        const auto& C = st.Cv.C;

        // 主分支：LSAP 解中第一个非 dummy-dummy 配对。
        int64_t i = -1, j = -1;
        for (int64_t k = 0; k < dim; ++k) {
            const int64_t c = st.Cv.rowAss[k];
            if (k < m || c < n) {
                i = k;
                j = c;
                break;
            }
        }
        if (i < 0) {
            return; // 全部 dummy-dummy（空状态，不会发生）
        }

        std::vector<std::pair<int64_t, int64_t>> xy;
        LsapState localCe;
        matchEdges(i < m ? st.pendingU[i] : -1, j < n ? st.pendingV[j] : -1, st, xy, localCe);

        // 重解换真实 LSAP 下界（比 NetworkX 的增量 reduce_ind 更紧；C++ 重解成本可接受）。
        // 注意：必须重解以填充 rowAss —— 下一层 getEditOps 依赖它做主分支选择。
        LsapState cvIj = makeReduced(st.Cv, i, j, m - (i < m ? 1 : 0), n - (j < n ? 1 : 0));
        LsapState ceXy = reduceCe(st.Ce, xy);
        ops.push_back({i, j, C[i][j], std::move(cvIj), std::move(ceXy), localCe.ls,
            std::move(xy)});

        // 其余候选：固定列（m<=n）或固定行（m>n）枚举。
        const int64_t fixedI = i, fixedJ = j;
        std::vector<EditOp> others;
        const auto processCandidate = [&](int64_t ci, int64_t cj) {
            if (prune(matchedCost + C[ci][cj] + st.Ce.ls)) {
                return;
            }
            LsapState cvIj2 = makeReduced(st.Cv, ci, cj, m - (ci < m ? 1 : 0), n - (cj < n ? 1 : 0));
            if (prune(matchedCost + C[ci][cj] + cvIj2.ls + st.Ce.ls)) {
                return;
            }
            std::vector<std::pair<int64_t, int64_t>> xy2;
            LsapState localCe2;
            matchEdges(ci < m ? st.pendingU[ci] : -1, cj < n ? st.pendingV[cj] : -1, st, xy2,
                localCe2);
            if (prune(matchedCost + C[ci][cj] + cvIj2.ls + localCe2.ls)) {
                return;
            }
            LsapState ceXy2 = reduceCe(st.Ce, xy2);
            if (prune(matchedCost + C[ci][cj] + cvIj2.ls + localCe2.ls + ceXy2.ls)) {
                return;
            }
            others.push_back({ci, cj, C[ci][cj], std::move(cvIj2), std::move(ceXy2), localCe2.ls,
                std::move(xy2)});
        };

        if (m <= n) {
            for (int64_t t = 0; t < dim; ++t) {
                if (t != fixedI && (t < m || t == m + fixedJ)) {
                    processCandidate(t, fixedJ);
                }
            }
        } else {
            for (int64_t t = 0; t < dim; ++t) {
                if (t != fixedJ && (t < n || t == n + fixedI)) {
                    processCandidate(fixedI, t);
                }
            }
        }
        // 按下界成本排序（NetworkX: key = edit_cost + Cv_ij.ls + Ce_xy.ls）。
        std::sort(others.begin(), others.end(), [](const EditOp& a, const EditOp& b) {
            return a.edgeCost + a.cvIj.ls + a.ceXy.ls < b.edgeCost + b.cvIj.ls + b.ceXy.ls;
        });
        for (auto& op : others) {
            ops.push_back(std::move(op));
        }
    }

    // DFS-BB 主循环。
    void dfs(State& st, double matchedCost, uint64_t nodeOps, uint64_t edgeOps) {
        if (timedOut) {
            return;
        }
        if (params.timeoutSec > 0.0) {
            const auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - startT).count() > params.timeoutSec) {
                timedOut = true;
                return;
            }
        }
        // 下界剪枝：已付成本 + 剩余顶点 LSAP 下界 + 剩余边 LSAP 下界。
        if (prune(matchedCost + st.Cv.ls + st.Ce.ls)) {
            return;
        }

        if (st.pendingU.empty() && st.pendingV.empty()) {
            // 完成：剩余边兜底（正常路径每条边都会在其端点配对时处理，但删除/插入
            // 节点的"双未配对端点"边（两端都未配对）永不进入任何 g_ind/h_ind，
            // 必须在这里全删/全插，否则低估）。
            matchedCost +=
                static_cast<double>(st.pendingG.size()) * params.edgeWeight +
                static_cast<double>(st.pendingH.size()) * params.edgeWeight;
            // 仍须检查 upperBound（prune 只查下界路径，完整解要显式查）。
            if (params.upperBound >= 0.0 && matchedCost > params.upperBound) {
                return;
            }
            // <= 而不是 <：初始 best 是平凡上界，最优解可能等于它（此时也要记录操作数）。
            if (matchedCost <= best) {
                best = matchedCost;
                bestNodeOps = nodeOps;
                bestEdgeOps = edgeOps;
                bestReached = true;
            }
            return;
        }

        std::vector<EditOp> ops;
        getEditOps(st, matchedCost, ops);
        const int64_t m = static_cast<int64_t>(st.pendingU.size());
        const int64_t n = static_cast<int64_t>(st.pendingV.size());
        for (const auto& op : ops) {
            // 注意：不能持有 st.Cv.C 的引用跨越 st.Cv = op.cvIj 赋值（悬垂）；
            // op.nodeCost 已在 getEditOps 中预读。
            if (prune(matchedCost + op.nodeCost + op.edgeCost + op.cvIj.ls + op.ceXy.ls)) {
                continue;
            }
            // 应用节点操作。
            int64_t u = -1, v = -1;
            if (op.i < m) {
                u = st.pendingU[op.i];
                st.pendingU.erase(st.pendingU.begin() + op.i);
            }
            if (op.j < n) {
                v = st.pendingV[op.j];
                st.pendingV.erase(st.pendingV.begin() + op.j);
            }
            st.matchedUv.emplace_back(u, v);
            // 应用边操作（xy：替换/删除/插入）。
            const auto savedG = st.pendingG;
            const auto savedH = st.pendingH;
            std::vector<int64_t> gToRemove = [&] {
                std::vector<int64_t> idx;
                for (const auto& [gIdx, hIdx] : op.xy) {
                    if (gIdx < static_cast<int64_t>(st.pendingG.size())) {
                        idx.push_back(gIdx);
                    }
                }
                return idx;
            }();
            std::vector<int64_t> hToRemove = [&] {
                std::vector<int64_t> idx;
                for (const auto& [gIdx, hIdx] : op.xy) {
                    if (hIdx < static_cast<int64_t>(st.pendingH.size())) {
                        idx.push_back(hIdx);
                    }
                }
                return idx;
            }();
            // 必须降序排序后移除（xy 配对顺序任意；升序 erase 会使后续索引失效，
            // 甚至 begin()+size() 越界 —— NetworkX 对 sortedx/sortedy 显式排序）。
            std::sort(gToRemove.begin(), gToRemove.end(), std::greater<int64_t>());
            std::sort(hToRemove.begin(), hToRemove.end(), std::greater<int64_t>());
            for (const auto idx : gToRemove) {
                st.pendingG.erase(st.pendingG.begin() + idx);
            }
            for (const auto idx : hToRemove) {
                st.pendingH.erase(st.pendingH.begin() + idx);
            }
            const auto savedCv = st.Cv;
            const auto savedCe = st.Ce;
            st.Cv = op.cvIj;
            st.Ce = op.ceXy;

            dfs(st, matchedCost + op.nodeCost + op.edgeCost, nodeOps + 1,
                edgeOps + static_cast<uint64_t>(op.xy.size()));

            // 回溯。
            st.Cv = savedCv;
            st.Ce = savedCe;
            st.pendingG = savedG;
            st.pendingH = savedH;
            st.matchedUv.pop_back();
            if (u >= 0) {
                st.pendingU.insert(st.pendingU.begin() + op.i, u);
            }
            if (v >= 0) {
                st.pendingV.insert(st.pendingV.begin() + op.j, v);
            }
        }
    }
};

} // namespace

// ---------- 公开接口 ----------

std::vector<int64_t> solveHungarian(const std::vector<std::vector<double>>& cost,
    double& totalCost) {
    // 经典 Kuhn-Munkres O(n^3)（CP-Algorithms 版），方阵最小化。
    const int64_t n = static_cast<int64_t>(cost.size());
    std::vector<int64_t> rowAss;
    if (n == 0) {
        totalCost = 0.0;
        return rowAss;
    }
    std::vector<double> u(n + 1, 0.0), v(n + 1, 0.0);
    std::vector<int64_t> p(n + 1, 0), way(n + 1, 0);
    for (int64_t i = 1; i <= n; ++i) {
        p[0] = i;
        int64_t j0 = 0;
        std::vector<double> minv(n + 1, INF);
        std::vector<bool> used(n + 1, false);
        do {
            used[j0] = true;
            const int64_t i0 = p[j0];
            double delta = INF;
            int64_t j1 = 0;
            for (int64_t j = 1; j <= n; ++j) {
                if (!used[j]) {
                    const double cur = cost[i0 - 1][j - 1] - u[i0] - v[j];
                    if (cur < minv[j]) {
                        minv[j] = cur;
                        way[j] = j0;
                    }
                    if (minv[j] < delta) {
                        delta = minv[j];
                        j1 = j;
                    }
                }
            }
            for (int64_t j = 0; j <= n; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int64_t j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0 != 0);
    }
    rowAss.assign(n, -1);
    for (int64_t j = 1; j <= n; ++j) {
        if (p[j] > 0) {
            rowAss[p[j] - 1] = j - 1;
        }
    }
    totalCost = 0.0;
    for (int64_t i = 0; i < n; ++i) {
        totalCost += cost[i][rowAss[i]];
    }
    return rowAss;
}

double trivialUpperBound(const DirectedCSR& g1, const DirectedCSR& g2, const GEDParams& params) {
    // 编辑路径：删 E1∖E2 边、插 E2∖E1 边、删/插节点差额 → 合法上界。
    // multigraph 语义：对称差按计数差（|c1-c2|），平行边算多份。
    auto countIn = [](const DirectedCSR& csr, uint64_t u, uint64_t v) -> uint64_t {
        if (u >= csr.numNodes) {
            return 0;
        }
        uint64_t cnt = 0;
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            if (*it == v) {
                ++cnt;
            }
        }
        return cnt;
    };
    uint64_t symDiff = 0;
    for (uint64_t u = 0; u < g1.numNodes; ++u) {
        for (auto it = g1.fwdNeighbors.begin() + static_cast<ptrdiff_t>(g1.fwdOffsets[u]);
             it != g1.fwdNeighbors.begin() + static_cast<ptrdiff_t>(g1.fwdOffsets[u + 1]); ++it) {
            const uint64_t c1 = countIn(g1, u, *it);
            const uint64_t c2 = countIn(g2, u, *it);
            symDiff += c1 > c2 ? c1 - c2 : 0; // g1 多余部分
        }
    }
    for (uint64_t u = 0; u < g2.numNodes; ++u) {
        for (auto it = g2.fwdNeighbors.begin() + static_cast<ptrdiff_t>(g2.fwdOffsets[u]);
             it != g2.fwdNeighbors.begin() + static_cast<ptrdiff_t>(g2.fwdOffsets[u + 1]); ++it) {
            const uint64_t c1 = countIn(g1, u, *it);
            const uint64_t c2 = countIn(g2, u, *it);
            symDiff += c2 > c1 ? c2 - c1 : 0; // g2 多余部分
        }
    }
    const int64_t nodeDiff =
        static_cast<int64_t>(g1.numNodes) > static_cast<int64_t>(g2.numNodes)
        ? static_cast<int64_t>(g1.numNodes - g2.numNodes)
        : static_cast<int64_t>(g2.numNodes - g1.numNodes);
    return static_cast<double>(symDiff) * params.edgeWeight +
           static_cast<double>(nodeDiff) * params.nodeWeight;
}

GEDResult computeGED(const DirectedCSR& g1, const DirectedCSR& g2, const GEDParams& params) {
    GEDResult res;
    if (g1.numNodes == 0 && g2.numNodes == 0) {
        res.found = true;
        res.distance = 0.0;
        return res;
    }
    // 快速路径的编辑操作数与 DFS 语义一致：同构 = 全部节点/边替换。
    const auto setIsoResult = [&] {
        res.found = true;
        res.distance = 0.0;
        res.nodeEdits = g1.numNodes;
        res.edgeEdits = g1.numEdges;
    };
    // 快速路径：平凡上界为 0 ⟺ multigraph 边集一致 + 节点数一致 ⟺ 同构 → GED = 0。
    // 注意：不用 VF2++ 做同构判定 —— 它是子图同构（存在性语义），平行边分布不同的
    // 两图会被误报（edgeExists 只查存在）。trivialUpperBound 的计数差语义更准确。

    const auto e1 = toEdgeList(g1);
    const auto e2 = toEdgeList(g2);
    SearchContext ctx{e1, e2, params};
    const double trivialUB = trivialUpperBound(g1, g2, params);
    // 剪枝上界：upperBound 提供时用它截断（其本身不保证是可行解，故不直接作为 best）。
    ctx.best = params.upperBound >= 0.0 ? std::min(trivialUB, params.upperBound) : trivialUB;
    ctx.startT = std::chrono::steady_clock::now();
    // 仅当平凡上界本身为 0（合法解 = 同构）时提前返回；upperBound 截断后的 best==0
    // 不代表存在 0 成本解。
    if (trivialUB == 0.0) {
        setIsoResult();
        return res;
    }

    State st;
    st.pendingU.resize(g1.numNodes);
    for (uint64_t i = 0; i < g1.numNodes; ++i) {
        st.pendingU[i] = static_cast<int64_t>(i);
    }
    st.pendingV.resize(g2.numNodes);
    for (uint64_t i = 0; i < g2.numNodes; ++i) {
        st.pendingV[i] = static_cast<int64_t>(i);
    }
    st.pendingG.resize(e1.size());
    for (size_t i = 0; i < e1.size(); ++i) {
        st.pendingG[i] = static_cast<int64_t>(i);
    }
    st.pendingH.resize(e2.size());
    for (size_t i = 0; i < e2.size(); ++i) {
        st.pendingH[i] = static_cast<int64_t>(i);
    }
    st.Cv = makeLsap(static_cast<int64_t>(g1.numNodes), static_cast<int64_t>(g2.numNodes),
        params.nodeWeight, params.nodeWeight);
    st.Ce = makeLsap(static_cast<int64_t>(e1.size()), static_cast<int64_t>(e2.size()),
        params.edgeWeight, params.edgeWeight);

    ctx.dfs(st, 0.0, 0, 0);

    res.timedOut = ctx.timedOut;
    // found 语义：存在 <= upperBound 的完整解。平凡上界本身是合法编辑路径，可直接兜底。
    const bool ubOk = params.upperBound < 0.0 || trivialUB <= params.upperBound;
    if (ctx.bestReached || ubOk) {
        res.found = true;
        res.distance = ctx.best;
        res.nodeEdits = ctx.bestNodeOps;
        res.edgeEdits = ctx.bestEdgeOps;
    }
    return res;
}

GEDResult approximateGED(const DirectedCSR& g1, const DirectedCSR& g2, const GEDParams& params) {
    GEDResult res;
    if (g1.numNodes == 0 && g2.numNodes == 0) {
        res.found = true;
        res.distance = 0.0;
        return res;
    }
    // 快速路径：平凡上界 0 ⟺ 同构（与 computeGED 一致）。
    if (trivialUpperBound(g1, g2, params) == 0.0) {
        res.found = true;
        res.distance = 0.0;
        res.nodeEdits = g1.numNodes;
        res.edgeEdits = g1.numEdges;
        return res;
    }

    const auto e1 = toEdgeList(g1);
    const auto e2 = toEdgeList(g2);
    const int64_t m = static_cast<int64_t>(g1.numNodes);
    const int64_t n = static_cast<int64_t>(g2.numNodes);
    const int64_t dim = m + n;
    const double M = bigM(m, n, params.nodeWeight, params.nodeWeight);

    // 1. 节点成本矩阵：subst[u][v] = 局部结构签名差异（出/入度差）。
    //    Riesen & Bunke 2009：用局部结构引导 Hungarian 分配结构相似的节点。
    //    注意：不能用"邻居绝对编号"签名——同构但编号不同的图签名不匹配；
    //    度签名是结构等价的（同构节点度相同 → 成本 0）。
    auto degreeDiff = [](const DirectedCSR& a, uint64_t u, const DirectedCSR& b, uint64_t v) {
        const int64_t aOut = static_cast<int64_t>(a.fwdOffsets[u + 1] - a.fwdOffsets[u]);
        const int64_t aIn = static_cast<int64_t>(a.bwdOffsets[u + 1] - a.bwdOffsets[u]);
        const int64_t bOut = static_cast<int64_t>(b.fwdOffsets[v + 1] - b.fwdOffsets[v]);
        const int64_t bIn = static_cast<int64_t>(b.bwdOffsets[v + 1] - b.bwdOffsets[v]);
        return static_cast<uint64_t>(std::abs(aOut - bOut) + std::abs(aIn - bIn));
    };

    std::vector<std::vector<double>> C(dim, std::vector<double>(dim, M));
    for (int64_t i = 0; i < m; ++i) {
        for (int64_t j = 0; j < n; ++j) {
            C[i][j] = static_cast<double>(degreeDiff(g1, i, g2, j));
        }
    }
    for (int64_t i = 0; i < m; ++i) {
        C[i][n + i] = params.nodeWeight; // 删除 G1_i
    }
    for (int64_t j = 0; j < n; ++j) {
        C[m + j][j] = params.nodeWeight; // 插入 G2_j
    }
    for (int64_t j = 0; j < n; ++j) {
        for (int64_t i = 0; i < m; ++i) {
            C[m + j][n + i] = 0.0; // dummy-dummy
        }
    }

    // 2. Hungarian 分配 → 节点映射（替换对）。
    double assignCost = 0.0;
    const auto rowAss = solveHungarian(C, assignCost);
    std::vector<int64_t> map(m, -1); // G1 节点 -> G2 节点（-1 = 删除）
    for (int64_t i = 0; i < dim; ++i) {
        const int64_t j = rowAss[i];
        if (i < m && j < n) {
            map[i] = j; // 替换（i>=m 的行是插入槽，j>=n 的列是删除槽）
        }
    }

    // 3. 按映射计算全局边成本（multigraph 计数差）+ 无前像边的插入。
    auto countIn = [](const DirectedCSR& csr, uint64_t u, uint64_t v) -> uint64_t {
        if (u >= csr.numNodes) {
            return 0;
        }
        uint64_t cnt = 0;
        for (auto it = csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u]);
             it != csr.fwdNeighbors.begin() + static_cast<ptrdiff_t>(csr.fwdOffsets[u + 1]);
             ++it) {
            if (*it == v) {
                ++cnt;
            }
        }
        return cnt;
    };
    double edgeCost = 0.0;
    for (int64_t u = 0; u < m; ++u) {
        for (int64_t v = 0; v < m; ++v) {
            const int64_t c1 = static_cast<int64_t>(countIn(g1, u, v));
            if (map[u] < 0 || map[v] < 0) {
                edgeCost += static_cast<double>(c1); // 未映射端点 → 全删
            } else {
                const int64_t c2 = static_cast<int64_t>(countIn(g2, map[u], map[v]));
                edgeCost += static_cast<double>(std::abs(c1 - c2));
            }
        }
    }
    for (int64_t x = 0; x < n; ++x) {
        bool preX = false;
        for (int64_t u = 0; u < m; ++u) {
            if (map[u] == x) {
                preX = true;
            }
        }
        for (int64_t y = 0; y < n; ++y) {
            bool preY = false;
            for (int64_t u = 0; u < m; ++u) {
                if (map[u] == y) {
                    preY = true;
                }
            }
            if (!preX || !preY) {
                edgeCost += static_cast<double>(countIn(g2, x, y)); // 一端无前像 → 全插
            }
        }
    }

    // 4. 总成本 = 节点删插 + 边成本（替换 0）。
    uint64_t delCount = 0;
    for (int64_t u = 0; u < m; ++u) {
        if (map[u] < 0) {
            ++delCount;
        }
    }
    const int64_t mapped = m - static_cast<int64_t>(delCount); // 替换数
    res.found = true;
    res.distance = edgeCost + static_cast<double>(delCount + (n - mapped)) * params.nodeWeight;
    // 节点操作数 = 替换 + 删除 + 插入 = m + (n - mapped)。
    res.nodeEdits = static_cast<uint64_t>(m) + static_cast<uint64_t>(n - mapped);
    res.edgeEdits = static_cast<uint64_t>(edgeCost); // weight=1 时操作数 == 成本
    return res;
}

} // namespace algo_extension
} // namespace lbug
