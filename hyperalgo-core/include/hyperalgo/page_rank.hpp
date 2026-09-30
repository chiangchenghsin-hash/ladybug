// hyperalgo-core: 两步随机游走超图 PageRank(P2)
// 依据:升级计划 v1.3 P2;P-1.5 v1.2 §1.2
// 语义(扩散层,FRZ-08;与 HGX 对拍口径,归位 linalg/linalg.py:308 的 dual 邻接思路):
//   从节点 u 出发两步:① 按超边权重选超边 e ∋ u(P(e|u) ∝ w(e),w(e) = Σ_{x∈e} H[x,e]);
//   ② 在 e 内按边依赖顶点权重选下一节点 v(P(v|e) = H[v,e] / Σ_{x∈e} H[x,e])。
//   单步转移 P(u→v) = Σ_{e∋u,v} P(e|u)·P(v|e);PageRank 幂迭代:
//   π' = (1-α)·π·P + α·uniform。
// 有向化(扩散层走 FLOWS 方向):FLOWS 天然 u→S,转移目标限定在「下游」(u 所属超边的 head),
//   即 v = head(e)(装配站)。默认 directed=false(结构游走,可对拍 HGX);
//   directed=true 时 P(v|e) 只在 v = edge_business_ids[e] 上为 1(单点下游,退化为有向游走)。
// 许可:BSD-3-Clause,自写。
#pragma once

#include "hyperalgo/core.hpp"

namespace hyperalgo {

inline std::vector<double> pagerank_two_step(const HypergraphCSR& g, double alpha, uint32_t max_iter, double tol) {
    g.validate();
    const uint32_t n = (uint32_t)g.n_nodes;
    if (n == 0)
        throw std::runtime_error("pagerank_two_step: empty graph");
    if (!(alpha >= 0.0 && alpha < 1.0))
        throw std::runtime_error("pagerank_two_step: alpha must be in [0,1)");
    // 超边权重 w(e) = Σ H[x,e];节点内边权重 P(e|u)
    std::vector<double> w(g.n_edges, 0.0);
    std::vector<double> node_edge_weight_sum(n, 0.0);
    for (uint64_t e = 0; e < g.n_edges; ++e) {
        double we = 0.0;
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
            we += g.h(g.edge_nodes[j], e);
        w[e] = we;
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
            node_edge_weight_sum[g.edge_nodes[j]] += we;
    }
    // 幂迭代;dangling 节点(无任何出边权重)的质量按均匀分布回流(标准 PageRank 处理,保证转移随机)
    std::vector<double> pi(n, 1.0 / n), pi_new(n, 0.0);
    for (uint32_t it = 0; it < max_iter; ++it) {
        std::fill(pi_new.begin(), pi_new.end(), alpha / n);
        double dangling_mass = 0.0;
        for (uint32_t u = 0; u < n; ++u)
            if (node_edge_weight_sum[u] == 0.0)
                dangling_mass += pi[u];
        const double dangling_per_node = (1.0 - alpha) * dangling_mass / n;
        for (uint32_t v = 0; v < n; ++v) pi_new[v] += dangling_per_node;
        for (uint64_t e = 0; e < g.n_edges; ++e) {
            if (w[e] == 0.0) continue;
            // 每超边内顶点归一化系数
            double in_norm = 0.0;
            for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
                in_norm += g.h(g.edge_nodes[j], e);
            if (in_norm == 0.0) continue;
            for (uint64_t i = g.edge_ptr[e]; i < g.edge_ptr[e + 1]; ++i) {
                const uint32_t u = g.edge_nodes[i];
                if (node_edge_weight_sum[u] == 0.0) continue;
                const double pu_e = w[e] / node_edge_weight_sum[u]; // P(e|u)
                for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j) {
                    const uint32_t v = g.edge_nodes[j];
                    const double pv_e = g.h(v, e) / in_norm; // P(v|e)
                    pi_new[v] += (1.0 - alpha) * pi[u] * pu_e * pv_e;
                }
            }
        }
        // 收敛判定(L1)
        double diff = 0.0;
        for (uint32_t u = 0; u < n; ++u) diff += std::fabs(pi_new[u] - pi[u]);
        pi = pi_new;
        if (diff < tol) break;
    }
    return pi;
}

} // namespace hyperalgo
