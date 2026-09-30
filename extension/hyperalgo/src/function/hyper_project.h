#pragma once

#include "function/function.h"

namespace lbug {
namespace hyperalgo_extension {

// CALL hyper_project('g', ['Agent'], ['FLOWS']) → graph, n_nodes, n_edges,
//     isolated_stations, degenerate_edges, fingerprint(FRZ-10;P-1.5 §3.1)
struct HyperProjectFunction {
    static constexpr const char* name = "HYPER_PROJECT";

    static function::function_set getFunctionSet();
};

} // namespace hyperalgo_extension
} // namespace lbug
