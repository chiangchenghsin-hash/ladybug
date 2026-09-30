#pragma once

#include "function/function.h"

namespace lbug {
namespace algo_extension {

struct SCCFunction {
    static constexpr const char* name = "STRONGLY_CONNECTED_COMPONENTS";

    static function::function_set getFunctionSet();
};

struct SCCAliasFunction {
    using alias = SCCFunction;

    static constexpr const char* name = "SCC";
};

struct SCCKosarajuFunction {
    static constexpr const char* name = "STRONGLY_CONNECTED_COMPONENTS_KOSARAJU";

    static function::function_set getFunctionSet();
};

struct SCCKosarajuAliasFunction {
    using alias = SCCKosarajuFunction;

    static constexpr const char* name = "SCC_KO";
};

struct WeaklyConnectedComponentsFunction {
    static constexpr const char* name = "WEAKLY_CONNECTED_COMPONENTS";

    static function::function_set getFunctionSet();
};

struct WeaklyConnectedComponentsAliasFunction {
    using alias = WeaklyConnectedComponentsFunction;

    static constexpr const char* name = "WCC";
};

// Connected-component labeling: one row per node, `component_id` = weakly connected
// component ID (INT64). Run-to-convergence variant of WCC (no maxiterations cap).
struct ComponentIDsFunction {
    static constexpr const char* name = "COMPONENT_IDS";

    static function::function_set getFunctionSet();
};

struct ComponentIDsAliasFunction {
    using alias = ComponentIDsFunction;

    static constexpr const char* name = "component_ids";
};

struct PageRankFunction {
    static constexpr const char* name = "PAGE_RANK";

    static function::function_set getFunctionSet();
};

struct PageRankAliasFunction {
    using alias = PageRankFunction;

    static constexpr const char* name = "PR";
};

// icebug (NetworKit)-backed PageRank. Coexists with PAGE_RANK for one release cycle.
struct GDSPageRankFunction {
    static constexpr const char* name = "GDS_PAGE_RANK";

    static function::function_set getFunctionSet();
};

// VF2++ subgraph isomorphism: pattern DAG vs arbitrary directed target graph.
struct SubgraphIsomorphismFunction {
    static constexpr const char* name = "SUBGRAPH_ISOMORPHISM";

    static function::function_set getFunctionSet();
};

// Topological sort + longest-path depth for DAG verification (rejects cyclic graphs).
struct TopologicalSortFunction {
    static constexpr const char* name = "TOPOLOGICAL_SORT";

    static function::function_set getFunctionSet();
};

// Reflexion Model 三分类: convergence / divergence / absence between two projected graphs.
struct GraphDiffFunction {
    static constexpr const char* name = "GRAPH_DIFF";

    static function::function_set getFunctionSet();
};

// 图编辑距离（Sanfeliu & Fu 1983，DFS-BB 精确计算）：SHOULD-BE vs AS-IS 结构差异量化。
struct GraphEditDistanceFunction {
    static constexpr const char* name = "GRAPH_EDIT_DISTANCE";

    static function::function_set getFunctionSet();
};

// 架构漂移检测特征向量（一图一签名，供 detect_drift_points / 增量快照 diff 消费）。
struct GraphSignatureFunction {
    static constexpr const char* name = "GRAPH_SIGNATURE";

    static function::function_set getFunctionSet();
};


struct KCoreDecompositionFunction {
    static constexpr const char* name = "K_CORE_DECOMPOSITION";

    static function::function_set getFunctionSet();
};

struct KCoreDecompositionAliasFunction {
    using alias = KCoreDecompositionFunction;

    static constexpr const char* name = "KCORE";
};

struct LouvainFunction {
    static constexpr const char* name = "LOUVAIN";

    static function::function_set getFunctionSet();
};

struct SpanningForest {
    static constexpr const char* name = "SPANNING_FOREST";

    static function::function_set getFunctionSet();
};

struct SpanningForestAliasFunction {
    using alias = SpanningForest;

    static constexpr const char* name = "SF";
};

// Role-depth algorithms (角色深度扩展). All take a single-node-table projected graph.
// Lookup is case-insensitive, so `CALL betweenness(...)` resolves to BETWEENNESS.
struct BetweennessFunction {
    static constexpr const char* name = "BETWEENNESS";

    static function::function_set getFunctionSet();
};

struct LocalClusteringCoefficientFunction {
    static constexpr const char* name = "LOCAL_CLUSTERING_COEFFICIENT";

    static function::function_set getFunctionSet();
};

struct KatzCentralityFunction {
    static constexpr const char* name = "KATZ_CENTRALITY";

    static function::function_set getFunctionSet();
};

struct AssortativityFunction {
    static constexpr const char* name = "ASSORTATIVITY";

    static function::function_set getFunctionSet();
};

// 单点故障 / 关键路径分析（角色深度扩展续）。均接受单节点表投影图，纯 C++ CSR 实现，
// 不依赖 NetworKit/Arrow。语义对标 NetworkX：
//   BRIDGES             — 桥（割边），无向，输出 (src, dst) 边
//   ARTICULATION_POINTS — 割点（关节点），无向，输出 (node)
//   SHORTEST_PATH       — 单源最短路径（BFS，无权，有向），输出 (node, distance)
struct BridgesFunction {
    static constexpr const char* name = "BRIDGES";

    static function::function_set getFunctionSet();
};

struct ArticulationPointsFunction {
    static constexpr const char* name = "ARTICULATION_POINTS";

    static function::function_set getFunctionSet();
};

struct ShortestPathFunction {
    static constexpr const char* name = "SHORTEST_PATH";

    static function::function_set getFunctionSet();
};

// =============================================================================
// NASH × GNN P1 batch (ALGORITHM-SPEC-merged.md §5.1). 纯 C++ CSR 自实现,
// 对标 NetworkX/NetworKit 语义, 不依赖 NetworKit/Arrow。函数名大小写不敏感,
// 默认跑单节点表投影。除 DIJKSTRA/MAX_FLOW 外均为无向语义 (buildUndirectedAdjacency)。
// =============================================================================

// 结构分解与连通性
struct BiconnectedComponentsFunction {
    static constexpr const char* name = "BICONNECTED_COMPONENTS";
    static function::function_set getFunctionSet();
};

struct KTrussFunction {
    static constexpr const char* name = "K_TRUSS";
    static function::function_set getFunctionSet();
};

// 中心性（族化）
struct EigenvectorCentralityFunction {
    static constexpr const char* name = "EIGENVECTOR_CENTRALITY";
    static function::function_set getFunctionSet();
};

struct ClosenessCentralityFunction {
    static constexpr const char* name = "CLOSENESS_CENTRALITY";
    static function::function_set getFunctionSet();
};

struct ApproxBetweennessFunction {
    static constexpr const char* name = "APPROX_BETWEENNESS";
    static function::function_set getFunctionSet();
};

struct SpanningEdgeCentralityFunction {
    static constexpr const char* name = "SPANNING_EDGE_CENTRALITY";
    static function::function_set getFunctionSet();
};

struct GroupBetweennessFunction {
    static constexpr const char* name = "GROUP_BETWEENNESS";
    static function::function_set getFunctionSet();
};

struct GroupClosenessFunction {
    static constexpr const char* name = "GROUP_CLOSENESS";
    static function::function_set getFunctionSet();
};

// 社区检测
struct LabelPropagationFunction {
    static constexpr const char* name = "LABEL_PROPAGATION";
    static function::function_set getFunctionSet();
};

struct CutClusteringFunction {
    static constexpr const char* name = "CUT_CLUSTERING";
    static function::function_set getFunctionSet();
};

// 距离与路径
struct DijkstraFunction {
    static constexpr const char* name = "DIJKSTRA";
    static function::function_set getFunctionSet();
};

struct AllPairsShortestPathFunction {
    static constexpr const char* name = "ALL_PAIRS_SHORTEST_PATH";
    static function::function_set getFunctionSet();
};

struct DiameterFunction {
    static constexpr const char* name = "DIAMETER";
    static function::function_set getFunctionSet();
};

struct EffectiveDiameterFunction {
    static constexpr const char* name = "EFFECTIVE_DIAMETER";
    static function::function_set getFunctionSet();
};

// 流与结构度量
struct MaxFlowFunction {
    static constexpr const char* name = "MAX_FLOW";
    static function::function_set getFunctionSet();
};

struct MinCutValueFunction {
    static constexpr const char* name = "MIN_CUT_VALUE";
    static function::function_set getFunctionSet();
};

struct TriangleCountFunction {
    static constexpr const char* name = "TRIANGLE_COUNT";
    static function::function_set getFunctionSet();
};

struct GlobalClusteringFunction {
    static constexpr const char* name = "GLOBAL_CLUSTERING";
    static function::function_set getFunctionSet();
};

struct LinkPredictionFunction {
    static constexpr const char* name = "LINK_PREDICTION";
    static function::function_set getFunctionSet();
};

// =============================================================================
// NASH × GNN P2 batch (ALGORITHM-SPEC-merged.md §5.2). 纯 C++ CSR 自实现,
// 对标 NetworkX/NetworKit 语义, 不依赖 NetworKit/Arrow (官方 icebug 路线本机
// 缺 Arrow 依赖, 不可用)。批次一: BFS/幂迭代/稀疏化; 批次二: 动态/粗化/谱/匹配。
// =============================================================================

// P2-13 距离与度量: ECCENTRICITY (每节点) / RADIUS (单行标量), 无向无权。
struct EccentricityFunction {
    static constexpr const char* name = "ECCENTRICITY";
    static function::function_set getFunctionSet();
};

struct RadiusFunction {
    static constexpr const char* name = "RADIUS";
    static function::function_set getFunctionSet();
};

// P2-14 HITS (有向枢纽/权威, 幂迭代)。
struct HitsFunction {
    static constexpr const char* name = "HITS";
    static function::function_set getFunctionSet();
};

// P2-15 SIR_SIMULATOR (随机传染, 固定种子确定性)。
struct SirSimulatorFunction {
    static constexpr const char* name = "SIR_SIMULATOR";
    static function::function_set getFunctionSet();
};

// P2-C 稀疏化 (P2-06..09)。
struct LocalDegreeSparsificationFunction {
    static constexpr const char* name = "LOCAL_DEGREE_SPARSIFICATION";
    static function::function_set getFunctionSet();
};

struct GlobalThresholdFilterFunction {
    static constexpr const char* name = "GLOBAL_THRESHOLD_FILTER";
    static function::function_set getFunctionSet();
};

struct RandomEdgeSparsificationFunction {
    static constexpr const char* name = "RANDOM_EDGE_SPARSIFICATION";
    static function::function_set getFunctionSet();
};

struct TriangleFilterFunction {
    static constexpr const char* name = "TRIANGLE_FILTER";
    static function::function_set getFunctionSet();
};

// P2 批次二: 动态图 / 粗化 / 分区求交 / 谱 / 匹配 / 通勤 (P2-01..05/10/11/12)。
struct DynConnectedComponentsFunction {
    static constexpr const char* name = "DYN_CONNECTED_COMPONENTS";
    static function::function_set getFunctionSet();
};

struct DynKatzCentralityFunction {
    static constexpr const char* name = "DYN_KATZ_CENTRALITY";
    static function::function_set getFunctionSet();
};

struct Dyn2HopLandmarkFunction {
    static constexpr const char* name = "DYN_2HOP_LANDMARK";
    static function::function_set getFunctionSet();
};

struct GraphCoarseningFunction {
    static constexpr const char* name = "GRAPH_COARSENING";
    static function::function_set getFunctionSet();
};

struct PartitionIntersectionFunction {
    static constexpr const char* name = "PARTITION_INTERSECTION";
    static function::function_set getFunctionSet();
};

struct SpectralPartitioningFunction {
    static constexpr const char* name = "SPECTRAL_PARTITIONING";
    static function::function_set getFunctionSet();
};

struct MaxWeightMatchingFunction {
    static constexpr const char* name = "MAX_WEIGHT_MATCHING";
    static function::function_set getFunctionSet();
};

struct CommuteTimeDistanceFunction {
    static constexpr const char* name = "COMMUTE_TIME_DISTANCE";
    static function::function_set getFunctionSet();
};

// =============================================================================
// NASH × GNN P3 batch (ALGORITHM-SPEC-merged.md §5.3). 图生成器: 从零产边,
// 纯 C++ 自实现, 对标 NetworkX 3.6.1 生成器语义 (barabasi_albert_graph /
// watts_strogatz_graph / stochastic_block_model) 与 NetworKit Curveball。
// 前 3 个是普通表函数 (不接收投影图, 全命名参数); graph_randomization 接收
// 投影图做保度 Null Model 随机化。确定性: std::mt19937(seed)。
// =============================================================================

// P3-01 Barabási–Albert 优先连接生成器 (n 节点, 每新节点连 k 个去重目标)。
struct BarabasiAlbertFunction {
    static constexpr const char* name = "GENERATE_BARABASI_ALBERT";
    static function::function_set getFunctionSet();
};

// P3-02 Watts–Strogatz 小世界生成器 (n 环 + k 最近邻 + 概率 p 重连)。
struct WattsStrogatzFunction {
    static constexpr const char* name = "GENERATE_WATTS_STROGATZ";
    static function::function_set getFunctionSet();
};

// P3-03 随机分块模型生成器 (blocks 块大小 + P 块间边概率矩阵, 无向无自环)。
struct StochasticBlockmodelFunction {
    static constexpr const char* name = "GENERATE_STOCHASTIC_BLOCKMODEL";
    static function::function_set getFunctionSet();
};

// P3-04 保度 Null Model 随机化 (curveball 默认 / double_edge_swap 备选)。
struct GraphRandomizationFunction {
    static constexpr const char* name = "GRAPH_RANDOMIZATION";
    static function::function_set getFunctionSet();
};

} // namespace algo_extension
} // namespace lbug
