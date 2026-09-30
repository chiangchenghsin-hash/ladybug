# --- icebug (NetworKit fork) + Arrow, for the icebug-backed GDS_* algorithms ---
# Pulls the prebuilt icebug release into vendor/; Arrow + OpenMP are system deps.
# ENABLE_ICEBUG defaults to OFF so a stock build links no icebug/Arrow/OpenMP.
option(ENABLE_ICEBUG "Build the icebug-backed GDS_* algorithms (needs Apache Arrow + OpenMP)" OFF)
if (ENABLE_ICEBUG)
    add_compile_definitions(ENABLE_ICEBUG)
    include(cmake/download_icebug.cmake)
    find_package(Arrow REQUIRED)
    find_package(OpenMP REQUIRED)
endif ()

include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        ${ICEBUG_INCLUDE_DIR})

add_subdirectory(src/main)
add_subdirectory(src/function)
add_subdirectory(src/common)
add_subdirectory(test)

build_extension_lib(${BUILD_STATIC_EXTENSION} "algo")

if (ENABLE_ICEBUG)
    if (BUILD_STATIC_EXTENSION)
        set(_algo_target lbug_algo_static_extension)
    else ()
        set(_algo_target lbug_algo_extension)
    endif ()
    # The NetworKit Windows build ships a separate networkit_state.lib;
    # other platforms carry that code inside the main libnetworkit.
    if (WIN32)
        set(_networkit_state_lib ${ICEBUG_RPATH_DIR}/networkit/networkit_state.lib)
    else ()
        set(_networkit_state_lib)
    endif ()
    target_link_libraries(${_algo_target} PRIVATE
            ${ICEBUG_LIB}
            ${_networkit_state_lib}
            Arrow::arrow_shared
            OpenMP::OpenMP_CXX)
    # Let the loaded extension find a shared libnetworkit (prebuilt path); harmless for a static lib.
    set_target_properties(${_algo_target} PROPERTIES
            BUILD_RPATH "${ICEBUG_RPATH_DIR}"
            INSTALL_RPATH "${ICEBUG_RPATH_DIR}")
endif ()