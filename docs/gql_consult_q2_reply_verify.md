# 回帖验证报告 — docs/gql_consult_q2_reply.md（v2）

> 验证日期：2026-10-02。方法：探机复跑（e2e `verifyreplyprobe.test`，ANY 图哨兵集
> {9, 30, 33, 33.5, 100, "x", 缺失}）+ 源码对码 + TCK 语料对账。结论先给：**回帖主体可靠，
> 探机表全部复现，推荐序（先 B1 序算子、后 B2 等值、C 并行）成立；有 2 处实质修正 + 2 处
> 归因校准，其中「裸文本存储」一处若不修会直接废掉 sortkey 桥的 yyjson 方案。
> （Phase 10 已按此修正落地：B1 `c894d91`、B2 `e7c51a9`，自测 112/112、TCK 172/23/11。）**

---

## 1. 探机复跑（§0 表格逐行对照）

| 回帖声称 | 我方复跑实测 | 判定 |
|---|---|---|
| `> 30` 返回 9、33、"x"，漏 100 | `a|9, c|33, e|x, f|33.5`（漏 100、30） | ✅ 吻合 |
| `< 30` 返回 100，漏 9 | `d|100` | ✅ 吻合 |
| `= 33` 命中 | `c|33` | ✅ 吻合 |
| `= 33.0` 0 行 | `{}`（`EQUALS(p.age, CAST(33.000000, JSON))`） | ✅ 吻合 |
| ORDER BY 文本序 | `100, 30, 33, 33.5, 9, x, (null)` | ✅ 吻合 |
| `+1` 响亮拒 | `Could not convert "x" to INT64.` | ✅ 吻合 |
| `CAST(AS DOUBLE)` 逃生舱 75→75.0、1.5→1.5 | `h|75.0, i|1.5`；含 "x" 响亮拒 | ✅ 吻合 |
| `= null` 不判真（④ 的条件假设） | `EQUALS(p.age,)` → 0 行 | ✅ 吻合 |
| `NOT(p.age=null)` 仍是 UNKNOWN（④） | 0 行（引擎 NOT 三值逻辑实证） | ✅ 吻合 |
| 整数袋 `_gql_sum + 1` 值正确（Q2-B） | `142+1 = 143`（回帖的 76 同机制） | ✅ 吻合 |
| 实数袋同式「两侧同抛」 | `Could not convert "76.5" to INT64.` 响亮 | ✅ 吻合 |

结论：顾问的探机是真实独立实跑过的，不是编的。

## 2. 源码对码结果

**✅ 成立：**

- `built_in_function_utils.cpp:123,168` 行号属实：任意类型→STRING/JSON 有隐式 cast 代价，
  JSON→STRING/JSON 代价 0。但见修正 1。
- 比较的物理执行是文本序：`vector_comparison_functions.h:123-126`（JSON 的 physical exec
  走 `string_t` 比较）。
- 算术「响亮或对」机制属实（JSON→INT64 转换失败抛 ConversionException，无截断）。
- 引擎 WHERE 只放行 TRUE、UNKNOWN 被过滤、`NOT(UNKNOWN)=UNKNOWN`——三值逻辑 ✅（回帖 ④）。
- `ExtensionUtils::addScalarFunc` 存在（`src/include/extension/extension.h:156`），
  ANY 通配标量函数有 `_gql_to_json` 先例 ✅。
- GQL 语法：比较是 `ComparisonExprAltContext`（`GQLParser.h:7561`）=
  `valueExpression compOp valueExpression`——左/右子树天然切好，各有 source interval，
  回帖 ① 的「span 切分消解操作数切分」成立 ✅。
- SET 的 `=` 在 `SetItemContext → SetPropertyItemContext`（`GQLParser.h:2104`）,
  不在比较上下文——天然免疫 ✅。
- `=>` 是独立 token（`GQLParser.cpp:285` 字面量表），不会被比较改写误伤 ✅。
- TCK 跨类型全序确实被钉：Aggregation2 [11] `max([1,'a',null,[1,2],0.2,'b']) = 1`、
  [12] `min(...) = [1,2]` → **array < string < number** ✅。回帖「'x' < 30 应为 true」
  与此一致；验收表 `WHERE p.age < 30 → {9, "x"}` 内部自洽 ✅。
- boolean TCK 只钉三值真值表（openCypher 派生语料），不钉谓词位跨类型比较 ✅。
- `json_utils.cpp:658` 引擎 JSON 显示层用 vendored yyjson writer——Q2-E 第 2 条
  「formatJsonReal 换 yyjson writer 同源」的前提属实 ✅。
- int128 精度（>2^53 经 double 丢精度）、-0.0 规范化两点技术判断正确 ✅。
- openCypher front-end（Scala，Apache-2.0）Rewriter、FoundationDB tuple layer 编码规范——
  引用真实存在、公开、无抄写障碍 ✅（且回帖明说「概念参考，不搬代码」）。

**❌ 修正 1（最重要）：机制钉错了——比较落的是 (JSON,JSON) 原生重载，不是 (STRING,STRING)；且
回帖没看到「裸文本存储」这个决定性问题。**

计划输出实证：`FILTER[GREATER_THAN(p.age, CAST(30, JSON))]`——binder 把 INT64 字面量
cast 成 JSON 去匹配**为每个 LogicalTypeID 注册的原生比较重载**（`ComparisonFunction::getFunctionSet`
对含 JSON 在内的所有类型对注册；JSON 的 exec 是 `string_t` 裸文本比较）。没有任何一边 cast 成 STRING。
可观察语义仍是文本序（回帖结论方向对），但「落进 (STRING,STRING) 重载」的定位是错的。

更关键：**ANY 图字符串属性存的是裸文本 `x`，不是 JSON 引号形态 `"x"`。** 实证：
- `p.age = _gql_to_json('x')`（文本 `"x"` 带引号）→ **0 行**；
- `p.age = 'x'`（裸文本）→ 命中 e；
- `x > 30` 为真——只有裸 `x`（0x78 > 0x33）才成立；引号形态 `"x"`（0x22 < 0x33）字节级为假。
  回帖自己写的字节逻辑 `'"x"'>'30' 为真` 在引号形态下是错的——它碰巧得出对的实测结果，
  正因为它不知道存储是裸的。

机制根源：`vector_cast_functions.cpp:942-945`——CAST(X→JSON) 绑定到 STRING cast（纯文本透传，
不引号化）。所以 `_gql_to_json('x')` 产出 `"x"`，而属性写入产出 `x`，两种形态**同列共存**。

**对回帖方案的影响**：② 的「JSON 解析用 vendored yyjson」会直接在字符串属性上抛错——
`x` 不是合法 JSON。桥/sortkey 的解析步必须先做形态判别（首字符 `"` → yyjson 解析；否则按
裸字符串归类），或者按「裸文本=字符串值」的规则兜底。不修这一条，切片 B1/C 一上线，
所有字符串属性比较/排序全部响亮拒（等于把今天绿的 TCK 字符串场景打红）。

**⚠️ 修正 2（校准后）：Q2-E 第 1 条的前提属实、文件归因错——「yyjson 只对 JSON 字段做」发生在
harness，不在 run_tck.py。**

grep 实证：`run_tck.py` 无任何 `yyjson`/`json.loads` 调用——它的裸标量处理是：整数原文照抄、
实数 `float(c)` 后 `%.6f`、列表单元格递归拆分。yyjson 归一在 **e2e harness** 里：
`test_runner.cpp:23 canonicalizeJsonValue`（只对 `{`/`[` 开头字段做，对象键排序+数组规范化），
裸标量只走 `canonicalizeBareNumber`（剥小数尾零，`5` vs `5.0` 仍严格区分）。
所以回帖的现状描述（"只有 JSON 字段过 yyjson、裸标量没走"）**成立**，但照它说的文件去找
（run_tck.py）会扑空；正确落点要么在 harness 的 bare-number 分支加 yyjson 往返，要么在
run_tck.py 生成侧加。方向（两侧按解析后数值比）仍合理。

**⚠️ 修正 3（自查后撤回，本条原判有误）：「149 boolean 场景」是**对的**，我最初判它错是我数错口径。**

run_tck.py 会把多行真值表场景**展开成逐行生成用例**：Boolean1-5 的 run 数 = 30+30+30+51+8
= **149**（README 合规表与此一致）。我验证时用 `grep Scenario:` 数的是**场景级** 36——层级不同，
不构成回帖/简报的错误。门禁表述「TCK 总数 172 不降」仍然是对的（也是本阶段实际使用的门禁）。

**❌ 修正 4：vendored TCK 语料里没有任何 ORDER BY 场景。**

grep 实证：整个 `features/` 无一处 `ORDER BY`。回帖「TCK 全序要求 array 可排序」「排序不比聚合，
没有拒的余地」作为**规范一致性论证**（ISO 39075 要求按全序排）成立，但「TCK 会钉」的框架不成立——
切片 C（sortkey）没有语料网兜，只有自测，正确性门槛必须直接对齐 ISO 文本，且其静默错风险
（编码错误=静默错序）比 B1/B2 更高，验收时要加人工对照表。

## 3. 无法在本机验证、但可接受的口径（外部声称）

- ISO 39075 空袋 SUM/AVG/MIN/MAX=NULL、COUNT=0 例外（Q2-C）——与引擎 native SUM 行为一致
  （`AggregateStateWithNull`），维持 NULL 无异议。
- 「Neo4j GQL 模式遵守 GQL」「多家实现 harness 转译先例」（Q2-D）——外部事实，低风险。
- 全序 rank 表 `null<bool<array<string<number<object`——**只有 array<string<number 段被语料
  钉死**（Aggregation2 [11][12]）；bool/object/null 的位置语料未钉。B1（序算子，只碰
  数值/字符串）用不到未钉段；做 sortkey 的 bool/object 分支前，先对 ISO 39075 原文钉一次。

## 4. 验证后对回帖的总体裁决

| 回帖条目 | 裁决 |
|---|---|
| §0 探机表 + 互证声明 | ✅ 全部复现，互证成立 |
| 根因定位（123/168 + 文本序） | ⚠️ 行号对、文本序对、重载定位错、漏裸文本存储 |
| Q2-A 推荐序（先 1，B1→B2，C 并行；方案 2/3 否决） | ✅ 成立，逻辑链完整（B1 保绿论证正确：绿集=文本序=值序场景，桥下不变） |
| ① 操作数切分（ANTLR 子树 span + 漏斗递归） | ✅ 可行，语法事实全部对 |
| ① 谓词签名 `(ANY,ANY)->BOOL` + addScalarFunc 注册 | ✅ 有先例有 API |
| ② 单 sortkey 优于多键（array 类内缺口论证） | ✅ 论证成立（多键在 array 类内确实全 NULL 打平）——但见修正 1/4 |
| ③ B2 前做「文本等 vs 语义等」分叉清单门禁 | ✅ 正确且必要（`33=33.0`、`'1.0'vs'1.00'`、转义形态） |
| ④ 三值逻辑 | ✅ 引擎实证一致，无额外处理 |
| Q2-B 不自动包装、文档化+逃生舱 | ✅ 实测支持（响亮或对、CAST 可用） |
| Q2-C 维持 NULL | ✅ 一致 |
| Q2-D values-only + 具名 allowlist + 上游 issue | ✅ 站得住 |
| Q2-E 两条 | ⚠️ 第 2 条前提真；第 1 条前提真但文件归因错（见修正 2），方向可留 |

**总体：可采纳。** 实现前把修正 1（裸文本判别）写进桥的解析步设计、修正 2/3 改写门禁表述、
修正 4 记入 C 切片验收计划即可。
