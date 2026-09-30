# hyperalgo-core:header-only C++17,零 LadybugDB 依赖(独立可建可测)
# 依据:升级计划 v1.3 纪律 6(唯一算法实现)、P-1.5 v1.2(三端同源码)
cmake_minimum_required(VERSION 3.15)
project(hyperalgo_core VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED TRUE)

# 源文件为 UTF-8 无 BOM(中文注释);MSVC 需显式 /utf-8,否则按系统代码页解析报语法错
if(MSVC)
    add_compile_options(/utf-8)
endif()

add_library(hyperalgo_core INTERFACE)
target_include_directories(hyperalgo_core INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_compile_features(hyperalgo_core INTERFACE cxx_std_17)

option(HYPERALGO_BUILD_TESTS "Build hyperalgo-core standalone tests" ON)
if(HYPERALGO_BUILD_TESTS)
    enable_testing()
    add_executable(hyperalgo_tests test/test_main.cpp)
    target_link_libraries(hyperalgo_tests PRIVATE hyperalgo_core)
    add_test(NAME hyperalgo_tests COMMAND hyperalgo_tests)
    # 对拍 CLI(compare_scc.py 消费;零依赖)
    add_executable(hyperalgo_cli tools/hyperalgo_cli.cpp)
    target_link_libraries(hyperalgo_cli PRIVATE hyperalgo_core)
endif()
