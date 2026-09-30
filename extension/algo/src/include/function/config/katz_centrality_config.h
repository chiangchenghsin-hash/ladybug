#pragma once

#include <string>

#include "common/exception/binder.h"
#include "common/types/types.h"

namespace lbug {
namespace algo_extension {

// Katz centrality damping factor: x = beta * 1 + alpha * A^T x.
// Must satisfy alpha < 1/lambda_max for convergence; non-convergence is reported
// as a BinderException (never silently truncated).
struct KatzAlpha {
    static constexpr const char* NAME = "alpha";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 0.05;

    static void validate(double alpha) {
        if (alpha < 0) {
            throw common::BinderException{"Katz alpha must be >= 0."};
        }
    }
};

// Constant bias added to every node's score each iteration.
struct KatzBeta {
    static constexpr const char* NAME = "beta";
    static constexpr common::LogicalTypeID TYPE = common::LogicalTypeID::DOUBLE;
    static constexpr double DEFAULT_VALUE = 1.0;
};

} // namespace algo_extension
} // namespace lbug
