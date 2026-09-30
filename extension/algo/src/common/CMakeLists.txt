add_library(algo_extension_common
        OBJECT
        in_mem_graph.cpp
        in_mem_gds_utils.cpp
        directed_csr.cpp
        weighted_graph.cpp
        max_flow.cpp
        vf2pp_core.cpp
        ged_core.cpp
        graph_signature.cpp
        )

set(ALGO_EXTENSION_OBJECT_FILES
        ${ALGO_EXTENSION_OBJECT_FILES} $<TARGET_OBJECTS:algo_extension_common>
        PARENT_SCOPE)
