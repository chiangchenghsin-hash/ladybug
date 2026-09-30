// hyperalgo-core: (k,s)-core 双参数剥离(P1b)
// 依据:升级计划 v1.3 P1b(s 语义冻结 = 语义 B,对齐 Rust hypergraph get_core,MIT);
//   P-1.5 v1.2 §1.2。无 oracle 时代仅场景互证;v1.3 起可对 Rust get_core 交叉验证。
// 语义(Batagelj–Zaversnik 式同一轮内同时剥):
//   每轮:移除「超度 < k」的节点(仍存在超边中的度)与「边大小 < s」的超边(仍存在成员数),
//   直到稳定;层号 = 移除轮次(第 0 轮最先掉的最外层)。内层 = 平台件/关键自制件。
// 注:本实现按结构语义(Rust 口径)跑在完整 CSR 上;若需 alive 感知的度,
//   绑定层可先按 EdgeState 过滤构造子图 CSR(构造层职责,FRZ-05 阶段 C)。
// 许可:BSD-3-Clause;剥离框架思路参考 Rust hypergraph get_core(MIT)。
#pragma once

#include "hyperalgo/core.hpp"

namespace hyperalgo {

inline std::vector<std::vector<uint32_t>> ks_core_layers(const HypergraphCSR& g, uint32_t k, uint32_t s) {
    g.validate();
    if (k == 0 || s == 0)
        throw std::runtime_error("ks_core_layers: k and s must be >= 1");
    const uint32_t n = (uint32_t)g.n_nodes;
    const uint32_t m = (uint32_t)g.n_edges;
    std::vector<uint8_t> node_alive(n, 1), edge_alive(m, 1);
    std::vector<std::vector<uint32_t>> layers;
    while (true) {
        // 计算当前度与大小
        std::vector<uint32_t> deg(n, 0);
        for (uint32_t e = 0; e < m; ++e) {
            if (!edge_alive[e]) continue;
            for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
                if (node_alive[g.edge_nodes[j]])
                    ++deg[g.edge_nodes[j]];
        }
        // 本轮违反者
        std::vector<uint32_t> bad_nodes, bad_edges;
        for (uint32_t u = 0; u < n; ++u)
            if (node_alive[u] && deg[u] < k)
                bad_nodes.push_back(u);
        for (uint32_t e = 0; e < m; ++e) {
            if (!edge_alive[e]) continue;
            uint64_t sz = 0;
            for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
                if (node_alive[g.edge_nodes[j]]) ++sz;
            if (sz < s)
                bad_edges.push_back(e);
        }
        if (bad_nodes.empty() && bad_edges.empty())
            break; // 稳定 → 剩余为核心
        if (!bad_nodes.empty())
            layers.push_back(bad_nodes); // 只输出节点层(契约:返回每层节点;仅边删除轮不产生节点层)
        for (uint32_t u : bad_nodes) node_alive[u] = 0;
        for (uint32_t e : bad_edges) edge_alive[e] = 0;
        // 注:同轮内先删节点后删边;若边因节点删除而 <s 本已入 bad_edges(按删除前状态计),
        //   语义冻结:按轮首快照判定(报告 P1-2 附注;对拍时按 Rust 语义核对)。
    }
    return layers;
}

} // namespace hyperalgo
