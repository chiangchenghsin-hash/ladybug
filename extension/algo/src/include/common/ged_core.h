#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "common/directed_csr.h"

namespace lbug {
namespace algo_extension {

// GED（图编辑距离）核心 — Sanfeliu & Fu 1983 定义的最小编辑代价。
//
// MVP 语义：纯结构有向图 GED（无属性代价）。
//   - 节点替换 cost 0（任意配对）；节点删除/插入 cost = nodeWeight
//   - 边替换 cost 0（仅当方向一致且端点配对一致）；边删除/插入 cost = edgeWeight
//
// 精确算法：DFS-BB（Abu-Aisheh et al. 2015，ICPRAM），机制与 NetworkX
// `optimize_edit_paths` 一致：
//   - 顶点/边成本矩阵 (m+n)^2：subst 块 + del/ins 对角 + dummy-dummy 区 0
//   - Hungarian (LSAP) 松弛解作为可剪枝下界（Riesen & Bunke 2009 的矩阵构造）
//   - 边匹配只允许端点已配对（且方向一致）的边互相替换
//   - 候选操作枚举：主分支 = LSAP 最优配对（增量下界），其余按列/行固定枚举后重解
//   - 分支限界：超过当前最优（严格递减）、upperBound 或 timeoutSec 即剪枝
//
// 快速路径：节点数与边数相等时先跑 VF2++ 子图同构（同构 → GED=0，复用
// SUBGRAPH_ISOMORPHISM 引擎）。
struct GEDParams {
    double nodeWeight = 1.0; // 节点删除/插入成本
    double edgeWeight = 1.0; // 边删除/插入成本
    double upperBound = -1.0; // <0：不限；否则剪掉超过该值的解
    double timeoutSec = 0.0; // <=0：不限；超时返回当前最优（approximate=true）
};

struct GEDResult {
    bool found = false; // 是否找到完整解（upperBound 剪空时 false）
    bool timedOut = false;
    double distance = -1.0; // 最小编辑代价
    uint64_t nodeEdits = 0; // 节点操作总数（subst + del + ins）
    uint64_t edgeEdits = 0; // 边操作总数
};

GEDResult computeGED(const DirectedCSR& g1, const DirectedCSR& g2,
    const GEDParams& params = {});

// Bipartite 近似 GED（Riesen & Bunke 2009）：节点成本矩阵含局部结构签名
// （出/入邻居 multiset 对称差），Hungarian 分配引导结构相似节点配对，
// 再按映射计算全局边成本。结果 = 合法编辑路径 → 是精确 GED 的上界。
// 用于大图 fallback（精确 DFS-BB 超时/超规模时）。
GEDResult approximateGED(const DirectedCSR& g1, const DirectedCSR& g2,
    const GEDParams& params = {});

// 暴露给单测：Kuhn-Munkres 方阵最小化分配，返回行分配 rowAss（行 i 分配列 rowAss[i]）。
// 注意：调用方需保证矩阵不含 inf（禁止项在构造时已替换为大数 M）。
std::vector<int64_t> solveHungarian(const std::vector<std::vector<double>>& cost,
    double& totalCost);

// 暴露给单测：边集对称差 + 节点数差的平凡上界（合法编辑路径成本）。
double trivialUpperBound(const DirectedCSR& g1, const DirectedCSR& g2,
    const GEDParams& params = {});

} // namespace algo_extension
} // namespace lbug
