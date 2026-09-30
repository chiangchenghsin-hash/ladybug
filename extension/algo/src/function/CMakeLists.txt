add_subdirectory(config)

add_library(lbug_algo_function
        OBJECT
        component_ids.cpp
        strongly_connected_components.cpp
        strongly_connected_components_kosaraju.cpp
        weakly_connected_components.cpp
        page_rank.cpp
        k_core_decomposition.cpp
        louvain.cpp
        leiden.cpp
        spanning_forest.cpp
        subgraph_isomorphism.cpp
        topological_sort.cpp
        graph_diff.cpp
        graph_edit_distance.cpp
        graph_signature.cpp
        betweenness.cpp
        local_clustering.cpp
        katz_centrality.cpp
        assortativity.cpp
        bridges.cpp
        articulation_points.cpp
        shortest_path.cpp
        biconnected_components.cpp
        k_truss.cpp
        eigenvector_centrality.cpp
        closeness_centrality.cpp
        approx_betweenness.cpp
        spanning_edge_centrality.cpp
        group_centrality.cpp
        label_propagation.cpp
        cut_clustering.cpp
        dijkstra.cpp
        all_pairs_shortest_path.cpp
        diameter.cpp
        effective_diameter.cpp
        max_flow.cpp
        triangle_count.cpp
        link_prediction.cpp
        eccentricity.cpp
        hits.cpp
        sir_simulator.cpp
        local_degree_sparsification.cpp
        global_threshold_filter.cpp
        random_edge_sparsification.cpp
        triangle_filter.cpp
        dyn_connected_components.cpp
        dyn_katz_centrality.cpp
        dyn_2hop_landmark.cpp
        graph_coarsening.cpp
        partition_intersection.cpp
        spectral_partitioning.cpp
        max_weight_matching.cpp
        commute_time_distance.cpp
        generate_barabasi_albert.cpp
        generate_watts_strogatz.cpp
        generate_stochastic_blockmodel.cpp
        graph_randomization.cpp
        )

# gds_page_rank.cpp includes NetworKit + Arrow headers; only build the icebug-backed
# GDS_* algorithms when ENABLE_ICEBUG is on (stock builds must not need Arrow/OpenMP).
if (ENABLE_ICEBUG)
    target_sources(lbug_algo_function PRIVATE gds_page_rank.cpp)
    # Arrow pulls in <omp.h>; linking the imported targets to the object library
    # propagates their compile flags + include dirs to it.
    target_link_libraries(lbug_algo_function PRIVATE Arrow::arrow_shared OpenMP::OpenMP_CXX)
endif ()

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:lbug_algo_function>
        PARENT_SCOPE)