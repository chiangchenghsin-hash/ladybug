// hyperalgo-core: 贪心击垮集(P1a)
// 依据:升级计划 v1.3 P1a(v1.3 布尔化重定义,P1-4 落点);P-1 SPEC v1.1 FRZ-00/05
// 语义:在布尔化超图(存活超边集合,EdgeState)上,最小节点集 H 使「击中后存活超边
//   覆盖塌缩」——每条存活超边被 H 命中(含其成员之一)。set cover 贪心,理论近似 1+ln n。
// 需求组契约:同料多供/替代成员共享需求组时,绑定层需先将组内成员**收缩为一个代表节点**
//   (构造参数 material_groups,FRZ-00 条款 3)再调本算法——否则击中组内单点不击垮该组。
// 输出:选中的节点内部 id(按选择顺序);绑定层经 IdMap 回料号业务 id(FRZ-00 条款 2)。
// 许可:BSD-3-Clause,自写。
#pragma once

#include "hyperalgo/core.hpp"

namespace hyperalgo {

inline std::vector<uint32_t> hitting_set_greedy(const HypergraphCSR& g, const NodeState& ns, const EdgeState& es) {
    g.validate();
    ns.validate(g.n_nodes);
    es.validate(g.n_edges);
    const uint32_t n = (uint32_t)g.n_nodes;
    const uint32_t m = (uint32_t)g.n_edges;
    std::vector<uint8_t> edge_covered(m, 0);
    uint64_t uncovered = 0;
    for (uint32_t e = 0; e < m; ++e)
        if (es.alive[e]) ++uncovered;
    std::vector<uint8_t> node_selected(n, 0);
    std::vector<uint32_t> result;
    // 节点覆盖集:节点 u 覆盖的存活超边 = 含 u 的存活超边(node_edges)
    while (uncovered > 0) {
        uint32_t best = n;
        uint64_t best_cover = 0;
        for (uint32_t u = 0; u < n; ++u) {
            if (node_selected[u]) continue;
            uint64_t cover = 0;
            for (uint64_t j = g.node_ptr[u]; j < g.node_ptr[u + 1]; ++j) {
                const uint32_t e = g.node_edges[j];
                if (es.alive[e] && !edge_covered[e]) ++cover;
            }
            if (cover > best_cover) { best_cover = cover; best = u; }
        }
        if (best == n)
            throw std::runtime_error("hitting_set_greedy: no covering node for remaining alive edges");
        node_selected[best] = 1;
        result.push_back(best);
        for (uint64_t j = g.node_ptr[best]; j < g.node_ptr[best + 1]; ++j) {
            const uint32_t e = g.node_edges[j];
            if (es.alive[e] && !edge_covered[e]) {
                edge_covered[e] = 1;
                --uncovered;
            }
        }
    }
    return result;
}

} // namespace hyperalgo
