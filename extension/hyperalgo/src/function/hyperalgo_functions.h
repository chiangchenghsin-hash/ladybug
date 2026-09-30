#pragma once

#include "function/function.h"

namespace lbug {
namespace hyperalgo_extension {

// 分析层 CALL 函数(P-1.5 §3.2 端点表;hyperalgo-core 薄绑定,结果在 bind 期计算)
// 节点一律输出 ext id(FRZ-01 条款 5;P-1.5 通用约定「节点一律返回 ext_id」)

// CALL hyper_s_cc('g', s) RETURN node, component_id       (P0-1a 退化对照)
struct HyperSCCFunction {
    static constexpr const char* name = "HYPER_S_CC";
    static function::function_set getFunctionSet();
};

// CALL hyper_kitting_cc('g') RETURN node, block_id         (P0-1b 齐套连通;v1 存活=全活)
struct HyperKittingCCFunction {
    static constexpr const char* name = "HYPER_KITTING_CC";
    static function::function_set getFunctionSet();
};

// CALL hyper_hit_set('g') RETURN node                      (P1a 贪心击垮集,按选择序)
struct HyperHitSetFunction {
    static constexpr const char* name = "HYPER_HIT_SET";
    static function::function_set getFunctionSet();
};

// CALL hyper_ks_core('g', k, s) RETURN node, layer         (P1b 双参数剥离;layer 0 = 最外层)
struct HyperKSCoreFunction {
    static constexpr const char* name = "HYPER_KS_CORE";
    static function::function_set getFunctionSet();
};

// CALL hyper_b_cycles('g') RETURN cycle_id, edge          (P0-2 B-回路;edge = 超边业务 ext id)
struct HyperBCyclesFunction {
    static constexpr const char* name = "HYPER_B_CYCLES";
    static function::function_set getFunctionSet();
};

// CALL hyper_pr_walk('g', alpha, max_iter, tol) RETURN node, score   (P2 两步游走 PageRank)
struct HyperPRWalkFunction {
    static constexpr const char* name = "HYPER_PR_WALK";
    static function::function_set getFunctionSet();
};

// CALL hyper_fiedler('g') RETURN node, score               (P1c Fiedler 向量;候选评分在 REST 绑定层)
struct HyperFiedlerFunction {
    static constexpr const char* name = "HYPER_FIEDLER";
    static function::function_set getFunctionSet();
};

} // namespace hyperalgo_extension
} // namespace lbug
