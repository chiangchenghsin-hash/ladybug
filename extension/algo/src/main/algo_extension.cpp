#include "main/algo_extension.h"

#include "function/algo_function.h"
#include "function/leiden.h"
#include "main/client_context.h"

namespace lbug {
namespace algo_extension {

using namespace extension;

void AlgoExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    ExtensionUtils::addTableFunc<SCCFunction>(db);
    ExtensionUtils::addTableFuncAlias<SCCAliasFunction>(db);
    ExtensionUtils::addTableFunc<SCCKosarajuFunction>(db);
    ExtensionUtils::addTableFuncAlias<SCCKosarajuAliasFunction>(db);
    ExtensionUtils::addTableFunc<WeaklyConnectedComponentsFunction>(db);
    ExtensionUtils::addTableFuncAlias<WeaklyConnectedComponentsAliasFunction>(db);
    ExtensionUtils::addTableFunc<ComponentIDsFunction>(db);
    ExtensionUtils::addTableFuncAlias<ComponentIDsAliasFunction>(db);
    ExtensionUtils::addTableFunc<PageRankFunction>(db);
#if defined(ENABLE_ICEBUG)
    ExtensionUtils::addTableFunc<GDSPageRankFunction>(db);
#endif
    ExtensionUtils::addTableFuncAlias<PageRankAliasFunction>(db);
    ExtensionUtils::addTableFunc<KCoreDecompositionFunction>(db);
    ExtensionUtils::addTableFuncAlias<KCoreDecompositionAliasFunction>(db);
    ExtensionUtils::addTableFunc<LouvainFunction>(db);
    ExtensionUtils::addTableFunc<LeidenFunction>(db);
    ExtensionUtils::addTableFuncAlias<LeidenAliasFunction>(db);
    ExtensionUtils::addTableFunc<SpanningForest>(db);
    ExtensionUtils::addTableFuncAlias<SpanningForestAliasFunction>(db);
    ExtensionUtils::addTableFunc<SubgraphIsomorphismFunction>(db);
    ExtensionUtils::addTableFunc<TopologicalSortFunction>(db);
    ExtensionUtils::addTableFunc<GraphDiffFunction>(db);
    ExtensionUtils::addTableFunc<GraphEditDistanceFunction>(db);
    ExtensionUtils::addTableFunc<GraphSignatureFunction>(db);
    ExtensionUtils::addTableFunc<BetweennessFunction>(db);
    ExtensionUtils::addTableFunc<LocalClusteringCoefficientFunction>(db);
    ExtensionUtils::addTableFunc<KatzCentralityFunction>(db);
    ExtensionUtils::addTableFunc<AssortativityFunction>(db);
    ExtensionUtils::addTableFunc<BridgesFunction>(db);
    ExtensionUtils::addTableFunc<ArticulationPointsFunction>(db);
    ExtensionUtils::addTableFunc<ShortestPathFunction>(db);
    // NASH × GNN P1 batch (ALGORITHM-SPEC-merged.md §5.1)
    ExtensionUtils::addTableFunc<BiconnectedComponentsFunction>(db);
    ExtensionUtils::addTableFunc<KTrussFunction>(db);
    ExtensionUtils::addTableFunc<EigenvectorCentralityFunction>(db);
    ExtensionUtils::addTableFunc<ClosenessCentralityFunction>(db);
    ExtensionUtils::addTableFunc<ApproxBetweennessFunction>(db);
    ExtensionUtils::addTableFunc<SpanningEdgeCentralityFunction>(db);
    ExtensionUtils::addTableFunc<GroupBetweennessFunction>(db);
    ExtensionUtils::addTableFunc<GroupClosenessFunction>(db);
    ExtensionUtils::addTableFunc<LabelPropagationFunction>(db);
    ExtensionUtils::addTableFunc<CutClusteringFunction>(db);
    ExtensionUtils::addTableFunc<DijkstraFunction>(db);
    ExtensionUtils::addTableFunc<AllPairsShortestPathFunction>(db);
    ExtensionUtils::addTableFunc<DiameterFunction>(db);
    ExtensionUtils::addTableFunc<EffectiveDiameterFunction>(db);
    ExtensionUtils::addTableFunc<MaxFlowFunction>(db);
    ExtensionUtils::addTableFunc<MinCutValueFunction>(db);
    ExtensionUtils::addTableFunc<TriangleCountFunction>(db);
    ExtensionUtils::addTableFunc<GlobalClusteringFunction>(db);
    ExtensionUtils::addTableFunc<LinkPredictionFunction>(db);
    // P2 batch (NASH×GNN advanced analysis).
    ExtensionUtils::addTableFunc<EccentricityFunction>(db);
    ExtensionUtils::addTableFunc<RadiusFunction>(db);
    ExtensionUtils::addTableFunc<HitsFunction>(db);
    ExtensionUtils::addTableFunc<SirSimulatorFunction>(db);
    ExtensionUtils::addTableFunc<LocalDegreeSparsificationFunction>(db);
    ExtensionUtils::addTableFunc<GlobalThresholdFilterFunction>(db);
    ExtensionUtils::addTableFunc<RandomEdgeSparsificationFunction>(db);
    ExtensionUtils::addTableFunc<TriangleFilterFunction>(db);
    // P2 batch 2 (dynamic/coarsening/spectral/matching/metrics).
    ExtensionUtils::addTableFunc<DynConnectedComponentsFunction>(db);
    ExtensionUtils::addTableFunc<DynKatzCentralityFunction>(db);
    ExtensionUtils::addTableFunc<Dyn2HopLandmarkFunction>(db);
    ExtensionUtils::addTableFunc<GraphCoarseningFunction>(db);
    ExtensionUtils::addTableFunc<PartitionIntersectionFunction>(db);
    ExtensionUtils::addTableFunc<SpectralPartitioningFunction>(db);
    ExtensionUtils::addTableFunc<MaxWeightMatchingFunction>(db);
    ExtensionUtils::addTableFunc<CommuteTimeDistanceFunction>(db);
    // P3 batch (NASH×GNN generators).
    ExtensionUtils::addTableFunc<BarabasiAlbertFunction>(db);
    ExtensionUtils::addTableFunc<WattsStrogatzFunction>(db);
    ExtensionUtils::addTableFunc<StochasticBlockmodelFunction>(db);
    ExtensionUtils::addTableFunc<GraphRandomizationFunction>(db);
}

} // namespace algo_extension
} // namespace lbug

#if defined(BUILD_DYNAMIC_LOAD)
extern "C" {
// Because we link against the static library on windows, we implicitly inherit LBUG_STATIC_DEFINE,
// which cancels out any exporting, so we can't use LBUG_API.
#if defined(_WIN32)
#define INIT_EXPORT __declspec(dllexport)
#else
#define INIT_EXPORT __attribute__((visibility("default")))
#endif
INIT_EXPORT void init(lbug::main::ClientContext* context) {
    lbug::algo_extension::AlgoExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::algo_extension::AlgoExtension::EXTENSION_NAME;
}
}
#endif
