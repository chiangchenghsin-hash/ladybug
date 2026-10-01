# Q2 专项咨询简报 — ANY 图 JSON 属性的聚合/比较（第二轮，含已落地实现）

> **读者**：未接触本仓库的外部 AI 顾问。**目的**：就第 5 节开放问题（尤其比较位红线的修法）征求可落地建议。
> **状态快照**：2026-10-01，Q2 聚合半已落地并验收（TCK **172 过 / 23 挂 / 11 跳**，自测 101 全绿）；比较/排序半为红线悬案（探机已证实静默错，修法分叉见 §5 Q2-A）。前情提要见 `docs/gql_consult_brief.md`（Q1 已落地：TCK 165→171）。
> **回帖期望**：建议落到「翻译层内、不动引擎语义」的粒度；凡声称可行，请给出可双跑验证的形态。

---

## 1. 背景（1 分钟，与前简报同口径）

LadybugDB 是 Cypher 方言图数据库；`extension/gql` 是 **ISO GQL 兼容桥 / 翻译模拟器**：
`CALL GQL("...")` 把 GQL 翻译成等价 Cypher 在引擎执行，**双跑对照一致**为验收口径。
红线 **0 静默错答案**：未映射构造响亮拒，宁拒不给错。硬约束：不改 GQL.g4/Cypher.g4/引擎语义；
抄写优先于自研（Apache-2.0 署名）；够用即可；不可声称 ISO 合规认证。

**Q2 是什么**：ANY 图（`CREATE GRAPH g ANY`，开放图）的动态属性列是 **JSON 类型**（JSON 文本存储）。
`MATCH (p) RETURN p.name, sum(p.age)` 被 binder 拒：`Function SUM did not receive correct arguments: Actual: (JSON)`。
GQL 语义是跳过 null 求和（TCK Aggregation3 [1] 期望 75）。`avg(p.age)`、比较、排序在同一列上同理存疑。

---

## 2. 关联事实（均经实证/读码，勿重复调研）

| # | 事实 | 依据 |
|---|---|---|
| 1 | **ANY 图属性列=JSON**；表图属性=强类型列 | 报错实证 `Actual: (JSON)` |
| 2 | **JSON 值显示=存储原文**；toJSON：int→`5`、double→`5.0`（yyjson 补 `.0`）、串→`"a"`；DOUBLE 列显示 `%.6f`（`75.000000`） | `value.cpp`、`json_utils.cpp` |
| 3 | harness 裸数值归一（Phase 8 已加）：`75.000000`→`75.0`，但 **int `75` 与 real `75.0` 严格区分** | `test_runner.cpp` canonicalizeBareNumber |
| 4 | **native SUM 语义**：整数族→INT128/UINT128 累加（无溢出），浮点→DOUBLE；**AVG 恒 DOUBLE**；multiplicity 逐次加；**空组/全 NULL → NULL**（AggregateStateWithNull，不是 Cypher 的 0） | `sum.h:42-48`、`aggregate_function.cpp:32-59` |
| 5 | **DISTINCT 去重在 executor**（distinct hash table），聚合函数只需 isDistinct 双重载签名，state 不去重 | `simple_aggregate.cpp`、`aggregate_hash_table.cpp` |
| 6 | 扩展可注册聚合（`extension::addFunc` + `AGGREGATE_FUNCTION_ENTRY`），**聚合路径无隐式 cast**；参数精确匹配，ANY+bindFunc 可延迟钉型 | 前简报 §8（已用 Phase 8 验证） |
| 7 | **Phase 8 全序**：`null < bool < array < string < number < object`（TCK [11][12] 钉，与 CIP 数组/串/数字三类相反）；数字跨 INT/DOUBLE 按数值、胜者保型；`_gql_max(ANY)→输入类型`（JSON 进 JSON 出） | `gql_json_functions.cpp`、README #20 |
| 8 | CAST FROM JSON 走 string-cast 路径存在；JSON=JSON 等值/ORDER BY「今天就工作」——但**实证面是单位数数值**（labels.test `v:1,2,3`），文本序=数值序的盲区 | labels.test |
| 9 | TCK Aggregation3 [1] **语料期望列名与自己的查询不一致**：查询 `RETURN p.name, sum(p.age)`，期望表头 `n.name|sum(n.num)`（值 `75` 对）——无实现能在校验列名的同时过此场景 | Aggregation3.feature:33-49 |
| 10 | 引擎表达式对 JSON 算术（`p.age + 1`）/JSON vs 字面量比较的行为**待探机**（进行中）；若为文本拼接/文本序=静默错，触红线 | 探机矩阵 P2-P6、P11-P15 |

---

## 3. 已落地并验收（TCK Aggregation3 [1] 翻绿；jsonagg.test 9 组双跑首跑全绿，含与 native SUM/AVG 直接对拍）

### 3.1 `_GQL_SUM` / `_GQL_AVG` 扩展聚合（ANY 参数）

| 输入 | SUM 返回 | AVG 返回 |
|---|---|---|
| 整数族 | INT128（无符号 UINT128） | DOUBLE |
| FLOAT/DOUBLE | DOUBLE | DOUBLE |
| **JSON** | **JSON（保型）** | **JSON（恒 real）** |
| 其它（STRING/LIST/…） | bind 期响亮拒 | 同 |

- **JSON 结果文本**：SUM 全整数→`75`（精确十进制，int128 直出）；含实数→`3.5`/`2.0`（yyjson 风格，整值 real 补 `.0`）；AVG 恒 `2.0`/`37.5`。
- 跳过 SQL NULL 与 JSON `null`；multiplicity 循环加（抄 SumFunction）；空组/全 NULL→**NULL**（与 native 一致）；JSON 非数字/非法文本/非有限结果→响亮抛。
- 理由：bindFunc 在 bind 期无法预知 JSON 里是 int 还是 real，**只有 JSON 返回能运行时保型**——TCK 期望 `75`（int 文本）与 `3.5`（real 文本）靠它同时成立。

### 3.2 翻译 splice

GQL `sum(`/`avg(`/`max(`/`min(` 在 call-position 逐词改写为 `_gql_sum(/_gql_avg(/_gql_max(/_gql_min(`（字面量/词边界/点号属性安全；结果列名保持 GQL 源文本 `` `sum(p.age)` ``）。
**typed 输入与 native SUM/AVG 同型同显示**，双跑对照是 `sum(n.x)` vs native `sum(n.x)`（真对齐证明）。

---

## 4. 红线实证：ANY 图 JSON 属性上的比较/排序/算术（探机已回，2026-10-01）

GQL 查询会写 `WHERE p.age > 30`、`ORDER ORDER BY p.age`、`WHERE p.age = 33`、`RETURN p.age + 1`。
JSON 列物理上是 JSON 文本。探机矩阵（哨兵值 9 / 100：文本序与数值序必分叉）实证结果：

| 探机 | 语义真值 | 引擎实际 | 判定 |
|---|---|---|---|
| `WHERE p.age >= 100` | c | **a,b,c,d**（`'33'≥'100'` 等文本真） | ✖ 静默错 |
| `WHERE p.age > 30` | a,c,d | 混入 **b(9)**、丢 **c(100)** | ✖ 静默错 |
| `ORDER BY p.age` | 9,10,33,33.5,100 | **10,100,33,33.5,9**（文本序） | ✖ 静默错 |
| `p.age = 33.0`（存 `33`） | True | **False**（JSON 文本不等） | ✖ 静默错 |
| 两节点 `a.age = b.age`（33 vs 33.0） | True | **False** | ✖ 静默错 |
| `p.age = 33`（同形字面量） | True | True（`EQUALS(prop, CAST(33, JSON))` 文本等恰巧对） | ✔ 碰巧对 |
| `p.age + 1`（age 33.5） | 34.5 | **响亮 ConversionException**（尝试 `"33.5"→INT64`） | ✔ 拒得响 |
| `p.age * 2`（age 33） | 66 | 66（按另一侧类型数值化） | ✔ |
| `CAST(p.age AS INT64)`（33.5） | — | **响亮拒**（不静默截断） | ✔ |
| `max/min/sum/avg(p.age)` | 100/9/251.5/37.1 | 全对（`_gql_*` 扩展聚合，本轮已落） | ✔ |

**新约束（改变方案权衡，重要）**：粗粒度「响亮拒比较」**不可行**——TCK 整套跑在 ANY 图上
（run_tck.py untyped 模式），expressions/boolean 场景今天全绿靠的正是「单值/字符串场景
文本序=值序」的巧合。一刀切拒会把 TCK 从 172 砍到 ~120。**修法必须语义正确（保 TCK 且保红线）**，
「先拒后建桥」的方案 3 路线被排除（除非缩小拒绝面到只拒不碰巧对的场景——静态不可判，schema-less）。
算术位不用管：按另一侧类型数值化、不匹配响亮抛，无静默错。
（勘误 2026-10-02：本段原写「149 场景」系笔误——语料实数 boolean/ 为 36、expressions/ 全目录 52，
见 `gql_consult_q2_reply_verify.md` 修正 3；门禁表述改用「TCK 172 不降」。）

---

## 5. 开放问题（重点）

### Q2-A 比较/排序位红线的修法（最大分叉；探机已证实文本序静默错）

**已排除**：① 改引擎比较算子/加 JSON 隐式 cast=动引擎表面；② **粗粒度响亮拒绝**——会回归 TCK ~50 绿场景（见 §4 新约束）。

**剩余候选**（都想听利弊与最小落地面）：

1. **全序比较桥**（倾向）：把比较运算改写为 `_gql_gt(a,b)` 等六谓词（ANY×ANY→BOOL，两侧走 Phase 8 `compareJsonValues` 全序，NULL 三值逻辑）；ORDER BY 键走多键 `ORDER BY _gql_rank(k), _gql_numkey(k), _gql_strkey(k)`（rank=全序类，非本类键 NULL 自然打平）或单一 `_gql_sortkey` 字符串编码。面：文本级算子改写的**操作数切分**（括号/优先级/布尔分段）是主要工程风险；我们表达式漏斗 `finishExpr` 是文本级（字面量感知），SET 赋值 `=` 不经过该文本（只过 rhs valueExpression）——但 CASE/`=>` 等边角要清点。
2. **`_gql_num(json)` 数值抽取包装**：面小，但非数值 JSON 的处理偏离 GQL 全序（`'x' < 30` 按全序是 true，抽取抛错或 NULL 都不对）——只适合「纯数值属性」假设的退化档。
3. ~~响亮拒绝~~ **已被 TCK 约束排除**（上文）。
4. **ORDER BY 专用排序键**（方案 1 的 ORDER BY 半边可独立先行：`_gql_rank`/`_gql_numkey`/`_gql_strkey` 多键，或 STRING 编码键；数组/对象键响亮拒）。

**想问**：① 方案 1 六算子改写的操作数切分，业界有没有现成的「比较→函数」AST 重写可抄（Neo4j front-end / other，Apache-2.0）？② ORDER BY 多键拆解（rank/numkey/strkey）vs 单一 sortkey 字符串编码，哪个更稳（NULL 打平语义、-0/NaN/超大整数边界）？③ `_gql_eq` 与引擎 JSON 文本等值并存期的迁移策略——先只接 `<,<=,>,>=` 四个序比较（今天必错），`=,<>` 文本等只在「跨表示数值等」（33 vs 33.0）时错、碰巧对面更大，是否值得先收窄改动面？④ 三值逻辑（任一侧 NULL → 谓词 NULL → WHERE 丢行）与 GQL/SQL 的一致性确认。

### Q2-B JSON 返回值的算术组合性

`_gql_sum(p.age)` 返回 JSON 后，`sum(p.age) + 1` 撞引擎 JSON 算术（可能响亮拒=可接受缺口，可能静默拼接=红线）。
**想问**：有没有比「文档记缺口+CAST 逃生舱（`CAST(sum(p.age) AS DOUBLE)`，CAST FROM JSON 已存在）」更好的轻量方案？
例如只在**算术操作数位**自动插 `_gql_num`/CAST？GQL 里聚合结果进算术的频率值不值得这层包装？

### Q2-C 空聚合的返回：NULL 还是 0？

我们按 **native SUM = NULL**（引擎 AggregateStateWithNull）对齐。openCypher 是 `sum([])=0`；ISO GQL/SQL 是 NULL？
TCK Aggregation3 [1] 不钉这个（组内有值）。**想问**：ISO GQL 对 SUM/AVG 空袋的规范文本在哪一条？Neo4j GQL 模式下返回什么？（若规范是 0，我们的 typed 双跑会跟 native Cypher 分叉——GQL≡Cypher 双跑口径怎么摆？）

### Q2-D TCK 语料列名漂移的处置（Q7 延伸，倾向已定，请复核）

Aggregation3 [1] 期望表头与自己的查询不一致（事实 #9）。我们**不改 vendored 语料**，在 `run_tck.py` 加显式 allowlist：命中场景 values-only 校验（不检查列名），计入 passed，REPORT methodology 脚注透明披露。
**想问**：业界对自相矛盾语料的惯例（转译 setup / values-only / skip / 上游 issue）里，values-only+脚注这个口径是否站得住？还是 skip 更稳？

### Q2-E 实数文本拼写（小项）

JSON real 结果用 `std::format` 最短往返（`3.5`、`2.0`），指数形可能与 yyjson 的 `1e-5`/`1e+21` 拼写不同（值同、均合法 JSON）。
harness 已有 JSON 字段 yyjson 归一但**裸标量 JSON 不走它**。值同拼写异的场景（>1e16 或 <1e-4）值得现在处理吗（harness 解析归一/固定 yyjson 打印），还是接受文档化？

---

## 6. 已决事项（勿重复讨论）

1. Q1 跨类型全序 min/max 已落地（`_gql_max`/`_gql_min`/`_gql_to_json`，TCK +6）；全序 `null<bool<array<string<number<object` 是 TCK 钉法（与 CIP 相反，TCK 是可执行规范）。
2. JSON 聚合**结果类型=JSON 保型**（方案 3.1）——为 TCK `75`/`3.5` 双形态成立，非口味问题。
3. typed 输入走 `_gql_sum` 但**与 native SUM 同型同显示**（双跑对着 native 拍）；无计划改引擎 SUM。
4. DISTINCT 去重在 executor，聚合 state 不做去重。
5. 图类型注册表 WAL 持久化=暂缓（勿重复评估）；`:A:B` 三方分叉已钉死（输入侧是 GQL 语法错）。

---

## 7. 代码锚点

| 路径 | 内容 |
|---|---|
| `extension/gql/src/function/gql_json_functions.cpp` | Phase 8 全序 + 本轮 `_GQL_SUM`/`_GQL_AVG`（GqlSumAvgState、bindSumAvg、formatJsonReal） |
| `extension/gql/src/gql_transformer.cpp` | `rewriteGqlAggCalls` call-position splice（max/min/sum/avg） |
| `src/include/function/aggregate/sum.h`、`avg.h` | native SUM/AVG 语义模板（multiplicity/空组/类型） |
| `extension/gql/test/tck/features/expressions/aggregation/Aggregation3.feature` | [1] 语料列名漂移案 |
| `extension/gql/test/tck/run_tck.py` | TCK 跑批 + values-only 例外（本轮新增） |
| `docs/gql_consult_brief.md` | 前轮简报（Q1-Q7 全景 + §8 引擎核验附录） |

## 8. 建议回帖格式

每条按：**落点**（翻译层哪个机制/文件）/ **是否动引擎** / **双跑验收形态** / **抄写来源** / **响亮性（失败路径仍 0 静默错？）**。
Q2-A 请给明确的推荐序（如「先 3 后 1」）与最小落地切片。
