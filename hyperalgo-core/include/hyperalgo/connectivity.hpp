// hyperalgo-core: 连通性算法(P0-1 双版本)
// 依据:P-1 SPEC v1.1 FRZ-00/05/06;升级计划 v1.3 P0-1;P-1.5 v1.2 §1.2
// 许可:BSD-3-Clause。s_connected_components 为 NWHypergraph(BSD-3)的参考重写
// (依赖剥离 NWGraph/TBB/C++20 → C++17,升级计划 v1.3 P2-2)。
#pragma once

#include "hyperalgo/core.hpp"
#include <unordered_set>
#include <limits>

namespace hyperalgo {

// ---- 并查集(内部工具)----
namespace detail {
struct UnionFind {
    std::vector<uint32_t> parent, rank;
    explicit UnionFind(uint32_t n) : parent(n), rank(n, 0) {
        std::iota(parent.begin(), parent.end(), 0u);
    }
    uint32_t find(uint32_t x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    }
    void unite(uint32_t a, uint32_t b) {
        a = find(a);
        b = find(b);
        if (a == b) return;
        if (rank[a] < rank[b]) std::swap(a, b);
        parent[b] = a;
        if (rank[a] == rank[b]) ++rank[a];
    }
};
} // namespace detail

// P0-1a s-walk 连通(退化对照版;与 HNX s_connected_components 同语义,可对拍):
// 两节点 s-邻接 ⟺ 共现于 ≥ s 条超边;s-连通块 = s-邻接图上的 BFS 连通块。
// 不判超边完整性(HNX 语义,见升级计划 v1.3 P0-1a)。
// 返回:每节点所属连通块 id(0..k-1)。
inline std::vector<uint32_t> s_connected_components(const HypergraphCSR& g, uint32_t s) {
    g.validate();
    if (s == 0)
        throw std::runtime_error("s_connected_components: s must be >= 1");
    const uint32_t n = (uint32_t)g.n_nodes;
    detail::UnionFind uf(n);
    // 共现计数:对每超边内成员两两计数(线图构造,参考 NWHypergraph s-linegraph naive 思路)
    std::unordered_map<uint64_t, uint32_t> shared;
    shared.reserve(g.edge_nodes.size());
    for (uint64_t e = 0; e < g.n_edges; ++e) {
        const uint64_t a = g.edge_ptr[e], b = g.edge_ptr[e + 1];
        for (uint64_t i = a; i < b; ++i) {
            const uint32_t u = g.edge_nodes[i];
            for (uint64_t j = i + 1; j < b; ++j) {
                const uint32_t v = g.edge_nodes[j];
                const uint64_t key = (uint64_t(u) << 32) | v; // u < v(成员升序)
                auto it = shared.find(key);
                if (it == shared.end()) shared.emplace(key, 1);
                else ++it->second;
            }
        }
    }
    for (const auto& [key, cnt] : shared)
        if (cnt >= s)
            uf.unite(uint32_t(key >> 32), uint32_t(key & 0xffffffffu));
    // 压缩
    std::vector<uint32_t> comp_id(n);
    std::unordered_map<uint32_t, uint32_t> id_of_root;
    for (uint32_t u = 0; u < n; ++u) {
        const uint32_t r = uf.find(u);
        auto it = id_of_root.find(r);
        if (it == id_of_root.end()) {
            const uint32_t cid = (uint32_t)id_of_root.size();
            id_of_root[r] = cid;
            comp_id[u] = cid;
        } else {
            comp_id[u] = it->second;
        }
    }
    return comp_id;
}

// P0-1b 齐套连通(自创主交付):仅存活超边(EdgeState.alive=1)参与 BFS/并查。
// 齐套连通块 = 存活超边子图上的连通块;节点 u,v 连通 ⟺ ∃ 存活超边同时含 u,v。
// 返回:每节点所属连通块 id;未被任何存活超边覆盖的节点返回 UINT32_MAX(不计入块)。
// 语义依据:升级计划 v1.3 P0-1b;布尔化产物 = FRZ-05 阶段 C。
inline std::vector<uint32_t> kitting_components(const HypergraphCSR& g, const NodeState& ns, const EdgeState& es) {
    g.validate();
    ns.validate(g.n_nodes);
    es.validate(g.n_edges);
    const uint32_t n = (uint32_t)g.n_nodes;
    detail::UnionFind uf(n);
    for (uint64_t e = 0; e < g.n_edges; ++e) {
        if (!es.alive[e]) continue;
        const uint64_t a = g.edge_ptr[e], b = g.edge_ptr[e + 1];
        if (a == b) continue;
        const uint32_t first = g.edge_nodes[a];
        for (uint64_t j = a + 1; j < b; ++j)
            uf.unite(first, g.edge_nodes[j]);
    }
    std::vector<uint32_t> comp_id(n, std::numeric_limits<uint32_t>::max());
    std::unordered_map<uint32_t, uint32_t> id_of_root;
    for (uint32_t u = 0; u < n; ++u) {
        if (!ns.alive[u]) continue;            // 死亡节点不进连通块
        if (g.edges_size(u) == 0) continue;    // 无任何超边关联(孤立)
        bool in_alive = false;
        for (uint64_t j = g.node_ptr[u]; j < g.node_ptr[u + 1]; ++j)
            if (es.alive[g.node_edges[j]]) { in_alive = true; break; }
        if (!in_alive) continue;               // 关联的超边全部失效
        const uint32_t r = uf.find(u);
        auto it = id_of_root.find(r);
        if (it == id_of_root.end()) {
            const uint32_t cid = (uint32_t)id_of_root.size();
            id_of_root[r] = cid;
            comp_id[u] = cid;
        } else {
            comp_id[u] = it->second;
        }
    }
    return comp_id;
}

// 辅助:统计连通块数量与大小分布(供 REST/验收用)
inline std::vector<uint64_t> component_sizes(const std::vector<uint32_t>& comp_id) {
    std::unordered_map<uint32_t, uint64_t> sz;
    for (uint32_t c : comp_id)
        if (c != std::numeric_limits<uint32_t>::max())
            ++sz[c];
    std::vector<uint64_t> out;
    out.reserve(sz.size());
    for (const auto& [c, s] : sz) out.push_back(s);
    std::sort(out.begin(), out.end(), std::greater<uint64_t>());
    return out;
}

} // namespace hyperalgo
