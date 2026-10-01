#pragma once

#include "common/vector/value_vector.h"
#include "function/function.h"

namespace lbug {
namespace gql_extension {

// GQL extension functions backing mixed-type comparison and GQL aggregation:
//   _GQL_TO_JSON(ANY) -> JSON          encode any value as JSON text
//   _GQL_MAX(ANY) -> <argument type>   max over values (bindFunc pins the
//   _GQL_MIN(ANY) -> <argument type>   result type to the input's logical type)
//   _GQL_SUM(ANY) -> INT128/UINT128/DOUBLE/JSON   sum over numeric or JSON
//   _GQL_AVG(ANY) -> DOUBLE/JSON                  values (bindFunc pins the
//                                                  result type likewise)
//   _GQL_LT/_GQL_LE/_GQL_GT/_GQL_GE(ANY, ANY) -> BOOL
//                                      GQL total-order comparisons; operands
//                                      are classified per value from their own
//                                      logical type (no bindFunc pins them)
//   _GQL_SORTKEY(ANY) -> STRING        byte-comparable encoding of the same
//                                      total order for ORDER BY
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

struct GqlSumFunction {
    static constexpr const char* name = "_GQL_SUM";

    static function::function_set getFunctionSet();
};

struct GqlAvgFunction {
    static constexpr const char* name = "_GQL_AVG";

    static function::function_set getFunctionSet();
};

struct GqlLtFunction {
    static constexpr const char* name = "_GQL_LT";

    static function::function_set getFunctionSet();
};

struct GqlLeFunction {
    static constexpr const char* name = "_GQL_LE";

    static function::function_set getFunctionSet();
};

struct GqlGtFunction {
    static constexpr const char* name = "_GQL_GT";

    static function::function_set getFunctionSet();
};

struct GqlGeFunction {
    static constexpr const char* name = "_GQL_GE";

    static function::function_set getFunctionSet();
};

struct GqlSortKeyFunction {
    static constexpr const char* name = "_GQL_SORTKEY";

    static function::function_set getFunctionSet();
};

} // namespace gql_extension
} // namespace lbug
