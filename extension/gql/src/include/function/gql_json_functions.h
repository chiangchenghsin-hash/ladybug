#pragma once

#include "common/vector/value_vector.h"
#include "function/function.h"

namespace lbug {
namespace gql_extension {

// GQL extension functions backing mixed-type comparison:
//   _GQL_TO_JSON(ANY) -> JSON          encode any value as JSON text
//   _GQL_MAX(ANY) -> <argument type>   max over values (bindFunc pins the
//   _GQL_MIN(ANY) -> <argument type>   result type to the input's logical type)
// The `_GQL_` prefix keeps them clear of the JSON extension's `to_json` (the
// catalog is case-insensitive; `extension::addFunc` is idempotent).

struct GqlToJsonFunction {
    static constexpr const char* name = "_GQL_TO_JSON";

    static function::function_set getFunctionSet();

    static void execFunc(const std::vector<std::shared_ptr<common::ValueVector>>& parameters,
        const std::vector<common::SelectionVector*>& parameterSelVectors,
        common::ValueVector& result, common::SelectionVector* resultSelVector,
        void* /*dataPtr*/);
};

struct GqlMaxFunction {
    static constexpr const char* name = "_GQL_MAX";

    static function::function_set getFunctionSet();
};

struct GqlMinFunction {
    static constexpr const char* name = "_GQL_MIN";

    static function::function_set getFunctionSet();
};

} // namespace gql_extension
} // namespace lbug
