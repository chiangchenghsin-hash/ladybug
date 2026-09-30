// hyperalgo 扩展:图注册表 + 从投影图构建双 CSR(FRZ-02 绑定层职责)
// 依据:P-1 SPEC v1.1 FRZ-01/02/04/07.1、P-1.5 v1.2 §1.1
// v1 冻结(文档见 P-1.5 §2「0.20.2 适配要点」与扩展注释):
//   - ext id = "<tableID>:<offset>" 复合键(业务 id 由 REST 绑定层映射)
//   - 成员角色 v1 全为站(material_groups/alive 参数为 v1.1 扩展)
//   - 权重属性名冻结 "weight",缺失/NULL 回退 1.0(与 libalgo weighted_graph 同口径)
//   - k_e 默认 = |e_S|(全成员各自独立需求组,FRZ-05 默认;异料单供语义)
#pragma once

#include <unordered_map>
#include <unordered_set>

#include "catalog/catalog.h"
#include "common/exception/binder.h"
#include "common/types/types.h"
#include "function/gds/gds.h"
#include "graph/graph.h"
#include "main/client_context.h"
#include "transaction/transaction.h"

#include <hyperalgo/hyperalgo.hpp>

namespace lbug {
namespace hyperalgo_extension {

static constexpr const char* HYPER_WEIGHT_PROPERTY = "weight";

// 缓存的超图句柄(FRZ-02 构造产物 + 统计)
struct HyperGraphHandle {
    hyperalgo::HypergraphCSR csr;
    hyperalgo::IdMap idmap;
    std::vector<hyperalgo::FlowTriple> flows; // 供调试/重建(可选)
    uint64_t isolated_stations = 0;
    uint64_t degenerate_edges = 0;
};

// 命名句柄注册表(契约 §3.1:hyper_project 物化并缓存;多算法零重建)
class HyperGraphRegistry {
public:
    static HyperGraphRegistry& instance() {
        static HyperGraphRegistry r;
        return r;
    }
    bool exists(const std::string& name) const { return handles.count(name) > 0; }
    const HyperGraphHandle& get(const std::string& name) const {
        auto it = handles.find(name);
        if (it == handles.end())
            throw common::BinderException(
                "hyperalgo: projected hypergraph '" + name +
                "' not found; run CALL hyper_project('" + name +
                "', node_tables, rel_tables) first");
        return it->second;
    }
    void put(const std::string& name, HyperGraphHandle handle) { handles[name] = std::move(handle); }
    void drop(const std::string& name) { handles.erase(name); }

private:
    std::unordered_map<std::string, HyperGraphHandle> handles;
};

namespace {

// 读取关系权重(与 extension/algo weighted_graph.cpp 同口径:double/float/int 家族,缺失回退 1.0)
double readWeightAsDouble(common::ValueVector& vector, common::idx_t pos) {
    switch (vector.dataType.getLogicalTypeID()) {
    case common::LogicalTypeID::DOUBLE:
        return vector.getValue<double>(pos);
    case common::LogicalTypeID::FLOAT:
        return static_cast<double>(vector.getValue<float>(pos));
    case common::LogicalTypeID::INT64:
        return static_cast<double>(vector.getValue<int64_t>(pos));
    case common::LogicalTypeID::INT32:
        return static_cast<double>(vector.getValue<int32_t>(pos));
    case common::LogicalTypeID::INT16:
        return static_cast<double>(vector.getValue<int16_t>(pos));
    case common::LogicalTypeID::UINT64:
        return static_cast<double>(vector.getValue<uint64_t>(pos));
    case common::LogicalTypeID::UINT32:
        return static_cast<double>(vector.getValue<uint32_t>(pos));
    default:
        return 1.0;
    }
}

std::string extIdOfNode(const common::nodeID_t& nid) {
    return std::to_string(nid.tableID) + ":" + std::to_string(nid.offset);
}

// 从投影图构建 CSR(FRZ-02 步骤 1~4 + 统计)。调用方须在活跃事务内。
HyperGraphHandle buildHandleFromGraph(graph::Graph* graph, transaction::Transaction* tx) {
    HyperGraphHandle handle;
    std::vector<hyperalgo::FlowTriple> flows;
    std::unordered_set<std::string> dstNodes; // 出现过入边的节点(装配站)
    const auto maxOffsetMap = graph->getMaxOffsetMap(tx);
    const auto nodeTables = graph->getNodeTableIDs();
    // 边扫描:每源节点表 × 每关系表(FLOWS 方向 = src→dst,即 u→S)
    for (const auto srcTableID : nodeTables) {
        const auto relInfos = graph->getRelInfos(srcTableID);
        for (const auto& info : relInfos) {
            const bool hasWeight = info.relGroupEntry->containsProperty(HYPER_WEIGHT_PROPERTY);
            const auto scanState = graph->prepareRelScan(*info.relGroupEntry, info.relTableID,
                info.dstTableID, hasWeight ? std::vector<std::string>{HYPER_WEIGHT_PROPERTY}
                                           : std::vector<std::string>{},
                false /*randomLookup*/);
            const auto srcMax = graph->getMaxOffset(tx, srcTableID);
            for (uint64_t offset = 0; offset < srcMax; ++offset) {
                const common::nodeID_t src{static_cast<common::offset_t>(offset), srcTableID};
                for (auto chunk : graph->scanFwd(src, *scanState)) {
                    chunk.forEach([&](auto neighbors, auto props, auto i) {
                        const auto& dst = neighbors[i];
                        double w = 1.0;
                        if (hasWeight && props[0] && !props[0]->isNull(i)) {
                            w = readWeightAsDouble(*props[0], i);
                        }
                        flows.push_back({extIdOfNode(src), extIdOfNode(dst), w,
                            hyperalgo::ROLE_STATION});
                        dstNodes.insert(extIdOfNode(dst));
                    });
                }
            }
        }
    }
    // 孤立站 = 节点表中从未作为装配端(入边)出现的节点(FRZ-01 条款 3)
    uint64_t totalNodes = 0;
    for (const auto tableID : nodeTables)
        totalNodes += graph->getMaxOffset(tx, tableID);
    handle.isolated_stations = totalNodes > dstNodes.size() ? totalNodes - dstNodes.size() : 0;
    // 构造双 CSR + IdMap
    auto [csr, idmap] = hyperalgo::build_csr_from_flows(flows);
    // k_e 默认 = g(e) = |e_S|(v1 全站成员,FRZ-05 默认;样例 C2 语义)
    for (uint64_t e = 0; e < csr.n_edges; ++e)
        csr.edge_thresholds[e] = (uint32_t)csr.members_size(e);
    // 退化边计数
    handle.degenerate_edges = 0;
    for (uint64_t e = 0; e < csr.n_edges; ++e)
        if (csr.members_size(e) == 1) ++handle.degenerate_edges;
    handle.csr = std::move(csr);
    handle.idmap = std::move(idmap);
    handle.flows = std::move(flows);
    return handle;
}

// 全存活状态(分析层 v1 默认;alive/round 参数为 v1.1 扩展)
std::pair<hyperalgo::NodeState, hyperalgo::EdgeState> allAliveState(const HyperGraphHandle& handle) {
    hyperalgo::NodeState ns;
    ns.alive.assign(handle.csr.n_nodes, 1);
    hyperalgo::EdgeState es;
    es.alive.assign(handle.csr.n_edges, 1);
    return {ns, es};
}

} // namespace
} // namespace hyperalgo_extension
} // namespace lbug
