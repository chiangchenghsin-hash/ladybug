#pragma once

#include <algorithm>
#include <string>

#include "common/string_utils.h"

namespace lbug {
namespace algo_extension {

// Optional-parameter name matching for the NASH/GNN P1 batch: case-insensitive
// and underscore-insensitive, so `source_node`, `sourcenode` and `SourceNode`
// are all accepted (the older libalgo params such as `maxiterations` keep
// working because underscores are simply stripped before comparison).
inline std::string normalizeParamName(const std::string& alias) {
    std::string lowered = lbug::common::StringUtils::getLower(alias);
    lowered.erase(std::remove(lowered.begin(), lowered.end(), '_'), lowered.end());
    return lowered;
}

} // namespace algo_extension
} // namespace lbug
