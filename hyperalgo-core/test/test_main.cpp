// hyperalgo-core 独立测试(零依赖,gtest 不在独立构建路径内)
// 黄金图集 = P-1 SPEC v1.1 FRZ-12 样例 A/B/C/C1/C2;构造层验收 checklist 1/2/4/5
// 语义注:头节点(装配站 S)不是成员(FRZ-01 条款 1)→ s-邻接/齐套块只联结成员侧;
//   (k,s)-core 的度 = 成员超度,纯头节点度 0 会剥落(与 Rust get_core 口径一致)。
// 编译:cmake 或 g++ -std=c++17 -I../include test_main.cpp -o hyperalgo_tests
#include <hyperalgo/hyperalgo.hpp>

#include <iostream>
#include <numeric>
#include <set>

using namespace hyperalgo;

static int g_fails = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::cerr << "FAIL: " << #cond << " (line " << __LINE__ << ")\n";    \
            ++g_fails;                                                           \
        }                                                                        \
    } while (0)

// ---- 黄金图集样例(FRZ-12)----

// 样例 A:四级链  R→A→B→C→D(教科书基准;全退化 |e|=1;b_cycles 应为空)
static std::pair<HypergraphCSR, IdMap> sample_chain() {
    std::vector<FlowTriple> f = {
        {"R", "A", 1.0, ROLE_MATERIAL},
        {"A", "B", 1.0, ROLE_STATION},
        {"B", "C", 1.0, ROLE_STATION},
        {"C", "D", 1.0, ROLE_STATION},
    };
    return build_csr_from_flows(f);
}

// 样例 A':回路闭合(FLOWS 环形 A→B→C→D→A;b_cycles 应检出 1 条 4 超边回路)
static std::pair<HypergraphCSR, IdMap> sample_ring() {
    std::vector<FlowTriple> f = {
        {"A", "B", 1.0, ROLE_STATION},
        {"B", "C", 1.0, ROLE_STATION},
        {"C", "D", 1.0, ROLE_STATION},
        {"D", "A", 1.0, ROLE_STATION},
    };
    return build_csr_from_flows(f);
}

// 样例 B:菱形供应 + 退化边(e_P={R4})。孤立站由节点表统计(FRZ-01 条款 3),流构造不含
static std::pair<HypergraphCSR, IdMap> sample_diamond() {
    std::vector<FlowTriple> f = {
        {"R1", "M1", 1.0, ROLE_MATERIAL},
        {"R2", "M1", 1.0, ROLE_MATERIAL},
        {"R2", "M2", 1.0, ROLE_MATERIAL},
        {"R3", "M2", 1.0, ROLE_MATERIAL},
        {"M1", "Z", 1.0, ROLE_STATION},
        {"M2", "Z", 1.0, ROLE_STATION},
        {"R4", "P", 1.0, ROLE_MATERIAL}, // |e_P|=1 退化边
    };
    return build_csr_from_flows(f);
}

// 样例 C:阈值超边(异料 4 成员,显式 k_e=2;g(e)=4)
static std::pair<HypergraphCSR, IdMap> sample_threshold() {
    std::vector<FlowTriple> f = {
        {"M1", "Z", 1.0, ROLE_MATERIAL},
        {"M2", "Z", 1.0, ROLE_MATERIAL},
        {"M3", "Z", 1.0, ROLE_MATERIAL},
        {"M4", "Z", 1.0, ROLE_MATERIAL},
    };
    return build_csr_from_flows(f);
}

// 样例 C1:同料双供(x1/x2 同料号 X + 异料 y1;g(e)=2 默认 k_e=2)
static std::pair<HypergraphCSR, IdMap> sample_dup_material() {
    std::vector<FlowTriple> f = {
        {"x1", "Z", 1.0, ROLE_MATERIAL},
        {"x2", "Z", 1.0, ROLE_MATERIAL},
        {"y1", "Z", 1.0, ROLE_MATERIAL},
    };
    return build_csr_from_flows(f);
}

// 构造 material_of_member:ext 料号 id 或 UINT32_MAX(站)
static std::vector<uint32_t> material_of(const HypergraphCSR& g, const IdMap& idmap,
    const std::vector<std::string>& mat_of_ext) {
    std::unordered_map<std::string, uint32_t> mat_id;
    uint32_t next = 0;
    for (const auto& m : mat_of_ext)
        if (!mat_id.count(m)) mat_id[m] = next++;
    std::vector<uint32_t> out(g.edge_nodes.size(), std::numeric_limits<uint32_t>::max());
    for (size_t i = 0; i < g.edge_nodes.size(); ++i) {
        const auto& ext = idmap.ext_of_int(g.edge_nodes[i]);
        auto it = mat_id.find(ext);
        if (it != mat_id.end()) out[i] = it->second;
    }
    return out;
}

static void test_sample_a_chain() {
    auto [g, idmap] = sample_chain();
    CHECK(g.n_nodes == 5);
    CHECK(g.n_edges == 4);
    // 全退化边:无共现对 → s-邻接永不触发,s=1/s=2 均 5 个单点分量(退化基线)
    auto cc1 = s_connected_components(g, 1);
    std::set<uint32_t> comps1(cc1.begin(), cc1.end());
    CHECK(comps1.size() == 5);
    // 齐套连通:各超边单成员,无相互联结 → 5 个单点块
    NodeState ns;
    ns.alive.assign(g.n_nodes, 1);
    EdgeState es;
    es.alive.assign(g.n_edges, 1);
    auto kc = kitting_components(g, ns, es);
    std::set<uint32_t> blocks(kc.begin(), kc.end());
    blocks.erase(std::numeric_limits<uint32_t>::max());
    CHECK(blocks.size() == 4); // R,A,B,C;D 为纯头节点(不在成员侧)不计
    // 击垮集:每超边单成员 → 最小 = 4(验收 1:4 级链给出最小击垮集)
    auto hs = hitting_set_greedy(g, ns, es);
    CHECK(hs.size() == 4);
    // b_cycles:链无环
    CHECK(b_cycles(g).empty());
    // 指纹:确定性 + 存活态敏感
    const std::string fp1 = fingerprint(g, idmap, &ns, &es);
    const std::string fp2 = fingerprint(g, idmap, &ns, &es);
    CHECK(fp1 == fp2);
    CHECK(fp1.size() == 64);
    NodeState ns3 = ns;
    ns3.alive[0] = 0;
    CHECK(fingerprint(g, idmap, &ns3, &es) != fp1);
}

static void test_sample_ring() {
    auto [g, idmap] = sample_ring();
    CHECK(g.n_nodes == 4);
    CHECK(g.n_edges == 4);
    auto cycles = b_cycles(g);
    CHECK(cycles.size() == 1); // 恰一条回路
    CHECK(cycles[0].size() == 4);
    // 人为断一环(去掉 D→A):e_A 无成员 → 无回路(验收:断一环检出)
    std::vector<FlowTriple> f = {
        {"A", "B", 1.0, ROLE_STATION},
        {"B", "C", 1.0, ROLE_STATION},
        {"C", "D", 1.0, ROLE_STATION},
    };
    auto [g2, id2] = build_csr_from_flows(f);
    CHECK(b_cycles(g2).empty());
    (void)idmap;
    (void)id2;
}

static void test_sample_b_diamond() {
    auto [g, idmap] = sample_diamond();
    CHECK(g.n_nodes == 8);
    CHECK(g.n_edges == 4);
    uint64_t deg_edges = 0;
    for (uint64_t e = 0; e < g.n_edges; ++e)
        if (g.members_size(e) == 1) ++deg_edges;
    CHECK(deg_edges == 1); // e_P 退化
    // s-walk s=1:成员侧共现链 {R1,R2,R3}、{M1,M2};R4、P、Z 各自单点 → 5 分量
    auto cc = s_connected_components(g, 1);
    std::set<uint32_t> comps(cc.begin(), cc.end());
    CHECK(comps.size() == 5);
    // 齐套连通全活:块 {R1,R2,R3}、{M1,M2}、{R4} → 3 块(头 P/Z 不在成员侧,不计)
    NodeState ns;
    ns.alive.assign(g.n_nodes, 1);
    EdgeState es;
    es.alive.assign(g.n_edges, 1);
    auto kc = kitting_components(g, ns, es);
    std::set<uint32_t> blocks;
    for (uint32_t c : kc)
        if (c != std::numeric_limits<uint32_t>::max()) blocks.insert(c);
    CHECK(blocks.size() == 3);
    auto sz = component_sizes(kc);
    CHECK(sz.size() == 3 && sz[0] == 3 && sz[1] == 2 && sz[2] == 1);
    // 断供影响面:R2 死 → e_M1/e_M2 失效;活块 {M1,M2}、{R4} → 2 块
    NodeState ns2 = ns;
    ns2.alive[idmap.int_of_ext("R2")] = 0;
    EdgeState es2;
    es2.alive.assign(g.n_edges, 1);
    es2.alive[0] = 0; // e_M1 失效
    es2.alive[1] = 0; // e_M2 失效
    auto kc2 = kitting_components(g, ns2, es2);
    std::set<uint32_t> blocks2;
    for (uint32_t c : kc2)
        if (c != std::numeric_limits<uint32_t>::max()) blocks2.insert(c);
    CHECK(blocks2.size() == 2);
    // 击垮集:贪心 = 3(R2 + M1/M2 + R4)
    auto hs = hitting_set_greedy(g, ns, es);
    CHECK(hs.size() == 3);
    // (k,s)-core(1,1):头节点 Z/P 度 0 → 一层剥落
    auto layers = ks_core_layers(g, 1, 1);
    CHECK(layers.size() == 1);
    CHECK(layers[0].size() == 2);
}

static void test_sample_c_threshold() {
    auto [g, idmap] = sample_threshold();
    CHECK(g.n_edges == 1);
    NodeState ns;
    ns.alive.assign(g.n_nodes, 0);
    BooleanizeParams p;
    p.material_of_member = material_of(g, idmap, {"M1", "M2", "M3", "M4"});
    p.k_e_override = 2;
    // 模式 1(齐):全员活 → 4/4 ≥ 2
    for (uint64_t u = 0; u < g.n_nodes; ++u) ns.alive[u] = 1;
    auto es = compute_edge_state(g, ns, p);
    CHECK(es.alive[0] == 1);
    // 模式 2(缺 2 员):2 活组 → 2/4 ≥ 2 仍齐(k_e 允许部分满足)
    ns.alive.assign(g.n_nodes, 0);
    ns.alive[idmap.int_of_ext("M1")] = 1;
    ns.alive[idmap.int_of_ext("M2")] = 1;
    auto es2 = compute_edge_state(g, ns, p);
    CHECK(es2.alive[0] == 1);
    // 模式 3(缺 3 员):1/4 < 2 → 不齐
    ns.alive[idmap.int_of_ext("M2")] = 0;
    auto es3 = compute_edge_state(g, ns, p);
    CHECK(es3.alive[0] == 0);
}

static void test_sample_c1_dup_material() {
    auto [g, idmap] = sample_dup_material();
    CHECK(g.n_edges == 1);
    NodeState ns;
    ns.alive.assign(g.n_nodes, 0);
    BooleanizeParams p;
    p.material_of_member = material_of(g, idmap, {"x1", "x2", "y1"});
    // x1/x2 同料号(同料双供)→ 共享需求组;手写映射(成员按 int id 升序 = x1,x2,y1)
    p.material_of_member = {0, 0, 1};
    // 默认 k_e = g(e) = 2(同料 x1/x2 共享需求组);x1 死 x2 活 → 齐
    ns.alive[idmap.int_of_ext("x1")] = 0;
    ns.alive[idmap.int_of_ext("x2")] = 1;
    ns.alive[idmap.int_of_ext("y1")] = 1;
    auto es = compute_edge_state(g, ns, p);
    CHECK(es.alive[0] == 1); // 2/2 组活(同料多供韧性)
    ns.alive[idmap.int_of_ext("x2")] = 0; // 同料全死
    auto es2 = compute_edge_state(g, ns, p);
    CHECK(es2.alive[0] == 0); // 1/2 < 2 → 不齐
}

static void test_sample_c2_distinct_materials() {
    auto [g, idmap] = sample_threshold(); // 结构同上,无 k_e 覆盖
    NodeState ns;
    ns.alive.assign(g.n_nodes, 0);
    ns.alive[idmap.int_of_ext("M1")] = 1; // 只到 1 种料
    BooleanizeParams p;
    p.material_of_member = material_of(g, idmap, {"M1", "M2", "M3", "M4"});
    auto es = compute_edge_state(g, ns, p);
    CHECK(es.alive[0] == 0); // 1/4 < 4 → 不齐(v1.0 默认 k_e=1 误判齐套已修复)
}

static void test_extra_groups_substitution() {
    // 异料替代并组:a1/a2 不同料但可互换(extra_groups)→ g(e)=2 默认 k_e=2
    std::vector<FlowTriple> f = {
        {"a1", "Z", 1.0, ROLE_MATERIAL},
        {"a2", "Z", 1.0, ROLE_MATERIAL},
        {"b1", "Z", 1.0, ROLE_MATERIAL},
    };
    auto [g, idmap] = build_csr_from_flows(f);
    NodeState ns;
    ns.alive.assign(g.n_nodes, 0);
    BooleanizeParams p;
    p.material_of_member = material_of(g, idmap, {"a1", "a2", "b1"});
    p.extra_groups = {{idmap.int_of_ext("a1"), idmap.int_of_ext("a2")}};
    ns.alive[idmap.int_of_ext("a1")] = 0;
    ns.alive[idmap.int_of_ext("a2")] = 1; // 替代组内一供方活
    ns.alive[idmap.int_of_ext("b1")] = 1;
    auto es = compute_edge_state(g, ns, p);
    CHECK(es.alive[0] == 1); // 2/2 组活
    ns.alive[idmap.int_of_ext("a2")] = 0; // 替代组全死
    auto es2 = compute_edge_state(g, ns, p);
    CHECK(es2.alive[0] == 0); // 1/2 < 2 → 不齐
}

static void test_ks_core() {
    // 环 A→B→C→D→A:每节点度 1(成员侧)
    auto [g, idmap] = sample_ring();
    CHECK(ks_core_layers(g, 1, 1).empty()); // 度 ≥1、边大小 ≥1 → 稳定
    auto l2 = ks_core_layers(g, 2, 1);
    CHECK(l2.size() == 1); // 全部度 1 < 2 → 一轮剥光
    CHECK(l2[0].size() == 4);
    // 双层:菱形 k=2:除 R2(度 2)外全剥(含头节点度 0)
    auto [g2, id2] = sample_diamond();
    auto l3 = ks_core_layers(g2, 2, 1);
    CHECK(l3.size() == 1);
    CHECK(l3[0].size() == 7); // R1,R3,M1,M2,Z,P,R4
    std::set<uint32_t> remaining;
    for (uint32_t u = 0; u < g2.n_nodes; ++u) {
        bool peeled = false;
        for (const auto& layer : l3)
            for (uint32_t v : layer)
                if (v == u) peeled = true;
        if (!peeled) remaining.insert(u);
    }
    CHECK(remaining.size() == 1);
    CHECK(id2.ext_of_int(*remaining.begin()) == "R2");
    // 边大小剥离:e1={a,b,c}, e2={a,b}, e3={b,c};k=2,s=3
    std::vector<FlowTriple> f2 = {
        {"a", "H1", 1.0, ROLE_STATION}, {"b", "H1", 1.0, ROLE_STATION}, {"c", "H1", 1.0, ROLE_STATION},
        {"a", "H2", 1.0, ROLE_STATION}, {"b", "H2", 1.0, ROLE_STATION},
        {"b", "H3", 1.0, ROLE_STATION}, {"c", "H3", 1.0, ROLE_STATION},
    };
    auto [g3, id3] = build_csr_from_flows(f2);
    auto l4 = ks_core_layers(g3, 2, 3);
    CHECK(l4.size() == 2); // 轮1:头节点 3 个;轮2:a,b,c
    CHECK(l4[0].size() == 3 && l4[1].size() == 3);
    (void)id3;
}

static void test_spectral() {
    // 手工 CSR:路径 P3(3 节点 2 超边),无孤立头污染谱
    HypergraphCSR g;
    g.n_nodes = 3;
    g.n_edges = 2;
    g.edge_ptr = {0, 2, 4};
    g.edge_nodes = {0, 1, 1, 2};
    g.edge_business_ids = {2, 0}; // S 不在自身成员内(FRZ-01 条款 1)
    g.edge_thresholds = {1, 1};
    g.member_roles = {ROLE_STATION, ROLE_STATION, ROLE_STATION, ROLE_STATION};
    g.node_ptr = {0, 1, 3, 4};
    g.node_edges = {0, 0, 1, 1};
    g.h_values = {1.0, 1.0, 1.0, 1.0};
    g.validate();
    auto fiedler = fiedler_vector(g);
    CHECK(fiedler.size() == 3);
    CHECK(std::fabs(fiedler[0] + fiedler[2]) < 1e-6); // P3 反对称
    auto scores = fiedler_candidate_scores(g, {{0, 2}, {0, 1}});
    CHECK(scores[0] > scores[1]); // 跨端 (a,c) 优于相邻 (a,b)
    CHECK(scores[0] >= 0.0);
}

static void test_pagerank() {
    auto [g, idmap] = sample_ring(); // 4 节点每节点度 1 → 平稳分布均匀
    auto pi = pagerank_two_step(g, 0.15, 200, 1e-9);
    double sum = 0.0;
    for (double x : pi) {
        CHECK(x >= 0.0);
        sum += x;
    }
    CHECK(std::fabs(sum - 1.0) < 1e-6);
    // 双成员超边:e1={a,b},e2={b,c}(手工 CSR,无头污染)
    HypergraphCSR g2;
    g2.n_nodes = 3;
    g2.n_edges = 2;
    g2.edge_ptr = {0, 2, 4};
    g2.edge_nodes = {0, 1, 1, 2};
    g2.edge_business_ids = {2, 0};
    g2.edge_thresholds = {1, 1};
    g2.member_roles = {ROLE_MATERIAL, ROLE_MATERIAL, ROLE_MATERIAL, ROLE_MATERIAL};
    g2.node_ptr = {0, 1, 3, 4};
    g2.node_edges = {0, 0, 1, 1};
    g2.h_values = {1.0, 1.0, 1.0, 1.0};
    g2.validate();
    auto pi2 = pagerank_two_step(g2, 0.15, 200, 1e-9);
    double sum2 = 0.0;
    for (double x : pi2) sum2 += x;
    CHECK(std::fabs(sum2 - 1.0) < 1e-6);
    CHECK(pi2[0] > 0.0);
    (void)idmap;
}

static void test_validation() {
    // FRZ-01 条款 1:自环必须拒绝
    std::vector<FlowTriple> f = {{"S", "S", 1.0, ROLE_STATION}};
    bool threw = false;
    try {
        build_csr_from_flows(f);
    } catch (const std::exception&) {
        threw = true;
    }
    CHECK(threw);
}

int main() {
    test_sample_a_chain();
    test_sample_ring();
    test_sample_b_diamond();
    test_sample_c_threshold();
    test_sample_c1_dup_material();
    test_sample_c2_distinct_materials();
    test_extra_groups_substitution();
    test_ks_core();
    test_spectral();
    test_pagerank();
    test_validation();
    if (g_fails == 0) {
        std::cout << "ALL TESTS PASSED\n";
        return 0;
    }
    std::cerr << g_fails << " CHECK(s) FAILED\n";
    return 1;
}
