#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "common/directed_csr.h"

namespace lbug {
namespace algo_extension {

// 架构漂移检测特征向量：一图一签名（多行 feature/value）。
// 消费路径：每快照一行签名 → 展平为向量 → detect_drift_points（libtimeseries）
// 找漂移点；或两快照签名向量差的范数作为连续漂移度量（增量快照 diff 的量化基础）。
struct GraphSignature {
    std::vector<std::pair<std::string, double>> features;
};

// 计算图签名（纯核心，无引擎依赖，可单测）。
// 特征集（全部 DOUBLE 值）：
//   num_nodes, num_edges, density（有向：edges/(n*(n-1))，n<=1 为 0）,
//   in_degree_mean, out_degree_mean, in_degree_max, out_degree_max,
//   bidirectional_edges（双向边对数）, self_loops, sources（入度 0）, sinks（出度 0）,
//   scc_count（Tarjan 强连通分量）, wcc_count（弱连通分量）,
//   is_dag（1/0）, max_depth（DAG 最长路径深度，非 DAG 为 0）
GraphSignature computeSignature(const DirectedCSR& csr);

} // namespace algo_extension
} // namespace lbug
