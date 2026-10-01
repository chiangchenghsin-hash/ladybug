# Q2 专项回帖 — ANY 图 JSON 属性聚合/比较（第二轮）

> **对应简报**：`docs/gql_consult_q2.md`（2026-10-01 更新版）。**回帖 v2**：2026-10-01（按简报更新修订）。
> **互证状态**：本顾问独立探机（`lbug_shell.exe :memory:`，哨兵 9/30/33/100/"x"）与简报 §4 探机矩阵**结果一致**：比较/排序=静默错、算术=响亮拒、CAST 逃生舱可用。聚合半已验收（TCK 172/23/11）——Q1/Q2 聚合方案落地有效，恭喜。
> **⚠️ v2 修订点**：简报 §4 新约束（TCK untyped 全跑 ANY 图、boolean 149 场景靠"文本序=值序"巧合绿、粗粒度拒会砍到 ~120）**排除了 v1 推荐的「先 3 止血」**——Q2-A 推荐序重排为「直接上 1，序算子先行、等值跟进」，详见 Q2-A 节。Q2-B/C/D/E 结论不变（探机互证一致）。

---

## 0. 探机实证结果（本顾问独立实跑，与简报 §4 矩阵互证一致，可复跑）

数据集：age ∈ {9, 30, 33, 100, "x"}（哨兵 9/30/100 即你们设计的文本序歧义组）。

| 操作 | 实测结果 | 判定 |
|---|---|---|
| `WHERE p.age > 30` | 返回 **9、33、"x"**，漏 100 | **静默错**（文本序 `'9'>'30'`、`'"x"'>'30'` 为真，`'100'<'30'`） |
| `WHERE p.age < 30` | 返回 **100**，漏 9 | **静默错** |
| `WHERE p.age = 33` | 命中 | 巧合正确（文本 `'33'='33'`） |
| `WHERE p.age = 33.0` | **0 行** | **静默错**（GQL 数值等应真；文本 `'33'≠'33.0'`） |
| `ORDER BY p.age` | 100 → 9 → "x" | **静默错**（文本序） |
| `p.age + 1`（含 "x" 或 1.5 的行在场） | `Conversion exception: Cast failed ... to INT64` | **响亮拒**（可接受缺口） |
| `CAST(p.age AS DOUBLE)` | 1.5→1.5、75→75.0 | **逃生舱可用**（Q2-B 兜底实证） |

**根因（读码钉死）**：`built_in_function_utils.cpp:123,168`——任何类型→STRING/JSON、JSON→STRING 的隐式 cast 都存在，比较匹配落进 `(STRING,STRING)` 重载，JSON 物理层即文本。算术则匹配数值重载并尝试 JSON→INT64 转换，失败即抛。

---

## Q2-A 比较位红线的修法

**推荐序（v2 修订）：方案 3 已随简报 §4 新约束排除——直接上 1，分两步走：B1 先接 `<,<=,>,>=` 四个序算子（今天必错面），TCK 回归验证后 B2 再接 `=,<>`；ORDER BY 走单一 `_gql_sortkey`（方案 4，与 B 可并行）。方案 2 维持否决。**

v1 曾推荐「先 3 止血」，其前提是"拒绝只影响出错场景"；简报 §4 证明前提不成立（TCK 149 个 boolean 场景活在文本序巧合上，检测面无法静态区分"碰巧对"与"真错"），故止血切片删除，红线修复只能靠语义正确的桥本身。

### 最小落地切片

```
切片 B1（~2-3 天）：序算子谓词桥 _gql_lt/le/gt/ge + TCK 全量回归（149 boolean 场景不得翻红）
切片 B2（~1 天）：等值 _gql_eq/ne + 等值断言清单核查（见 ③）
切片 C（~1-2 天，可与 B 并行）：ORDER BY _gql_sortkey 单键编码
```

B1/B2 切分的理由见 ③：序算子是"必错面"且语义验收干净（哨兵值一测便知）；等值"碰巧对面"大，需要先做存量断言核查再放行。

**方案 2 维持否决的依据**（v1 已述，保留备查）：TCK 全序下 `'x' < 30` 应为 true，`_gql_num` 抽取返回 NULL → WHERE 丢行=静默偏离 ISO；且 TCK 不钉谓词位跨类型比较（`features/expressions/boolean/*` 只钉三值逻辑真值表，跨类型序仅聚合 [11][12] 钉）——偏离没有 TCK 网兜，违反"0 静默错"自我约束。

### ① 操作数切分：树定位 + span 切分 + 漏斗递归（业界无可直抄变换，方法论可抄）

**没有现成的"比较→函数"AST 重写可整段搬**——这个变换太特化。可抄的是方法论：

- **openCypher front-end**（Scala，Apache-2.0）的 Rewriter 模式（自底向上条件重写）——概念参考，不搬代码；
- **真正的操作数切分解法**：ANTLR 比较上下文本身已是切分好的树——`left / operator / right` 三个子节点各有 source interval（`ParserRuleContext.getSourceInterval()`，配合 `TokenStream.getText(interval)` 取原文）。操作数切分问题**消解为读子树文本区间**，括号/优先级/布尔分段全由 parser 解决，不需要任何文本级括号匹配。
- **落地形态**：遍历命中比较上下文且当前图 ANY → 取左/右子树 source span 文本 → 各自由现有表达式漏斗（`replaceExprs`/`finishExpr`，管标识符映射与函数别名）递归处理 → 拼成 `_gql_gt(<左>, <右>)`。即"结构化层定位 + 文本漏斗填内容"，不是全结构化重写、也不是纯文本正则——与你们混合架构同构。
- **边角清点**（SET/CASE/`=>`）：枚举 GQL.g4 所有含比较运算符token 的产生式，逐个标注"改/不改"：SET 赋值的 `=` 在 setItem 上下文（不在比较上下文，天然免疫——你们已确认 `=` 只过 rhs valueExpression）；CASE 的 WHEN 比较在比较上下文内（要改，语义同 WHERE）；`=>` 若有则是独立 token 不受影响。清单化后逐条加回归钉子。
- 谓词函数签名维持 v1 设计：`_gql_eq/ne/lt/le/gt/ge(ANY, ANY) -> BOOL`，操作数不包装（exec 层按实参 LogicalType 自取：JSON 直接用、非 JSON 视为单值 JSON）——翻译层只换算子与切 span，不碰操作数内容。注册走 `ExtensionUtils::addScalarFunc`（ANY 通配有 `to_json` 先例）。
- **null 语义**：`compareJsonValues` 改返回 `std::optional<int>`，任一侧 null → 谓词 NULL（三值逻辑，见 ④）。

### ② ORDER BY：单一 `_gql_sortkey` 字符串编码 **比多键稳**，三个边界必须处理

多键方案（`_gql_rank, _gql_numkey, _gql_strkey`）的"非本类 NULL 打平"语义本身成立（rank 已分序时跨类不打到第二键；类内同型时对应键非 NULL）——**真正的缺口在数组/对象类内**：rank 相同（皆 array）时三个键全 NULL 打平，要么丢序（**静默错**）要么响亮拒（**违反全序完备**——排序不比聚合，没有"拒"的余地，TCK 全序要求 array 可排序）。单 sortkey 字符串编码无此缺口（array 递归编码、object 可拒在编码时响亮抛或按键排序编码）。

单 sortkey 的三个边界（简报问到的 -0/NaN/超大整数）：

| 边界 | 处理 |
|---|---|
| **-0.0 vs +0.0** | 编码前规范化 `-0.0 → +0.0`（IEEE total-order 变换会把两者排开，与"数值等"语义矛盾） |
| **NaN/±Inf** | JSON 文本无 NaN/Inf 字面（非法 JSON）——源数据天然免疫；`_gql_avg/sum` 的非有限结果你们已响亮抛，不进排序 |
| **int128 精度** | ⚠️ IEEE754 double 变换对 >2^53 整数**丢精度**（两个不同 int128 编码相同=静默错序）——整数走独立的 128 位保序编码（符号翻转后大端定长字节序），与 double 编码之间用"数值类内子 rank"区分整数/实数子段；跨子段数值序（int 5 < real 5.5）由子段边界值对齐保证，或统一走十进制字符串保序编码（实现更简、速度够） |
| 实现建议 | **优先十进制保序编码**（指数+尾数定宽文本，思想同 FoundationDB tuple 的数值编码），绕开 IEEE 变换的全部边角；JSON 解析用 vendored yyjson，编码输出纯 ASCII，引擎 STRING 序即全序 |

抄写来源：FoundationDB tuple layer 编码规范（类型 rank + 保序负载的公开设计）；CouchDB collation 仅参考思路、**rank 表必须换 TCK 钉法**（`null<bool<array<string<number<object`）。均思路抄写，无许可证问题。

### ③ 等值收窄：值得，但接入前先做"碰巧对依赖"核查

- **序算子（B1）先行无风险**：桥的全序就是 TCK 钉法序——今天碰巧对的场景（单值/同型比较，文本序=值序）在桥下结果不变，149 场景保持绿；必错面（哨兵分叉）被修正。验收一测便知。
- **等值（B2）的真错面**：跨表示数值等（`33` vs `33.0`，探机已证）；**碰巧对面**：同形字面量等值（`EQUALS(prop, CAST(33,JSON))` 文本等）。
- **B2 特有风险**：桥的等值是**解析后语义等**，碰巧对是**文本等**——两者在规范化边界分叉（`'1.0'` vs `'1.00'` 存储文本不同、数值相等：桥 true、文本 false；字符串转义形式差异同理）。若 TCK/自测里有场景**依赖文本不等的结果**（碰巧错在绿），B2 接入会翻红——接入前 grep 一遍 boolean/谓词场景的等值断言，列出"文本等 vs 语义等"分叉清单，逐条定性（该翻的翻、该记的记），再放行。
- 结论：**先 B1 后 B2 的收窄是对的**，且 B2 的门禁是那份分叉清单，不是时间。

### ④ 三值逻辑：确认一致

GQL/SQL 三值逻辑：比较谓词任一侧 NULL → UNKNOWN；WHERE/HAVING 只放行 TRUE（UNKNOWN 与 FALSE 同被过滤）。你们的实现方向（谓词返回 NULL → WHERE 丢行）**与规范一致**。两个细节：① `=` 与 null 的组合别与 `IS NULL` 谓词混（`p.age = null` 是 UNKNOWN 不是 true——若今天文本等碰巧把 `null = null` 判真，那是另一个静默错，桥接入时一并修）；② `NOT (UNKNOWN)` 仍是 UNKNOWN——谓词桥返回 NULL 后经 NOT/AND/OR 的组合走引擎原生三值逻辑即可，无需额外处理。

### 双跑验收形态

哨兵集 `{9, 30, 33, 33.5, 100, "x", null}`：

| GQL | 等价 Cypher | 期望 |
|---|---|---|
| `MATCH (p) WHERE p.age > 30 RETURN p.id` | `MATCH (p) WHERE _gql_gt(p.age, 30) RETURN p.id` | {33, 33.5, 100} |
| `MATCH (p) WHERE p.age < 30 RETURN p.id` | `... _gql_lt(p.age, 30) ...` | {9, "x"}（TCK 全序 string<number） |
| `MATCH (p) WHERE p.age = 33.0 RETURN p.id` | `... _gql_eq(p.age, 33.0) ...` | {33}（B2 后） |
| `MATCH (p) RETURN p.id ORDER BY p.age` | `... ORDER BY _gql_sortkey(p.age)` | 9,30,33,33.5,100,"x" |
| 回归门禁 | TCK expressions/boolean 149 场景 | B1/B2 后全绿不翻 |

### 响亮性

谓词桥对全序已定义的全部类型对总可比较（全序是 total 的），无新拒绝路径；sortkey 编码遇 object（若选择不支持）在编码时响亮抛；B2 门禁清单外的存量断言翻红即停线核查，不靠静默放行。双跑两侧同函数天然一致。

---

## Q2-B JSON 返回值的算术组合性

**探机结论：红线不触发，文档化 + 逃生舱即可，不做自动包装。**

- 实测：JSON 算术撞数值重载的 JSON→INT64 转换——非数值/实数**响亮抛**；纯整数 JSON（含 `_gql_sum` 的 `75` 文本）`+1` 得 76，**值正确**。即算术是"响亮或对"，无静默错面。
- `CAST(p.age AS DOUBLE)` 逃生舱已实证可用（1.5/75.0 正确）。
- **不建议**算术位自动插 `_gql_num`/CAST：GQL 里聚合结果进算术的频率低，而自动包装要新开的定界面（算术上下文识别）与 Q2-A 切片 B 同构却收益零（无红线），纯负资产。
- 轻量改进项：把 `_gql_num(json)` 作为**显式用户函数**提供（手写逃生舱，与 CAST 并列），成本是切片 B 谓词桥的零头——可做可不做，列入 README 差异条目即可。
- 双跑：`RETURN _gql_sum(p.age) + 1`（整数袋）↔ Cypher 同式，期望 76；实数袋的拒绝对照"两侧同抛"。

## Q2-C 空聚合：NULL 还是 0

**维持 NULL（现状正确），依据：**

- ISO SQL/Foundation 聚合通则：空袋标量聚合（SUM/AVG/MIN/MAX）返回 **NULL**，**COUNT=0 是唯一例外**；ISO 39075 沿用 SQL 值表达式语义（GQL 的聚合语义章引用同一模型）。
- Cypher `sum([])=0` 是 openCypher 方言特例，不是规范方向；Neo4j GQL 模式遵守 GQL（NULL）。
- **双跑口径无冲突**：你们的双跑是 GQL≡等价 Cypher 对着 native 拍，`_gql_sum` 空袋 NULL 与 native SUM NULL 两边一致——对齐的是引擎不是 openCypher 文档。若未来 TCK 出现钉 0 的场景再议（届时 `_gql_sum` 加 `COALESCE` 即可，是翻译层一行事），Aggregation3 [1] 不钉此项。

## Q2-D 语料列名漂移：values-only + 脚注 **站得住，且优于 skip**

- 该场景**值自洽**（75 正确）、只有表头漂移——values-only 仍在验证聚合语义本身，覆盖强度远大于 skip。skip 应保留给"值本身矛盾"的语料。
- 三个加固条件（满足即可对外辩护）：① allowlist **逐场景具名** + 原因 + 上游 issue 链接（不允许裸条目）；② **必须同时上游提 issue**（这是口径合法性的来源——语义分歧留给权威方裁决）；③ REPORT methodology 脚注披露校验粒度差异。
- 业界惯例对照：跳过清单（skip-with-reason）最常见，但多家实现对 setup 漂移做 harness 转译/断言降粒度，先例存在；你们的口径=降粒度+透明披露，属其中较严的一档。

## Q2-E 实数文本拼写

**harness 解析归一为主，顺手根治为辅：**

1. run_tck.py 把**裸标量 JSON 也走 yyjson 解析归一**（现只对 JSON 字段做）——两侧都按"解析后数值"比，拼写分叉在断言层消失，改动最小；
2. 顺手根治（可选、数行）：`formatJsonReal` 改用 vendored yyjson 的 writer 输出——拼写与引擎 JSON 显示层（`json_utils.cpp`）同源，从源头消除分叉。yyjson 已链接，成本极低，推荐做。
- 不值得为此动产品侧任何其他东西；>1e16/<1e-4 的指数拼写差在 TCK 语料里实际不出现，归一后即可文档化关闭。

---

## 附：修改优先级汇总

| 序 | 项 | 动作 | 红线状态 |
|---|---|---|---|
| 1 | Q2-A 切片 B1 | 序算子谓词桥 `_gql_lt/le/gt/ge`（ANTLR 比较上下文 span 切分 + 漏斗递归）+ TCK 149 场景回归门禁 | 修复（必错面） |
| 2 | Q2-A 切片 B2 | 等值 `_gql_eq/ne`，门禁=文本等 vs 语义等分叉清单核查 | 修复（跨表示数值等） |
| 3 | Q2-A 切片 C | `_gql_sortkey` 单键十进制保序编码（-0 规范化 / int128 精度 / array 递归），与 B 可并行 | 修复 |
| 4 | Q2-E | harness 裸标量 yyjson 归一（+可选 yyjson writer） | 质量 |
| 5 | Q2-B | 文档化 + CAST 逃生舱（已实证）；可选 `_gql_num` 显式函数 | 无红线 |
| 6 | Q2-C | 维持 NULL，关闭 | 无动作 |
| 7 | Q2-D | values-only + 具名 allowlist + 上游 issue | 无动作 |
