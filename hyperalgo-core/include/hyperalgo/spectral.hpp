// hyperalgo-core: 超图拉普拉斯 / Fiedler / 候选加边评分(P1c)
// 依据:升级计划 v1.3 P1c;P-1.5 v1.2 §1.2
// 定义冻结(v1,待 P1c memo 终审):Zhou 2006 归一化拉普拉斯
//   L = I - D_v^{-1/2} H W D_e^{-1} H^T D_v^{-1/2}
//   H = 0/1 关联矩阵(与 HGX/XGI laplacian 同口径,双库交叉对拍),W=diag(w(e)),默认 w(e)=1;
//   D_e = 超边大小,D_v = 节点超度(加权)。
// 谱求解:核心零依赖,自带稠密循环 Jacobi(参考 libalgo spectral_partitioning.cpp 自写
//   稠密 Jacobi 思路,repo 内既有实现);绑定层可注入 Eigen/Spectra 解替代(升级计划 P1c)。
// 候选评分(加边 (u,v) 的 Fiedler 下降一阶近似)= (f[u]-f[v])²,越大越优。
// 许可:BSD-3-Clause,自写。
#pragma once

#include "hyperalgo/core.hpp"

namespace hyperalgo {

// Zhou 2006 归一化超图拉普拉斯(稠密 n×n)
inline std::vector<std::vector<double>> hypergraph_laplacian(const HypergraphCSR& g) {
    g.validate();
    const uint32_t n = (uint32_t)g.n_nodes;
    const uint32_t m = (uint32_t)g.n_edges;
    // D_e(超边大小),D_v(加权超度),H 0/1 关联
    std::vector<double> de(m, 0.0), dv(n, 0.0);
    for (uint32_t e = 0; e < m; ++e) {
        de[e] = (double)g.members_size(e);
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
            dv[g.edge_nodes[j]] += 1.0; // w(e)=1
    }
    std::vector<std::vector<double>> L(n, std::vector<double>(n, 0.0));
    // B = H W D_e^{-1} H^T;L = I - D_v^{-1/2} B D_v^{-1/2}
    // B[u,v] = Σ_{e ∋ u,v} w(e)/de[e]
    for (uint32_t u = 0; u < n; ++u)
        L[u][u] = 1.0;
    for (uint32_t e = 0; e < m; ++e) {
        if (de[e] == 0.0) continue;
        const double we = 1.0 / de[e];
        for (uint64_t i = g.edge_ptr[e]; i < g.edge_ptr[e + 1]; ++i) {
            const uint32_t u = g.edge_nodes[i];
            for (uint64_t j = i; j < g.edge_ptr[e + 1]; ++j) {
                const uint32_t v = g.edge_nodes[j];
                const double t = we / std::sqrt(std::max(dv[u], 1e-300) * std::max(dv[v], 1e-300));
                L[u][v] -= t;
                if (u != v) L[v][u] -= t;
            }
        }
    }
    return L;
}

namespace detail {
// 稠密对称矩阵循环 Jacobi 特征分解(自写;返回特征值升序 + 对应特征向量列)
inline void jacobi_eigen(const std::vector<std::vector<double>>& A,
    std::vector<double>& eigenvals, std::vector<std::vector<double>>& eigenvecs,
    uint32_t max_sweeps = 100, double tol = 1e-12) {
    const uint32_t n = (uint32_t)A.size();
    auto M = A;
    eigenvecs.assign(n, std::vector<double>(n, 0.0));
    for (uint32_t i = 0; i < n; ++i) eigenvecs[i][i] = 1.0;
    for (uint32_t sweep = 0; sweep < max_sweeps; ++sweep) {
        // 找最大非对角元
        double off = 0.0;
        uint32_t p = 0, q = 1;
        for (uint32_t i = 0; i < n; ++i)
            for (uint32_t j = i + 1; j < n; ++j)
                if (std::fabs(M[i][j]) > off) { off = std::fabs(M[i][j]); p = i; q = j; }
        if (off < tol) break;
        // 旋转
        double theta = (M[q][q] - M[p][p]) / (2.0 * M[p][q]);
        double t = (theta >= 0 ? 1.0 : -1.0) / (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
        double c = 1.0 / std::sqrt(t * t + 1.0);
        double s = t * c;
        for (uint32_t i = 0; i < n; ++i) {
            double aip = M[i][p], aiq = M[i][q];
            M[i][p] = c * aip - s * aiq;
            M[i][q] = s * aip + c * aiq;
        }
        for (uint32_t i = 0; i < n; ++i) {
            double api = M[p][i], aqi = M[q][i];
            M[p][i] = c * api - s * aqi;
            M[q][i] = s * api + c * aqi;
        }
        for (uint32_t i = 0; i < n; ++i) {
            double vip = eigenvecs[i][p], viq = eigenvecs[i][q];
            eigenvecs[i][p] = c * vip - s * viq;
            eigenvecs[i][q] = s * vip + c * viq;
        }
    }
    eigenvals.resize(n);
    for (uint32_t i = 0; i < n; ++i) eigenvals[i] = M[i][i];
    // 升序排序(带特征向量列)
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return eigenvals[a] < eigenvals[b]; });
    std::vector<double> ev2 = eigenvals;
    std::vector<std::vector<double>> V2(n, std::vector<double>(n));
    for (uint32_t r = 0; r < n; ++r)
        for (uint32_t i = 0; i < n; ++i)
            V2[r][i] = eigenvecs[r][order[i]];
    for (uint32_t i = 0; i < n; ++i) eigenvals[i] = ev2[order[i]];
    eigenvecs = V2;
}
} // namespace detail

// Fiedler 向量(第二小特征值特征向量;Zhou 归一化拉普拉斯)
inline std::vector<double> fiedler_vector(const HypergraphCSR& g, uint32_t max_sweeps = 100, double tol = 1e-12) {
    const auto L = hypergraph_laplacian(g);
    std::vector<double> evals;
    std::vector<std::vector<double>> evecs;
    detail::jacobi_eigen(L, evals, evecs, max_sweeps, tol);
    const uint32_t n = (uint32_t)g.n_nodes;
    if (n < 2)
        throw std::runtime_error("fiedler_vector: need n_nodes >= 2");
    std::vector<double> f(n);
    for (uint32_t i = 0; i < n; ++i) f[i] = evecs[i][1]; // 第二列 = 第二小特征值
    return f;
}

// 候选加边评分:score(u,v) = (f[u]-f[v])²(越大 = Fiedler 预期下降越多,越优)
inline std::vector<double> fiedler_candidate_scores(const HypergraphCSR& g,
    const std::vector<std::pair<uint32_t, uint32_t>>& candidates, const std::vector<double>& fiedler) {
    g.validate();
    if (fiedler.size() != g.n_nodes)
        throw std::runtime_error("fiedler_candidate_scores: vector size mismatch");
    std::vector<double> scores;
    scores.reserve(candidates.size());
    for (const auto& [u, v] : candidates) {
        if (u >= g.n_nodes || v >= g.n_nodes)
            throw std::runtime_error("fiedler_candidate_scores: candidate out of range");
        const double d = fiedler[u] - fiedler[v];
        scores.push_back(d * d);
    }
    return scores;
}

// 契约签名版(P-1.5 v1.2):内部自算 Fiedler 向量
inline std::vector<double> fiedler_candidate_scores(const HypergraphCSR& g,
    const std::vector<std::pair<uint32_t, uint32_t>>& candidates) {
    return fiedler_candidate_scores(g, candidates, fiedler_vector(g));
}

} // namespace hyperalgo
