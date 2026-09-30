#pragma once

#include "function/function.h"

namespace lbug {
namespace hyperalgo_extension {

// 判定层 v0(P3a-v0;FRZ-11 v1.1b 冻结决策:扩展内 CALL 形态):
// CALL hyper_kitting_bool('g', ['Agent'], ['FLOWS'], 'RoundState', round)
//   RETURN station, no_supplier, kitting, missing
// 语义 = FRZ-11 真值表(单站直接上游):上游空 → no_supplier=true,kitting=true;
//   缺 RoundState 行 → 该上游视作 inv=0(缺料);missing = 缺料上游业务 id 列表字符串 `[a,b]`。
// 站集合 = 有入边节点(S ∪ 超边头) ∪ 无入边且非任何超边成员的节点(孤立站,FRZ-01 条款 3)。
struct HyperKittingBoolFunction {
    static constexpr const char* name = "HYPER_KITTING_BOOL";

    static function::function_set getFunctionSet();
};

} // namespace hyperalgo_extension
} // namespace lbug
