#include "function/gql_json_functions.h"

#include <algorithm>
#include <cstring>
#include <format>
#include <memory>
#include <type_traits>

#include "common/assert.h"
#include "common/exception/runtime.h"
#include "common/json_utils.h"
#include "common/type_utils.h"
#include "common/types/json_type.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "function/aggregate_function.h"
#include "function/comparison/comparison_functions.h"
#include "function/scalar_function.h"
#include "yyjson.h"

namespace lbug {
namespace gql_extension {

using namespace lbug::common;
using namespace lbug::function;
using namespace lbug::json_extension;

// =============================================================================
// _GQL_TO_JSON(ANY) -> JSON
// =============================================================================
// Mirrors extension/json's to_json: INT64 encodes as a JSON integer (`5`),
// DOUBLE as a JSON real (`5.0`; yyjson appends `.0` to integer-valued reals),
// STRING as a quoted JSON string, lists as compact JSON arrays, and a NULL
// input yields a NULL result (never the text `null`). JSON-typed input passes
// through byte-for-byte: it is already JSON text, and re-serializing it would
// rewrite formatting the caller may rely on.

void GqlToJsonFunction::execFunc(const std::vector<std::shared_ptr<ValueVector>>& parameters,
    const std::vector<SelectionVector*>& parameterSelVectors, ValueVector& result,
    SelectionVector* resultSelVector, void* /*dataPtr*/) {
    DASSERT(parameters.size() == 1);
    result.resetAuxiliaryBuffer();
    for (auto i = 0u; i < resultSelVector->getSelSize(); ++i) {
        auto inputPos = (*parameterSelVectors[0])[i];
        auto resultPos = (*resultSelVector)[i];
        auto isNull = parameters[0]->isNull(inputPos);
        result.setNull(resultPos, isNull);
        if (isNull) {
            continue;
        }
        if (JsonType::isJson(parameters[0]->dataType)) {
            auto jsonVal = parameters[0]->getValue<string_t>(inputPos);
            StringVector::addString(&result, resultPos, jsonVal);
        } else {
            StringVector::addString(&result, resultPos,
                jsonToString(jsonify(*parameters[0], inputPos)));
        }
    }
}

static std::unique_ptr<FunctionBindData> bindToJson(const ScalarBindFuncInput& input) {
    DASSERT(input.arguments.size() == 1);
    LogicalType type = input.arguments[0]->dataType.copy();
    if (type.getLogicalTypeID() == LogicalTypeID::ANY) {
        type = LogicalType::INT64();
    }
    auto bindData = std::make_unique<FunctionBindData>(JsonType::getJsonType());
    bindData->paramTypes.push_back(std::move(type));
    return bindData;
}

function_set GqlToJsonFunction::getFunctionSet() {
    function_set result;
    auto func = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::JSON, execFunc);
    func->bindFunc = bindToJson;
    result.push_back(std::move(func));
    return result;
}

// =============================================================================
// _GQL_MAX / _GQL_MIN(ANY) -> <argument type>
// =============================================================================

namespace {

// Aggregate state for _GQL_MAX/_GQL_MIN.
//
// Contract (see AggregateState in function/aggregate_function.h): states are
// memcpy'd into factorized tables and their destructors never run, so every
// member must be trivially destructible. The winning value lives in `raw` as
// its physical representation; STRING/BLOB/JSON values store a string_t
// header whose overflow bytes are copied into the aggregate's
// InMemOverflowBuffer exactly like MinMaxFunction<string_t>::setVal
// (src/include/function/aggregate/min_max.h).
//
// `storedType` is the bound argument's logical type: one query binds one
// type, so it is constant for the lifetime of a state. ANY only ever appears
// while `isNull` is still true (no value stored yet).
struct GqlMinMaxState : public AggregateStateWithNull {
    // Largest physical representation: string_t / int128_t / uint128_t.
    static constexpr uint32_t MAX_VALUE_SIZE = 16;

    LogicalTypeID storedType = LogicalTypeID::ANY;
    alignas(8) uint8_t raw[MAX_VALUE_SIZE] = {};

    uint32_t getStateSize() const override { return sizeof(*this); }
    void writeToVector(ValueVector* outputVector, uint64_t pos) override;

    template<typename T>
    T load() const {
        T val;
        memcpy(&val, raw, sizeof(T));
        return val;
    }

    // Copies `val_` into the state, allocating overflow space for long strings
    // (same reuse policy as MinMaxFunction<string_t>::setVal: only grow when
    // the new value is a long string longer than the stored one).
    template<typename T>
    void setVal(const T& val_, InMemOverflowBuffer* overflowBuffer) {
        if constexpr (std::is_same_v<T, string_t>) {
            string_t dst;
            memcpy(&dst, raw, sizeof(string_t));
            if (val_.len > string_t::SHORT_STR_LENGTH && val_.len > dst.len) {
                dst.overflowPtr =
                    reinterpret_cast<uint64_t>(overflowBuffer->allocateSpace(val_.len));
            }
            dst.set(val_);
            memcpy(raw, &dst, sizeof(string_t));
        } else {
            memcpy(raw, &val_, sizeof(T));
        }
    }
};

// The type families built-in MIN/MAX instantiate (min_max.cpp loops over
// LogicalTypeUtils::getAllValidComparableLogicalTypes()), plus JSON. Anything
// else — LIST, STRUCT, MAP, NODE, ANY, ... — is rejected loudly rather than
// silently mis-ordered. Function-local static: built once, cheap to scan.
bool isSupportedInput(const LogicalType& type) {
    if (type.getLogicalTypeID() == LogicalTypeID::JSON) {
        return true;
    }
    static const std::vector<LogicalTypeID> supportedTypes =
        LogicalTypeUtils::getAllValidComparableLogicalTypes();
    return std::find(supportedTypes.begin(), supportedTypes.end(), type.getLogicalTypeID()) !=
           supportedTypes.end();
}

std::string unsupportedTypeError(const LogicalType& type) {
    return std::format("_GQL_MAX/_GQL_MIN: unsupported input type {}",
        LogicalTypeUtils::toString(type.getLogicalTypeID()));
}

void GqlMinMaxState::writeToVector(ValueVector* outputVector, uint64_t pos) {
    if (storedType == LogicalTypeID::ANY) {
        throw RuntimeException("_GQL_MAX/_GQL_MIN: no value to materialize");
    }
    // JSON visits as string_t (PhysicalTypeID::JSON), so JSON results are
    // written like STRING results: raw JSON text into a JSON-typed vector.
    TypeUtils::visit(LogicalType::getPhysicalType(storedType),
        [&]<ComparableTypes T>(T) { outputVector->setValue<T>(pos, load<T>()); },
        [&](auto) -> void { throw RuntimeException(unsupportedTypeError(LogicalType(storedType))); });
}

void storeFromVector(GqlMinMaxState* state, ValueVector* input, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    state->storedType = input->dataType.getLogicalTypeID();
    TypeUtils::visit(input->dataType.getPhysicalType(),
        [&]<ComparableTypes T>(T) {
            state->template setVal<T>(input->getValue<T>(pos), overflowBuffer);
        },
        [&](auto) -> void {
            throw RuntimeException(unsupportedTypeError(input->dataType));
        });
}

// Merges `other` into `state`. The incoming string_t header may point into
// `other`'s overflow buffer, so the bytes are re-copied through setVal into
// the combine-provided buffer — the same re-copy MinMaxFunction::combine does.
void storeFromState(GqlMinMaxState* state, const GqlMinMaxState* other,
    InMemOverflowBuffer* overflowBuffer) {
    state->storedType = other->storedType;
    TypeUtils::visit(LogicalType::getPhysicalType(other->storedType),
        [&]<ComparableTypes T>(T) {
            state->template setVal<T>(other->template load<T>(), overflowBuffer);
        },
        [&](auto) -> void {
            throw RuntimeException(unsupportedTypeError(LogicalType(other->storedType)));
        });
}

// -----------------------------------------------------------------------------
// JSON total order
// -----------------------------------------------------------------------------
// _GQL_MAX/_GQL_MIN parse JSON text and order values under the GQL/Cypher
// total order. Cross-class ranking (ascending):
//
//   null(0) < bool(1) < array(2) < string(3) < number(4) < object(5)
//
// The relative order of array/string/number is pinned by TCK
// test/tck/features/expressions/aggregation/Aggregation2.feature:
//   [12] min([1,'a',null,[1,2],0.2,'b']) = [1,2]
//        => array < string and array < number
//   [11] max([1,'a',null,[1,2],0.2,'b']) = 1
//        => string < number
// i.e. array < string < number — the reverse of CIP2016-06-14's
// Number < String < List rank for that block. Booleans and objects keep their
// CIP2016-06-14 positions relative to the block (Boolean below everything,
// Map/object above everything), which CIP does not conflict with here; JSON
// null is the lowest value (reachable only inside nested arrays, and matching
// the engine's list comparison, which orders null below every non-null
// element). Un-orderable pairs (distinct objects) throw rather than guess.
//
// Within a class:
//   numbers  — int/real/uint compare numerically regardless of text form
//              (TCK [5]/[6]); ints compare exactly, a real operand folds both
//              sides to double (ints beyond 2^53 against a non-integral real
//              inherit double's usual precision caveat).
//   strings  — byte-wise lexicographic (TCK [7]/[8]: 'b' > 'B' > 'abc').
//   arrays   — element-wise lexicographic, shorter prefix is smaller
//              (TCK [9]/[10]: [1] < [2] < [2,1]).
//   bools    — false < true.
int jsonRank(yyjson_val* val) {
    if (yyjson_is_null(val)) {
        return 0;
    }
    if (yyjson_is_bool(val)) {
        return 1;
    }
    if (yyjson_is_arr(val)) {
        return 2;
    }
    if (yyjson_is_str(val)) {
        return 3;
    }
    if (yyjson_is_num(val)) {
        return 4;
    }
    if (yyjson_is_obj(val)) {
        return 5;
    }
    throw RuntimeException("_GQL_MAX/_GQL_MIN: unsupported JSON value in comparison");
}

double jsonNumAsDouble(yyjson_val* val) {
    if (yyjson_is_real(val)) {
        return yyjson_get_real(val);
    }
    if (yyjson_is_sint(val)) {
        return static_cast<double>(yyjson_get_sint(val));
    }
    return static_cast<double>(yyjson_get_uint(val));
}

int compareJsonNumbers(yyjson_val* lhs, yyjson_val* rhs) {
    if (yyjson_is_int(lhs) && yyjson_is_int(rhs)) {
        if (yyjson_is_uint(lhs) && yyjson_is_uint(rhs)) {
            auto l = yyjson_get_uint(lhs);
            auto r = yyjson_get_uint(rhs);
            return l < r ? -1 : (l > r ? 1 : 0);
        }
        if (yyjson_is_sint(lhs) && yyjson_is_sint(rhs)) {
            auto l = yyjson_get_sint(lhs);
            auto r = yyjson_get_sint(rhs);
            return l < r ? -1 : (l > r ? 1 : 0);
        }
        // Mixed signedness: a negative sint always sorts below any uint.
        if (yyjson_is_sint(lhs)) {
            auto l = yyjson_get_sint(lhs);
            if (l < 0) {
                return -1;
            }
            auto lu = static_cast<uint64_t>(l);
            auto r = yyjson_get_uint(rhs);
            return lu < r ? -1 : (lu > r ? 1 : 0);
        }
        auto r = yyjson_get_sint(rhs);
        if (r < 0) {
            return 1;
        }
        auto l = yyjson_get_uint(lhs);
        auto ru = static_cast<uint64_t>(r);
        return l < ru ? -1 : (l > ru ? 1 : 0);
    }
    auto l = jsonNumAsDouble(lhs);
    auto r = jsonNumAsDouble(rhs);
    return l < r ? -1 : (l > r ? 1 : 0);
}

// Structural equality over parsed JSON (all-inline yyjson API, no link-time
// dependency on the yyjson library): used so equal objects nested inside
// arrays compare equal instead of tripping the object-order guard below.
bool jsonDeepEquals(yyjson_val* lhs, yyjson_val* rhs) {
    if (jsonRank(lhs) != jsonRank(rhs)) {
        return false;
    }
    if (yyjson_is_null(lhs)) {
        return true;
    }
    if (yyjson_is_bool(lhs)) {
        return yyjson_get_bool(lhs) == yyjson_get_bool(rhs);
    }
    if (yyjson_is_num(lhs)) {
        return compareJsonNumbers(lhs, rhs) == 0;
    }
    if (yyjson_is_str(lhs)) {
        auto len = yyjson_get_len(lhs);
        return len == yyjson_get_len(rhs) &&
               memcmp(yyjson_get_str(lhs), yyjson_get_str(rhs), len) == 0;
    }
    if (yyjson_is_arr(lhs)) {
        auto size = yyjson_arr_size(lhs);
        if (size != yyjson_arr_size(rhs)) {
            return false;
        }
        for (auto i = 0u; i < size; ++i) {
            if (!jsonDeepEquals(yyjson_arr_get(lhs, i), yyjson_arr_get(rhs, i))) {
                return false;
            }
        }
        return true;
    }
    auto size = yyjson_obj_size(lhs);
    if (size != yyjson_obj_size(rhs)) {
        return false;
    }
    yyjson_val* key = nullptr;
    yyjson_obj_iter iter;
    yyjson_obj_iter_init(lhs, &iter);
    while ((key = yyjson_obj_iter_next(&iter)) != nullptr) {
        auto lhsVal = yyjson_obj_iter_get_val(key);
        auto rhsVal = yyjson_obj_getn(rhs, yyjson_get_str(key), yyjson_get_len(key));
        if (rhsVal == nullptr || !jsonDeepEquals(lhsVal, rhsVal)) {
            return false;
        }
    }
    return true;
}

int compareJsonValues(yyjson_val* lhs, yyjson_val* rhs) {
    auto lhsRank = jsonRank(lhs);
    auto rhsRank = jsonRank(rhs);
    if (lhsRank != rhsRank) {
        return lhsRank < rhsRank ? -1 : 1;
    }
    if (yyjson_is_null(lhs)) {
        return 0;
    }
    if (yyjson_is_bool(lhs)) {
        auto l = yyjson_get_bool(lhs);
        auto r = yyjson_get_bool(rhs);
        return l == r ? 0 : (l ? 1 : -1);
    }
    if (yyjson_is_arr(lhs)) {
        auto lhsSize = yyjson_arr_size(lhs);
        auto rhsSize = yyjson_arr_size(rhs);
        auto commonSize = std::min(lhsSize, rhsSize);
        for (auto i = 0u; i < commonSize; ++i) {
            auto cmp = compareJsonValues(yyjson_arr_get(lhs, i), yyjson_arr_get(rhs, i));
            if (cmp != 0) {
                return cmp;
            }
        }
        // Shorter prefix sorts smaller (Cypher list order).
        return lhsSize < rhsSize ? -1 : (lhsSize > rhsSize ? 1 : 0);
    }
    if (yyjson_is_str(lhs)) {
        auto lhsLen = yyjson_get_len(lhs);
        auto rhsLen = yyjson_get_len(rhs);
        auto cmp = memcmp(yyjson_get_str(lhs), yyjson_get_str(rhs), std::min(lhsLen, rhsLen));
        if (cmp != 0) {
            return cmp;
        }
        return lhsLen < rhsLen ? -1 : (lhsLen > rhsLen ? 1 : 0);
    }
    if (yyjson_is_num(lhs)) {
        return compareJsonNumbers(lhs, rhs);
    }
    // Objects: equal content is fine, distinct content is un-orderable.
    if (jsonDeepEquals(lhs, rhs)) {
        return 0;
    }
    throw RuntimeException(
        "_GQL_MAX/_GQL_MIN: JSON objects do not have a well-defined total order");
}

std::string jsonTextSnippet(const string_t& val) {
    constexpr uint32_t MAX_SNIPPET = 64;
    auto len = std::min<uint32_t>(val.len, MAX_SNIPPET);
    std::string snippet(reinterpret_cast<const char*>(val.getData()), len);
    if (val.len > MAX_SNIPPET) {
        snippet += "...";
    }
    return snippet;
}

// -1 / 0 / +1 under the order documented at compareJsonValues. Throws loudly
// on invalid JSON text or un-orderable content — never a silent wrong answer.
int compareJsonTexts(const string_t& lhs, const string_t& rhs) {
    if (lhs.len == rhs.len && memcmp(lhs.getData(), rhs.getData(), lhs.len) == 0) {
        return 0;
    }
    auto parse = [](const string_t& val) {
        auto doc = stringToJsonNoError(
            std::string(reinterpret_cast<const char*>(val.getData()), val.len));
        if (doc.ptr == nullptr) {
            throw RuntimeException(
                std::format("_GQL_MAX/_GQL_MIN: invalid JSON value: {}", jsonTextSnippet(val)));
        }
        return doc;
    };
    auto lhsDoc = parse(lhs);
    auto rhsDoc = parse(rhs);
    return compareJsonValues(yyjson_doc_get_root(lhsDoc.ptr), yyjson_doc_get_root(rhsDoc.ptr));
}

// -----------------------------------------------------------------------------
// Update / combine
// -----------------------------------------------------------------------------

// True when the candidate value at `pos` beats the state's current winner.
// IS_MAX=true orders with GreaterThan (built-in MAX semantics), false with
// LessThan (built-in MIN semantics).
template<bool IS_MAX>
bool candidateWins(GqlMinMaxState* state, ValueVector* input, uint32_t pos) {
    if (state->storedType == LogicalTypeID::JSON) {
        auto candidate = input->getValue<string_t>(pos);
        auto winner = state->load<string_t>();
        auto cmp = compareJsonTexts(candidate, winner);
        return IS_MAX ? cmp > 0 : cmp < 0;
    }
    using OP = std::conditional_t<IS_MAX, GreaterThan, LessThan>;
    return TypeUtils::visit(input->dataType.getPhysicalType(),
        [&]<ComparableTypes T>(T) {
            uint8_t cmp = 0;
            OP::template operation<T, T>(input->getValue<T>(pos), state->template load<T>(), cmp,
                nullptr /* leftVector */, nullptr /* rightVector */);
            return cmp != 0;
        },
        [&](auto) -> bool { throw RuntimeException(unsupportedTypeError(input->dataType)); });
}

// True when `other`'s stored value beats `state`'s current winner.
template<bool IS_MAX>
bool otherWins(GqlMinMaxState* state, const GqlMinMaxState* other) {
    if (state->storedType == LogicalTypeID::JSON) {
        auto cmp = compareJsonTexts(other->load<string_t>(), state->load<string_t>());
        return IS_MAX ? cmp > 0 : cmp < 0;
    }
    using OP = std::conditional_t<IS_MAX, GreaterThan, LessThan>;
    return TypeUtils::visit(LogicalType::getPhysicalType(state->storedType),
        [&]<ComparableTypes T>(T) {
            uint8_t cmp = 0;
            OP::template operation<T, T>(other->template load<T>(), state->template load<T>(), cmp,
                nullptr /* leftVector */, nullptr /* rightVector */);
            return cmp != 0;
        },
        [&](auto) -> bool {
            throw RuntimeException(unsupportedTypeError(LogicalType(state->storedType)));
        });
}

template<bool IS_MAX>
void updateSingle(GqlMinMaxState* state, ValueVector* input, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    if (input->isNull(pos)) {
        return;
    }
    // Reject unsupported types on the first real value: all-null inputs of an
    // unknown type still produce NULL (no comparison happened), anything else
    // must fail loudly instead of guessing an order.
    if (!isSupportedInput(input->dataType)) {
        throw RuntimeException(unsupportedTypeError(input->dataType));
    }
    if (state->isNull) {
        storeFromVector(state, input, pos, overflowBuffer);
        state->isNull = false;
        return;
    }
    if (state->storedType != input->dataType.getLogicalTypeID()) {
        throw RuntimeException(std::format("_GQL_MAX/_GQL_MIN: cannot compare {} with {}",
            LogicalTypeUtils::toString(state->storedType),
            LogicalTypeUtils::toString(input->dataType.getLogicalTypeID())));
    }
    if (candidateWins<IS_MAX>(state, input, pos)) {
        storeFromVector(state, input, pos, overflowBuffer);
    }
}

template<bool IS_MAX>
void updateAll(uint8_t* state_, ValueVector* input, uint64_t /*multiplicity*/,
    InMemOverflowBuffer* overflowBuffer) {
    DASSERT(!input->state->isFlat());
    auto* state = reinterpret_cast<GqlMinMaxState*>(state_);
    input->forEachNonNull(
        [&](auto pos) { updateSingle<IS_MAX>(state, input, pos, overflowBuffer); });
}

template<bool IS_MAX>
void updatePos(uint8_t* state_, ValueVector* input, uint64_t /*multiplicity*/, uint32_t pos,
    InMemOverflowBuffer* overflowBuffer) {
    // The caller filters nulls, but filtering here too is harmless.
    updateSingle<IS_MAX>(reinterpret_cast<GqlMinMaxState*>(state_), input, pos, overflowBuffer);
}

template<bool IS_MAX>
void combine(uint8_t* state_, uint8_t* otherState_, InMemOverflowBuffer* overflowBuffer) {
    auto* other = reinterpret_cast<GqlMinMaxState*>(otherState_);
    if (other->isNull) {
        return;
    }
    auto* state = reinterpret_cast<GqlMinMaxState*>(state_);
    if (state->isNull) {
        storeFromState(state, other, overflowBuffer);
        state->isNull = false;
        return;
    }
    if (state->storedType != other->storedType) {
        throw RuntimeException(std::format("_GQL_MAX/_GQL_MIN: cannot combine {} with {}",
            LogicalTypeUtils::toString(state->storedType),
            LogicalTypeUtils::toString(other->storedType)));
    }
    if (otherWins<IS_MAX>(state, other)) {
        storeFromState(state, other, overflowBuffer);
    }
}

std::unique_ptr<AggregateState> initialize() {
    return std::make_unique<GqlMinMaxState>();
}

// No-op: empty / all-null groups stay NULL through the default
// AggregateStateWithNull.isNull path (needToHandleNulls stays false).
void finalize(uint8_t* /*state_*/) {}

// Same shape as CollectFunction::bindFunc: pin the definition's parameter to
// the concrete argument type and return the argument type unchanged, so
// JSON->JSON, STRING->STRING, INT64->INT64, DOUBLE->DOUBLE, ...
std::unique_ptr<FunctionBindData> bindAggregate(const ScalarBindFuncInput& input) {
    DASSERT(input.arguments.size() == 1);
    auto* aggFuncDefinition = reinterpret_cast<AggregateFunction*>(input.definition);
    aggFuncDefinition->parameterTypeIDs[0] = input.arguments[0]->dataType.getLogicalTypeID();
    auto returnType = input.arguments[0]->dataType.copy();
    return std::make_unique<FunctionBindData>(std::move(returnType));
}

template<bool IS_MAX>
function_set buildMinMaxFunctionSet(const std::string& funcName) {
    function_set result;
    // Matching is exact on isDistinct (built_in_function_utils.cpp), so both
    // overloads are mandatory.
    for (auto isDistinct : std::vector<bool>{true, false}) {
        result.push_back(std::make_unique<AggregateFunction>(funcName,
            std::vector<LogicalTypeID>{LogicalTypeID::ANY}, LogicalTypeID::ANY /* bindFunc wins */,
            initialize, updateAll<IS_MAX>, updatePos<IS_MAX>, combine<IS_MAX>, finalize,
            isDistinct, bindAggregate, nullptr /* paramRewriteFunc */));
    }
    return result;
}

} // namespace

function_set GqlMaxFunction::getFunctionSet() {
    return buildMinMaxFunctionSet<true>(name);
}

function_set GqlMinFunction::getFunctionSet() {
    return buildMinMaxFunctionSet<false>(name);
}

} // namespace gql_extension
} // namespace lbug
