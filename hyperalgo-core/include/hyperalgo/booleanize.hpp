// hyperalgo-core: FRZ-05 需求组阈值布尔化管线(阶段 B→C,绑定层共用)
// 依据:P-1 SPEC v1.1 FRZ-00 条款 3/FRZ-05;P-1.5 v1.2(EdgeState 由绑定层算,核心算法不重算)
// 语义:需求组划分(料号按 material_id 分组共享需求 + 站成员各一组 + extra_groups 显式并组),
//   组存活 = 组内任一成员 alive;g(e) = 组数;e_alive ⟺ 活组数 ≥ k_e(默认 k_e = g(e))。
// 反例锁定(样例 C1/C2):同料双供 = 同料成员并组共享需求(g=2,k_e=2;缺 1 家仍齐);
//   异料单供 = 各料独立组(g=4,k_e=4;只到 1 种料不齐)。
// 许可:BSD-3-Clause,自写(HGX metadata_filters/statistical_filters 思路,未照抄)。
#pragma once

#include "hyperalgo/core.hpp"
#include <map>
#include <optional>

namespace hyperalgo {

struct BooleanizeParams {
    // 与 edge_nodes 等长;成员料号 id(UINT32_MAX = 非料号,即站成员,各自独立需求组)
    std::vector<uint32_t> material_of_member;
    // 显式并组(替代/同料多供):每组为成员内部 id 列表,组内成员并入同一需求组
    std::vector<std::vector<uint32_t>> extra_groups;
    // k_e 覆盖(全局调试用):缺省 = g(e)(FRZ-05 默认值)
    std::optional<uint32_t> k_e_override = std::nullopt;
    // 逐超边 k_e 覆盖(FRZ-10 参数通道;空 = 未指定,元素 0 = 该边未指定)
    std::vector<uint32_t> k_e_per_edge;
};

// 每超边布尔化统计(FRZ-05 阶段 B:分组/活组/阈值,供 CLI 对拍与 REST 诊断)
struct EdgeGroupStat {
    uint32_t groups = 0;       // g(e) = 需求组数
    uint32_t k_e = 0;          // 阈值(默认 g(e),可被 k_e_override 覆盖)
    uint32_t alive_groups = 0; // 活需求组数
    uint8_t alive = 0;         // e_alive ⟺ alive_groups >= k_e
};

inline std::vector<EdgeGroupStat> edge_group_stats(const HypergraphCSR& g, const NodeState& ns,
    const BooleanizeParams& p) {
    g.validate();
    ns.validate(g.n_nodes);
    if (p.material_of_member.size() != g.edge_nodes.size())
        throw std::runtime_error("booleanize: material_of_member size mismatch");
    if (!p.k_e_per_edge.empty() && p.k_e_per_edge.size() != g.n_edges)
        throw std::runtime_error("booleanize: k_e_per_edge size mismatch");
    std::vector<EdgeGroupStat> out(g.n_edges);
    for (uint64_t e = 0; e < g.n_edges; ++e) {
        const uint64_t a = g.edge_ptr[e], b = g.edge_ptr[e + 1];
        // 组键:料号成员 = material_id;站成员 = UINT32_MAX - 边内位置(保证独立且唯一)
        auto key_of_pos = [&](uint64_t j) -> uint32_t {
            const uint32_t mat = p.material_of_member[j];
            return (mat == std::numeric_limits<uint32_t>::max())
                ? (std::numeric_limits<uint32_t>::max() - (uint32_t)(j - a))
                : mat;
        };
        std::map<uint32_t, std::vector<uint64_t>> groups;
        for (uint64_t j = a; j < b; ++j)
            groups[key_of_pos(j)].push_back(j);
        // 显式并组:grp 内成员的组键统一归并为最小键(FRZ-00 条款 3,替代/同料多供)
        for (const auto& grp : p.extra_groups) {
            std::vector<uint32_t> keys;
            for (uint32_t mem : grp)
                for (uint64_t j = a; j < b; ++j)
                    if (g.edge_nodes[j] == mem) { keys.push_back(key_of_pos(j)); break; }
            if (keys.size() < 2) continue;
            const uint32_t target = *std::min_element(keys.begin(), keys.end());
            for (uint32_t k : keys)
                if (k != target) {
                    auto& dst = groups[target];
                    auto& src = groups[k];
                    dst.insert(dst.end(), src.begin(), src.end());
                    groups.erase(k);
                }
            std::sort(groups[target].begin(), groups[target].end());
        }
        auto& st = out[e];
        st.groups = (uint32_t)groups.size();
        // k_e 优先级:逐边覆盖 > 全局覆盖 > 默认 g(e)(FRZ-05)
        st.k_e = (!p.k_e_per_edge.empty() && p.k_e_per_edge[e] != 0)
            ? p.k_e_per_edge[e]
            : p.k_e_override.value_or(st.groups);
        for (const auto& [k, poss] : groups) {
            bool ga = false;
            for (uint64_t pos : poss)
                if (ns.alive[g.edge_nodes[pos]]) { ga = true; break; }
            if (ga) ++st.alive_groups;
        }
        st.alive = (st.alive_groups >= st.k_e) ? 1 : 0;
    }
    return out;
}

// 计算 EdgeState(FRZ-05 阶段 C 布尔产物之一)
inline EdgeState compute_edge_state(const HypergraphCSR& g, const NodeState& ns, const BooleanizeParams& p) {
    auto stats = edge_group_stats(g, ns, p);
    EdgeState es;
    es.alive.resize(stats.size());
    for (size_t e = 0; e < stats.size(); ++e) es.alive[e] = stats[e].alive;
    return es;
}

} // namespace hyperalgo
