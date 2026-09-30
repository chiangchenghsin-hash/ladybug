# hyperalgo extension:LOAD EXTENSION + CALL 表函数,hyperalgo-core(独立 header-only C++17)的薄绑定
# 依据:升级计划 v1.3 纪律 6 / P-1.5 v1.2 §1.1(核心只认 CSR)、§3(端点表);独立扩展不进 libalgo(§5 条款 1)
include_directories(
        ${PROJECT_SOURCE_DIR}/src/include
        ${CMAKE_BINARY_DIR}/src/include
        src/include
        src/function
        src/main
        ${PROJECT_SOURCE_DIR}/hyperalgo-core/include)

add_subdirectory(src/main)
add_subdirectory(src/function)
add_subdirectory(test)

build_extension_lib(${BUILD_STATIC_EXTENSION} "hyperalgo")
