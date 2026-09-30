// hyperalgo-core: core data structures & fingerprint
// 依据:P-1 超图构造规范 SPEC v1.1(FRZ-00~07.1)、P-1.5 后端服务接口契约 v1.2 §1.1
// 许可:BSD-3-Clause(含 NWHypergraph 移植参考代码的声明保留见 connectivity.hpp)
// 编译要求:C++17,零外部依赖(header-only)
#pragma once

#include <cstdint>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <cstring>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace hyperalgo {

constexpr uint32_t ABI_VERSION = 2; // 结构体布局变更必须 +1(v1.2:+edge_business_ids/edge_thresholds/member_roles/EdgeState)

// 成员角色(FRZ-00 条款 1):0 = 站(station),1 = 料号(material)
enum MemberRole : uint8_t { ROLE_STATION = 0, ROLE_MATERIAL = 1 };

// 双 CSR 超图(FRZ-01/02 构造产物;P-1.5 §1.1)
struct HypergraphCSR {
    uint64_t n_nodes = 0;
    uint64_t n_edges = 0;
    // 方向一:按超边(在超边内迭代成员)
    std::vector<uint64_t> edge_ptr;      // 长度 n_edges+1
    std::vector<uint32_t> edge_nodes;    // 长度 edge_ptr.back(),成员的内部节点 id(每超边内升序,FRZ-01 条款 5)
    // 方向二:按节点(节点的超边关联,存关联矩阵 H 的值)
    std::vector<uint64_t> node_ptr;      // 长度 n_nodes+1
    std::vector<uint32_t> node_edges;    // 长度 node_ptr.back()
    std::vector<double>   h_values;      // 关联矩阵 H[u,e] = IO 系数,与 node_edges 等长(FRZ-04)
    // v1.2 新增(P1-3/P0-4 落点):
    std::vector<uint32_t> edge_business_ids; // 长度 n_edges,edge_pos → 超边业务 id(= S 内部 id,FRZ-01 条款 6)
    std::vector<uint32_t> edge_thresholds;   // 长度 n_edges,各超边 k_e(需求组阈值,FRZ-05;P3c/灵敏度保留原值)
    std::vector<uint8_t>  member_roles;      // 与 edge_nodes 等长,成员角色(FRZ-00,绑定层需求分组用)

    // 构造不变量校验(FRZ-02 步骤 5,I1~I6;违反抛 runtime_error)
    void validate() const {
        if (edge_ptr.size() != n_edges + 1)
            throw std::runtime_error("I2 fail: edge_ptr size");
        if (node_ptr.size() != n_nodes + 1)
            throw std::runtime_error("I2 fail: node_ptr size");
        for (uint64_t i = 0; i < n_edges; ++i)
            if (edge_ptr[i] > edge_ptr[i + 1])
                throw std::runtime_error("I2 fail: edge_ptr non-monotone at " + std::to_string(i));
        for (uint64_t i = 0; i < n_nodes; ++i)
            if (node_ptr[i] > node_ptr[i + 1])
                throw std::runtime_error("I2 fail: node_ptr non-monotone at " + std::to_string(i));
        if (edge_ptr.empty() || node_ptr.empty())
            throw std::runtime_error("I1 fail: empty CSR");
        const uint64_t nnz = edge_ptr.back();
        if (nnz != node_ptr.back())
            throw std::runtime_error("I1 fail: nnz mismatch"); // Σ|e_S| = nnz = node_ptr[n]
        for (uint64_t u = 0; u < nnz; ++u)
            if (!(h_values.size() > u && h_values[u] > 0.0))
                throw std::runtime_error("I3 fail: h_values must be > 0"); // weight 语义为 IO 系数
        if (n_edges != edge_business_ids.size())
            throw std::runtime_error("I4 fail: |edge_business_ids| != n_edges");
        if (n_edges != edge_thresholds.size())
            throw std::runtime_error("I4 fail: |edge_thresholds| != n_edges");
        if (nnz != member_roles.size())
            throw std::runtime_error("I6 fail: |member_roles| != |edge_nodes|");
        for (uint64_t e = 0; e < n_edges; ++e)
            if (edge_business_ids[e] >= n_nodes)
                throw std::runtime_error("I5 fail: edge_business_ids out of node id space");
        { // I5:元素互异
            auto ids = edge_business_ids;
            std::sort(ids.begin(), ids.end());
            if (std::adjacent_find(ids.begin(), ids.end()) != ids.end())
                throw std::runtime_error("I5 fail: duplicate edge_business_ids");
        }
        for (uint64_t u = 0; u < n_nodes; ++u) { // 成员 id 合法且超边内升序(FRZ-01 条款 5)
            for (uint64_t j = node_ptr[u]; j < node_ptr[u + 1]; ++j)
                if (node_edges[j] >= n_edges)
                    throw std::runtime_error("I2 fail: node_edges out of edge id space");
        }
        for (uint64_t e = 0; e < n_edges; ++e) {
            uint64_t a = edge_ptr[e], b = edge_ptr[e + 1];
            for (uint64_t j = a; j < b; ++j) {
                if (edge_nodes[j] >= n_nodes)
                    throw std::runtime_error("I2 fail: edge_nodes out of node id space");
                if (edge_nodes[j] == edge_business_ids[e])
                    throw std::runtime_error("FRZ-01 条款 1 fail: S ∈ e_S");
                if (j > a && edge_nodes[j - 1] >= edge_nodes[j])
                    throw std::runtime_error("I2 fail: members not ascending in edge " + std::to_string(e));
            }
        }
    }

    // 超边 e 的成员区间(引用)
    const uint32_t* members_of(uint64_t e) const { return edge_nodes.data() + edge_ptr[e]; }
    uint64_t members_size(uint64_t e) const { return edge_ptr[e + 1] - edge_ptr[e]; }
    // 节点 u 的超边区间(引用)
    const uint32_t* edges_of(uint64_t u) const { return node_edges.data() + node_ptr[u]; }
    uint64_t edges_size(uint64_t u) const { return node_ptr[u + 1] - node_ptr[u]; }
    // 节点 u 在超边 e 中的 IO 系数;不在返回 0.0
    double h(uint64_t u, uint64_t e) const {
        for (uint64_t j = node_ptr[u]; j < node_ptr[u + 1]; ++j)
            if (node_edges[j] == e)
                return h_values[j];
        return 0.0;
    }
};

// 外部业务 id ↔ 内部连续 id(FRZ-01 条款 5:按 UTF-8 字节序分配)
struct IdMap {
    std::vector<std::string> ext_ids; // index = 内部 id
    std::unordered_map<std::string, uint32_t> int_of;
    uint32_t int_of_ext(const std::string& ext) const {
        auto it = int_of.find(ext);
        if (it == int_of.end())
            throw std::out_of_range("ext id not in IdMap: " + ext);
        return it->second;
    }
    const std::string& ext_of_int(uint32_t i) const {
        if (i >= ext_ids.size())
            throw std::out_of_range("int id not in IdMap: " + std::to_string(i));
        return ext_ids[i];
    }
    void build(const std::vector<std::string>& all_ext_ids) { // 排序去重后分配(UTF-8 字节序,FRZ-01 条款 5)
        ext_ids = all_ext_ids;
        std::sort(ext_ids.begin(), ext_ids.end()); // std::string 比较 = 字节序(UTF-8)
        ext_ids.erase(std::unique(ext_ids.begin(), ext_ids.end()), ext_ids.end());
        int_of.clear();
        for (uint32_t i = 0; i < ext_ids.size(); ++i)
            int_of[ext_ids[i]] = i;
    }
};

// 节点存活(FRZ-06 分析层/判定层口径,构造时由 alive_mode 决定)
struct NodeState {
    std::vector<uint8_t> alive; // 长度 n_nodes,0/1
    void validate(uint64_t n) const {
        if (alive.size() != n)
            throw std::runtime_error("NodeState size mismatch");
    }
};

// 超边存活(v1.2 新增,P0-4 落点):由绑定层按 FRZ-05 阶段 B(需求分组 + k_e 阈值)从 NodeState 计算
struct EdgeState {
    std::vector<uint8_t> alive; // 长度 n_edges,0/1
    void validate(uint64_t n) const {
        if (alive.size() != n)
            throw std::runtime_error("EdgeState size mismatch");
    }
};

// ---- 便捷构造函数(绑定层/测试用;与 FRZ-02 步骤 1~4 对应)----
// 输入:按 (u, S) 的 FLOWS 三元组流(ext 业务 id 或内部 id)
// 返回:(CSR, IdMap)。u_roles[i] 与 flows 等长,0=站 1=料号;k_e 由外部按 FRZ-00/05 计算后写入 edge_thresholds。
struct FlowTriple {
    std::string u;      // 供应端 ext id
    std::string s;      // 装配端 ext id(超边业务 id)
    double weight;      // IO 系数 > 0
    uint8_t u_role;     // 成员角色
};

inline std::pair<HypergraphCSR, IdMap> build_csr_from_flows(const std::vector<FlowTriple>& flows) {
    HypergraphCSR g;
    IdMap idmap;
    // 1) 收集 ext id(FRZ-02 步骤 1~2)
    std::vector<std::string> all;
    all.reserve(flows.size() * 2);
    for (const auto& f : flows) {
        all.push_back(f.u);
        all.push_back(f.s);
    }
    idmap.build(all);
    g.n_nodes = idmap.ext_ids.size();
    // 2) 按 S 分组(FRZ-02 步骤 3)
    struct Member { uint32_t u; double weight; uint8_t role; };
    struct EdgeAcc {
        std::vector<Member> members; // (u int id, weight, role)
        uint32_t business_id;
    };
    std::unordered_map<uint32_t, size_t> edge_of_s;
    std::vector<EdgeAcc> edges;
    for (const auto& f : flows) {
        const uint32_t u = idmap.int_of_ext(f.u);
        const uint32_t s = idmap.int_of_ext(f.s);
        if (u == s)
            throw std::runtime_error("FRZ-01 条款 1 fail: self-edge in FLOWS (" + f.u + "→" + f.s + ")");
        auto it = edge_of_s.find(s);
        size_t idx;
        if (it == edge_of_s.end()) {
            idx = edges.size();
            edge_of_s[s] = idx;
            edges.push_back(EdgeAcc{{}, s});
        } else {
            idx = it->second;
        }
        // 并行边聚合(FRZ-01 条款 4:按 (u,S) 聚合取 SUM(weight));角色取首次出现值
        auto& m = edges[idx].members;
        bool found = false;
        for (auto& p : m)
            if (p.u == u) { p.weight += f.weight; found = true; break; }
        if (!found) m.push_back(Member{u, f.weight, f.u_role});
    }
    // 3) 建双 CSR + 映射(FRZ-02 步骤 4)
    g.n_edges = edges.size();
    g.edge_ptr.reserve(g.n_edges + 1);
    g.edge_ptr.push_back(0);
    g.edge_business_ids.reserve(g.n_edges);
    g.edge_thresholds.assign(g.n_edges, 1); // 占位,由调用方按 FRZ-05 填默认 g(e)
    g.edge_nodes.clear();
    g.member_roles.clear();
    for (auto& acc : edges) {
        std::sort(acc.members.begin(), acc.members.end(),
                  [](const Member& a, const Member& b) { return a.u < b.u; }); // 按内部 id 升序(FRZ-01 条款 5)
        for (const auto& m : acc.members) {
            g.edge_nodes.push_back(m.u);
            g.member_roles.push_back(m.role);
        }
        g.edge_ptr.push_back(g.edge_nodes.size());
        g.edge_business_ids.push_back(acc.business_id);
    }
    // 直接构造 node_edges:按超边顺序遍历,自然升序(节点方向,FRZ-02 步骤 4)
    g.node_edges.clear();
    g.node_ptr.assign(g.n_nodes + 1, 0);
    for (uint64_t e = 0; e < g.n_edges; ++e)
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
            ++g.node_ptr[g.edge_nodes[j] + 1];
    for (uint64_t u = 0; u < g.n_nodes; ++u)
        g.node_ptr[u + 1] += g.node_ptr[u];
    g.node_edges.resize(g.edge_nodes.size()); // nnz;上方 clear 后需 resize 再写入
    g.h_values.resize(g.edge_nodes.size());
    std::vector<uint64_t> fill = g.node_ptr;
    for (uint64_t e = 0; e < g.n_edges; ++e) {
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j) {
            const uint32_t u = g.edge_nodes[j];
            const uint64_t pos = fill[u]++;
            g.node_edges[pos] = (uint32_t)e;
            double w = 0.0;
            for (const auto& m : edges[e].members)
                if (m.u == u) { w = m.weight; break; }
            g.h_values[pos] = w;
        }
    }
    g.validate();
    return {g, idmap};
}

// ---- FRZ-07.1 指纹规范:canonical JSON + SHA256 ----
// double 定点化(FRZ-07.1 条款 3):round(x*1000)/1000 后 %.3f 序列化
inline std::string fixed_double(double x) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(3) << (std::round(x * 1000.0) / 1000.0);
    return os.str();
}

// 计算快照指纹(SHA256 十六进制;FRZ-07.1 条款 2/5)。
// node_alive/edge_alive 传入非空则产出 snapshot_fingerprint,否则 graph_fingerprint。
inline std::string fingerprint(const HypergraphCSR& g, const IdMap& idmap,
    const NodeState* node_state = nullptr, const EdgeState* edge_state = nullptr);

// 内联实现:canonical JSON(键按 UTF-8 字节序排序,无空白;ext_id 按 UTF-8 字节序)
// 键序(FRZ-07.1 条款 1,逐层字典序):顶层 edges < n_edges < n_nodes < node_alive;
// 超边 edge_alive < h < id < k_e < members
inline std::string canonical_json(const HypergraphCSR& g, const IdMap& idmap,
    const NodeState* node_state = nullptr, const EdgeState* edge_state = nullptr) {
    std::ostringstream os;
    os << "{";
    os << "\"edges\":[";
    // 按 edge_business_ids 升序(FRZ-07.1 条款 2)
    std::vector<uint64_t> order(g.n_edges);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](uint64_t a, uint64_t b) {
        return g.edge_business_ids[a] < g.edge_business_ids[b];
    });
    for (size_t k = 0; k < order.size(); ++k) {
        const uint64_t e = order[k];
        if (k) os << ",";
        os << "{\"edge_alive\":" << (edge_state ? (int)edge_state->alive[e] : 1);
        os << ",\"h\":[";
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j) {
            if (j != g.edge_ptr[e]) os << ",";
            os << "\"" << fixed_double(g.h(g.edge_nodes[j], e)) << "\"";
        }
        os << "],\"id\":" << g.edge_business_ids[e];
        os << ",\"k_e\":" << g.edge_thresholds[e];
        os << ",\"members\":[";
        std::vector<std::string> m;
        for (uint64_t j = g.edge_ptr[e]; j < g.edge_ptr[e + 1]; ++j)
            m.push_back(idmap.ext_of_int(g.edge_nodes[j]));
        std::sort(m.begin(), m.end()); // UTF-8 字节序(h 与 edge_nodes 同序,IdMap 已按 ext 序分配内部 id)
        for (size_t i = 0; i < m.size(); ++i) {
            if (i) os << ",";
            os << "\"" << m[i] << "\"";
        }
        os << "]}";
    }
    os << "],\"n_edges\":" << g.n_edges << ",\"n_nodes\":" << g.n_nodes;
    if (node_state) {
        os << ",\"node_alive\":{";
        bool first = true;
        for (uint64_t u = 0; u < g.n_nodes; ++u) {
            if (node_state->alive[u]) {
                if (!first) os << ",";
                first = false;
                os << "\"" << idmap.ext_of_int(u) << "\":" << 1;
            }
        }
        os << "}";
    }
    os << "}";
    return os.str();
}

// SHA-256(公有域风格实现,零依赖)
namespace detail {
struct Sha256 {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    static constexpr uint32_t K[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
        0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
        0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
        0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
        0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
        0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
        0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
        0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    static uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void block(const uint8_t* p) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) |
                   (uint32_t(p[4 * i + 2]) << 8) | uint32_t(p[4 * i + 3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::string hex(const std::string& msg) {
        size_t n = msg.size();
        uint64_t bits = (uint64_t)n * 8;
        std::string buf = msg;
        buf.push_back(char(0x80));
        while ((buf.size() % 64) != 56) buf.push_back(0);
        for (int i = 7; i >= 0; --i) buf.push_back(char((bits >> (8 * i)) & 0xff));
        for (size_t i = 0; i < buf.size(); i += 64) block((const uint8_t*)buf.data() + i);
        std::ostringstream os;
        os << std::hex << std::setfill('0');
        for (uint32_t x : h) os << std::setw(8) << x;
        return os.str();
    }
};
} // namespace detail

inline std::string fingerprint(const HypergraphCSR& g, const IdMap& idmap,
    const NodeState* node_state, const EdgeState* edge_state) {
    detail::Sha256 sha;
    return sha.hex(canonical_json(g, idmap, node_state, edge_state));
}

} // namespace hyperalgo
