// hyperalgo-core 命令行工具:供对拍 harness(Python oracle)调用
// 输入 TSV(逐行):
//   NODE <ext_id> <alive:0/1>        // 节点存活(alive 语义;缺省 0,与 FRZ-06 显式声明口径一致)
//   FLOW <u_ext> <S_ext> <weight>    // u→S 供应流(超边 S 的成员 u;并行边 SUM 聚合)
//   GROUP <member_ext> <material_id> // 料号成员分组(FRZ-00 条款 3;缺省站成员各自独立需求组)
//   KE <S_ext> <k_e>                 // 超边阈值覆盖(FRZ-05;缺省 k_e = g(e))
// 用法:hyperalgo_cli <cmd> <input.tsv> [args]
//   scc <s>        → node<TAB>component_id(全部节点)
//   kitting        → node<TAB>block_id(仅覆盖节点;alive 按 NODE 行)
//   hitset         → node(贪心选择序)
//   kscore <k> <s> → node<TAB>layer
//   bcycles        → cycle_id<TAB>edge_business_ext
//   pr <alpha> <maxiter> <tol> → node<TAB>score
//   fiedler        → node<TAB>score
//   booleanize     → S_ext<TAB>groups<TAB>k_e<TAB>alive_groups<TAB>alive(FRZ-05 阶段 B/C)
//   fp             → graph_fingerprint(hex)
// 对拍语义与 P-1 SPEC v1.1(FRZ-00/05/07.1)一致:成员角色缺省站(k_e=|e|);material 分组经 GROUP。
#include <hyperalgo/hyperalgo.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

using namespace hyperalgo;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "usage: hyperalgo_cli <cmd> <input.tsv> [args...]\n";
        return 2;
    }
    const std::string cmd = argv[1];
    std::ifstream in(argv[2]);
    if (!in) {
        std::cerr << "cannot open " << argv[2] << "\n";
        return 2;
    }
    std::vector<FlowTriple> flows;
    std::unordered_map<std::string, uint8_t> alive_map;
    std::vector<std::pair<std::string, std::string>> groups; // (member_ext, material_id)
    std::unordered_map<std::string, uint32_t> ke_override;   // S_ext → k_e
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string tag;
        ss >> tag;
        if (tag == "NODE") {
            std::string id;
            int alive = 0;
            ss >> id >> alive;
            alive_map[id] = (uint8_t)(alive ? 1 : 0);
        } else if (tag == "FLOW") {
            std::string u, s;
            double w = 1.0;
            ss >> u >> s >> w;
            flows.push_back({u, s, w, ROLE_STATION});
        } else if (tag == "GROUP") {
            std::string mem, mat;
            ss >> mem >> mat;
            groups.emplace_back(mem, mat);
        } else if (tag == "KE") {
            std::string s;
            uint32_t k = 0;
            ss >> s >> k;
            ke_override[s] = k;
        }
    }
    auto [g, idmap] = build_csr_from_flows(flows);
    NodeState ns;
    ns.alive.assign(g.n_nodes, 0);
    for (uint64_t u = 0; u < g.n_nodes; ++u) {
        auto it = alive_map.find(idmap.ext_of_int(u));
        ns.alive[u] = (it == alive_map.end()) ? 0 : it->second;
    }
    // FRZ-05 布尔化参数(阶段 B):料号成员按 material_id 并组,站成员各自独立
    BooleanizeParams bp;
    bp.material_of_member.assign(g.edge_nodes.size(),
        std::numeric_limits<uint32_t>::max());
    if (!groups.empty()) {
        std::unordered_map<std::string, uint32_t> mat_key;
        for (const auto& [mem, mat] : groups) {
            auto kit = mat_key.find(mat);
            uint32_t key;
            if (kit == mat_key.end()) {
                key = (uint32_t)mat_key.size();
                mat_key.emplace(mat, key);
            } else {
                key = kit->second;
            }
            for (uint64_t j = 0; j < g.edge_nodes.size(); ++j)
                if (idmap.ext_of_int(g.edge_nodes[j]) == mem)
                    bp.material_of_member[j] = key;
        }
    }
    if (!ke_override.empty()) {
        bp.k_e_per_edge.assign(g.n_edges, 0); // 0 = 未指定(回退默认 g(e))
        for (uint64_t e = 0; e < g.n_edges; ++e) {
            const std::string s = idmap.ext_of_int(g.edge_business_ids[e]);
            auto it = ke_override.find(s);
            if (it != ke_override.end()) bp.k_e_per_edge[e] = it->second;
        }
    }
    const auto stats = edge_group_stats(g, ns, bp);
    // 生效 k_e 写回 CSR(FRZ-07.1 指纹与算法层消费;FRZ-05 阶段 C)
    for (uint64_t e = 0; e < g.n_edges; ++e)
        g.edge_thresholds[e] = stats[e].k_e;
    auto es = compute_edge_state(g, ns, bp);

    auto ext = [&](uint32_t u) { return idmap.ext_of_int(u); };
    if (cmd == "scc") {
        const uint32_t s = (uint32_t)std::stoul(argv[3]);
        auto cc = s_connected_components(g, s);
        for (uint64_t u = 0; u < g.n_nodes; ++u)
            std::cout << ext((uint32_t)u) << "\t" << cc[u] << "\n";
    } else if (cmd == "kitting") {
        auto blocks = kitting_components(g, ns, es);
        for (uint64_t u = 0; u < g.n_nodes; ++u)
            if (blocks[u] != std::numeric_limits<uint32_t>::max())
                std::cout << ext((uint32_t)u) << "\t" << blocks[u] << "\n";
    } else if (cmd == "hitset") {
        auto hit = hitting_set_greedy(g, ns, es);
        for (uint32_t u : hit) std::cout << ext(u) << "\n";
    } else if (cmd == "kscore") {
        const uint32_t k = (uint32_t)std::stoul(argv[3]);
        const uint32_t s = (uint32_t)std::stoul(argv[4]);
        auto layers = ks_core_layers(g, k, s);
        for (uint64_t layer = 0; layer < layers.size(); ++layer)
            for (uint32_t u : layers[layer])
                std::cout << ext(u) << "\t" << layer << "\n";
    } else if (cmd == "bcycles") {
        auto cycles = b_cycles(g);
        for (uint64_t c = 0; c < cycles.size(); ++c)
            for (uint32_t e : cycles[c])
                std::cout << c << "\t" << ext(g.edge_business_ids[e]) << "\n";
    } else if (cmd == "booleanize") {
        for (uint64_t e = 0; e < g.n_edges; ++e)
            std::cout << ext(g.edge_business_ids[e]) << "\t" << stats[e].groups << "\t"
                      << stats[e].k_e << "\t" << stats[e].alive_groups << "\t"
                      << (int)stats[e].alive << "\n";
    } else if (cmd == "pr") {
        const double alpha = std::stod(argv[3]);
        const uint32_t maxIter = (uint32_t)std::stoul(argv[4]);
        const double tol = std::stod(argv[5]);
        auto pi = pagerank_two_step(g, alpha, maxIter, tol);
        for (uint64_t u = 0; u < g.n_nodes; ++u)
            std::cout << ext((uint32_t)u) << "\t" << pi[u] << "\n";
    } else if (cmd == "fiedler") {
        auto f = fiedler_vector(g);
        for (uint64_t u = 0; u < g.n_nodes; ++u)
            std::cout << ext((uint32_t)u) << "\t" << f[u] << "\n";
    } else if (cmd == "fp") {
        std::cout << fingerprint(g, idmap) << "\n";
    } else {
        std::cerr << "unknown cmd " << cmd << "\n";
        return 2;
    }
    return 0;
}
