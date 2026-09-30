// hyperalgo-core: B-回路检测(P0-2)
// 依据:P-1 SPEC v1.1 FRZ-08(Ausiello 文献口径);升级计划 v1.3 P0-2
// 语义:有向超边 head(e_S)={S}(edge_business_ids[e]),tail(e_S)=成员。
//   超边级有向图:e_a → e_b ⟺ head(e_a) ∈ tail(e_b)(尾头衔接)。
//   B-回路 = 超边序列(e_1..e_k)超边互异、尾头衔接、首尾闭合(有向简单环)。
// 环检出无 oracle(已核实 HALP 无环检测模块),场景互证(升级计划 v1.3 §8.1)。
// 许可:BSD-3-Clause,自写(按 Ausiello et al. 有向超图定义)。
#pragma once

#include "hyperalgo/core.hpp"
#include <deque>
#include <functional>

namespace hyperalgo {

namespace detail {

// Tarjan SCC(返回每个顶点的 SCC id)
inline void tarjan_scc(const std::vector<std::vector<uint32_t>>& adj, std::vector<uint32_t>& scc_id) {
    const uint32_t n = (uint32_t)adj.size();
    scc_id.assign(n, 0);
    std::vector<uint32_t> index(n, 0xffffffffu), low(n), stk;
    std::vector<uint8_t> on_stack(n, 0);
    uint32_t idx = 0, scc_cnt = 0;
    std::function<void(uint32_t)> dfs = [&](uint32_t v) {
        index[v] = low[v] = idx++;
        stk.push_back(v);
        on_stack[v] = 1;
        for (uint32_t w : adj[v]) {
            if (index[w] == 0xffffffffu) {
                dfs(w);
                low[v] = std::min(low[v], low[w]);
            } else if (on_stack[w]) {
                low[v] = std::min(low[v], index[w]);
            }
        }
        if (low[v] == index[v]) {
            while (true) {
                uint32_t w = stk.back();
                stk.pop_back();
                on_stack[w] = 0;
                scc_id[w] = scc_cnt;
                if (w == v) break;
            }
            ++scc_cnt;
        }
    };
    for (uint32_t v = 0; v < n; ++v)
        if (index[v] == 0xffffffffu)
            dfs(v);
}

// Johnson(1975)简单有向环枚举(经典伪代码):s = 环中最小顶点,只输出 s 为最小顶点的环,每环恰好一次。
// 顶点为全局 id;调用方传入的局部图应保证相邻 w 均 ≥ s(剪枝由调用方完成)。
inline void johnson_cycles(uint32_t s, const std::vector<std::vector<uint32_t>>& adj,
    std::vector<std::vector<uint32_t>>& out) {
    const uint32_t n = (uint32_t)adj.size();
    std::vector<uint8_t> blocked(n, 0);
    std::vector<std::vector<uint32_t>> B(n);
    std::vector<uint32_t> stack;

    std::function<void(uint32_t)> unblock = [&](uint32_t u) {
        blocked[u] = 0;
        for (uint32_t w : B[u])
            if (blocked[w])
                unblock(w);
        B[u].clear();
    };
    std::function<bool(uint32_t)> circuit = [&](uint32_t v) -> bool {
        bool found = false;
        stack.push_back(v);
        blocked[v] = 1;
        for (uint32_t w : adj[v]) {
            if (w < s) continue; // 经典算法:忽略小于 s 的顶点
            if (w == s) {
                out.push_back(stack); // 环 = stack + s(闭合),超边互异由 Johnson 保证
                found = true;
            } else if (!blocked[w]) {
                if (circuit(w))
                    found = true;
            }
        }
        if (found)
            unblock(v);
        else
            for (uint32_t w : adj[v])
                if (w >= s && std::find(B[v].begin(), B[v].end(), w) == B[v].end())
                    B[v].push_back(w);
        stack.pop_back();
        return found;
    };

    // 经典外层:每轮清空 blocked/B,只调用 circuit(s)
    std::fill(blocked.begin(), blocked.end(), 0);
    for (auto& b : B) b.clear();
    circuit(s);
}

} // namespace detail

// B-回路检测:返回超边序列(每条 = 超边紧凑索引列表,超边互异,首尾闭合;闭合点 = 序列首)。
// 仅对 FLOWS 方向(include_signals=false,FRZ-08 v1.1 冻结默认)计算。
inline std::vector<std::vector<uint32_t>> b_cycles(const HypergraphCSR& g) {
    g.validate();
    const uint32_t m = (uint32_t)g.n_edges;
    if (m == 0) return {};
    // 超边级有向邻接:e_a → e_b ⟺ S_a ∈ members(e_b)(尾头衔接,FRZ-08)
    std::vector<std::vector<uint32_t>> adj(m);
    for (uint32_t e = 0; e < m; ++e) {
        const uint32_t s = g.edge_business_ids[e];
        if (s >= g.n_nodes)
            throw std::runtime_error("b_cycles: edge_business_ids out of range");
        for (uint64_t j = g.node_ptr[s]; j < g.node_ptr[s + 1]; ++j) {
            const uint32_t e2 = g.node_edges[j];
            if (e2 != e)
                adj[e].push_back(e2);
        }
        std::sort(adj[e].begin(), adj[e].end());
        adj[e].erase(std::unique(adj[e].begin(), adj[e].end()), adj[e].end());
    }
    // 非平凡 SCC 才有环;单点 SCC 无自环(FRZ-01 禁止 S∈e_S)
    std::vector<uint32_t> scc_id;
    detail::tarjan_scc(adj, scc_id);
    std::unordered_map<uint32_t, std::vector<uint32_t>> members_of_scc;
    for (uint32_t e = 0; e < m; ++e)
        members_of_scc[scc_id[e]].push_back(e);
    std::vector<std::vector<uint32_t>> cycles;
    for (const auto& [sid, verts] : members_of_scc) {
        if (verts.size() < 2)
            continue;
        // 局部图(保留全局顶点 id,边只留 SCC 内)
        std::vector<std::vector<uint32_t>> sub(m);
        for (uint32_t v : verts) {
            sub[v].clear();
            for (uint32_t w : adj[v])
                if (scc_id[w] == sid)
                    sub[v].push_back(w);
            std::sort(sub[v].begin(), sub[v].end());
        }
        const uint32_t min_v = *std::min_element(verts.begin(), verts.end());
        detail::johnson_cycles(min_v, sub, cycles);
    }
    return cycles;
}

} // namespace hyperalgo
