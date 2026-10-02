# Q5 回帖验证 — 可行性裁定 + 一处证伪修正（SIMPLE 映射）

> 2026-10-02。对象：`docs/gql_consult_q5_reply.md`。方法与 Q3/Q4 轮同款：探机复现 + 源码/语料交叉，
> 先纠错再采纳。**结论：可行，落地 3 项（Q5-2/3/4）+ 挂账 1 项（Q5-1），但 Q5-2① 的核心映射被探机证伪，
> 必须按修正切片做，否则会引入静默错答（红线事故）。**

## 一、逐项裁定

| 项 | 回帖裁决 | 验证结果 | 落地口径 |
|---|---|---|---|
| Q5-1 多跳 QPPI | 缓做；无界永拒；有界 UNION 子集备好 | ✅ 成立（B 路线死路自洽；SurrealDB gql 模块 **BSL 1.1 不可抄**属实） | 挂账零工时；备忘入 README/handover |
| Q5-2① SIMPLE | **裸引擎 `*ACYCLIC` 零包装** | ❌ **证伪**（探机，见二）：引擎 `*ACYCLIC` 比 ISO SIMPLE **松**，照抄=静默错答 | 修正切片：`*ACYCLIC` 预过滤 + `_gql_is_simple` 谓词（见三） |
| Q5-2② DIFFERENT EDGES | 单模式=TRAIL 机器；多模式拒；REPEATABLE=no-op | ✅ 成立；REPEATABLE **今天已是 no-op**（探机 QUERY PASSED），零工时 | 单模式复用 Phase 7 TRAIL/IS_TRAIL 机器；多模式响亮拒；REPEATABLE 补钉子 |
| Q5-2③ IS LABELED / `%` | 接 Phase 7 标签谓词路；`%`=表图恒真/ANY 非空 | ✅ 成立（出口/分流均为现成机器） | 按回帖做；`IS NOT LABELED`=整体取反（勿混入 `!`） |
| Q5-3 bind 期守卫 | `_gql_list_checked` bindFunc 查推导型 | ✅ 成立（源码证实，见二） | 按回帖做；typed 限定、FOR 源不动、做完永久关账 |
| Q5-4 相对限定名 | 做 A（根解析=精确） | ✅ 成立；**前提已满足**（SESSION SET SCHEMA 今天已响亮拒，探机证实） | 按回帖做 + 改遗留措辞 |

## 二、探机/读码证据（勿重复验证）

1. **SIMPLE 证伪探机（决定性）**：ANY 图建 `n0→n1→n2→n0`（三角）与 `m0→m1→m0→m2`（端点=中间点），
   `MATCH p=(x)-[*ACYCLIC 3]->(y)` 实际返回 **6 行**，含 `m0|m2`（m0 重现在中间位）与
   `m0|m1`/`m1|m0`（边序列 e4,e5,e4/e5,e4,e5 均放行）。
   ⇒ 引擎 `*ACYCLIC` 只保**中间点两两互异**，端点与中间点可撞；
   ISO SIMPLE（四源同款定义：节点不重复、仅允许首尾重合）**禁止**这两类行走。
   ⇒ 回帖「SIMPLE=裸 `*ACYCLIC`」照抄会把非 SIMPLE 行走当 SIMPLE 放出=**静默错答**。
   回帖的另半句**正确**：节点互异（除首尾）蕴含边互异，SIMPLE 无需 TRAIL 包装；开/闭路径同一条规则。
2. **bindFunc 先于 coerce（Q5-3 架构）**：`bind_function_expression.cpp:60-115`——children 按
   `bindExpression` 绑出（带 `getDataType()`），`ScalarBindFuncInput{children,...}`（function.h:46）
   在 **`implicitCastIfNecessary` 之前**调用 bindFunc；同构化发生在 list 构造（list_creation）更下游。
   ⇒ 每实参静态型在守卫可见、未抹除。`to_json`/`_gql_to_json` 的 bindFunc 是现成先例。
3. **变长标量注册可行**：`ScalarFunction::isVarLength`（scalar_function.h:31；先例
   vector_string_functions.cpp:144），`matchVarLengthParameters`（built_in_function_utils.cpp:530+）
   对单参数型函数匹配任意实参个数。⇒ `_gql_list_checked(ANY...)` 有 API 面。
4. **路径谓词扩展函数可行**：`function/path/path_function_executor.h` 在 `src/include` 公共面，
   `UnaryPathExecutor::executeNodeIDs`/`selectNodeIDs` 可从扩展侧引用；
   IS_TRAIL/IS_ACYCLIC 即此机器（semantic_function.cpp，参数型 `RECURSIVE_REL`）。
   ⇒ `_gql_is_simple`（节点互异、仅首尾可重合）可同构实现。
5. **REPEATABLE ELEMENTS 今天已接受**：`CALL GQL("MATCH REPEATABLE ELEMENTS (n) RETURN count(n)")`
   探机 QUERY PASSED（no-op，Phase 3「REPEATABLE 丢弃」口径）——回帖该点为零工时确认。
6. **Q5-4 前提已满足**：`SESSION SET SCHEMA` 今天即 `GQL feature not supported: SESSION SET SCHEMA`
   （gql_transformer.cpp:607-608 + 探机）；`dir.name` 今天拒于 rewriteGraphExpression:281-283，
   措辞 `qualified graph name (schemas are not mapped)` 确为遗留（schemas 已映射），需改。
   `IS [NOT] LABELED` 拒绝消息 `label predicate (IS [NOT] LABELED ...)`（:3238）已就位。
7. 语料压力 0 复核（Q5 简报事实 5）维持；TCK 门禁不受本轮影响。

## 三、修正后落地切片（按此执行，勿照抄回帖 Q5-2①）

1. **Q5-2① SIMPLE（修正）**：`_gql_is_simple(ANY) -> BOOL` 扩展函数（RECURSIVE_REL 实参 →
   nodeIDs 序列 → 判「节点互异，仅允许 first=last 重合」，复用 UnaryPathExecutor 提取）；
   发射：单 var-length 槽 → `*ACYCLIC`（预过滤，严格更松故安全）+ 路径变量
   `WHERE _gql_is_simple(p)`；多跳沿 Phase 7 `_gql_pp{N}` wrap 同型。若扩展侧读 RECURSIVE_REL
   遇 API 墙 → **回退为 SIMPLE 维持响亮拒**（报告回主会话，勿硬闯）。
2. **Q5-2②③④**：按回帖（DIFFERENT EDGES 单模式走 TRAIL 机器/多模式拒/REPEATABLE 钉子；
   IS LABELED 接标签谓词出口；`%` 表图 TRUE、ANY `size(labels(v))>0`；IS NOT LABELED 整体取反）。
3. **Q5-3**：按回帖（`_gql_list_checked(ANY...) -> LIST` bindFunc 类不一致响亮抛；typed 图
   列表字面量含 ≥1 非字面量元素才改写；ANY 免检；FOR 源不动）。
4. **Q5-4**：按回帖（相对/点分名根前缀化后走现有 mangling 同一码路；改遗留措辞；
   SESSION SET SCHEMA 补钉子）。
5. **Q5-1**：零工时挂账；无界=模拟器固有上限入 README；SurrealDB BSL 1.1 不可抄入备忘。

## 四、修正后落地序（与回帖的差异：①不再排第一）

**A（Q5-2 全部，含修正的①）→ B（Q5-3 + Q5-4）**；两批顺序做（同触 gql_transformer.cpp，不并行）。
门禁：`_build_gql.bat` + 自测全绿（新增用例数以实际为准）+ TCK 190 不降 + wrong-GQLSTATUS=0。
完成条件：Q5-3 落地后 README #17 残余改写为「bind 期响亮拒 + 已知残余边界」；Q5-4 落地后
README #22「残余只剩 WAL」更新。
