# GQL→Cypher 翻译层评估（成果 / 缺点）

> 数据截至 2026-10-01（Phase 7 收工 + 本评估当日修正）。证据来源：`extension/gql/README.md`（分级兼容矩阵 +
> 19 条已知语义差异，下称 README #n）、`_HANDOVER_GQL.md`（分阶段交付记录）、
> `extension/gql/test/tck/REPORT.md`（TCK 逐场景明细）、`docs/gql_compat_plan.md`（原始目标）。
> 所有数字均可回溯到上述文件；不引入其外的统计。

## 1. 一句话定位

这是一个**兼容桥 / 翻译模拟器**：`CALL GQL("...")` 把 ISO GQL 语句翻译成等价 Cypher 文本、
在 LadybugDB 的 Cypher 方言引擎上执行（GQL 文本 → vendored opengql `GQL.g4` ANTLR 解析 →
`GqlToCypherTransformer` 显式逐语句映射 → Cypher 文本 → 引擎 standalone-call rewrite → 正常管线）。
它**不是**原生 ISO GQL 执行引擎：GQL 语义全部由翻译层在 Cypher 方言上模拟，引擎不理解 GQL。
项目规则（`_HANDOVER_GQL.md` 开篇、计划文档）：不改 `GQL.g4`/`Cypher.g4`/引擎语义，
**GQL→Cypher 翻译层本身就是本项目的贡献**。

验收口径（计划与交接文档一致）：**"GQL 语句翻译成等价 Cypher 执行、双跑对照结果一致"**。
**已达成（在已映射子集内）**：自测 **86/86** 双跑全绿；opengql/tck **165 过 / 30 挂 / 11 跳（206 场景）**，
30 个失败全部响亮。**未达成且不声称**：ISO GQL 全量合规——19 条语义差异与响亮拒绝面见第 3 节。

## 2. 成果

### ① 验收口径本身是成果

- **双跑对照方法学**：每条 GQL 用例旁挂等价 Cypher、断言同一期望结果
  （README "Tests"；`extension/gql/test/test_files/*.test` 9 个文件，`grep -c '^-CASE'` 合计 86）。
  这把"翻译对不对"变成可执行命题，而不是评审口径。
- **TCK 跑批方法学留痕**：`run_tck.py` 头部自述 "Methodology notes (kept honest on purpose)"——
  期望值统一转 `Value::toString` 形式；异常场景只断言"抛错"（GQLSTATUS 未实现，明说）；
  副作用仅在空图可观测时校验；无法校验的记 "unchecked" 而非放行（REPORT.md 同口径）。
- **方法学能自我纠错**：Phase 5 曾把 15 个 TCK 失败归因为"布尔运算不校验操作数"，
  Phase 6 复核推翻——实为 harness 正则 `.+` 不跨行导致多行错误被误记"未抛异常"，
  真实输入是 GQL map 字面量；README #19 留档更正。

### ② 功能覆盖

| 类别 | 映射 | 证据 |
|---|---|---|
| 查询 | MATCH / OPTIONAL MATCH 透传；`SELECT…FROM GRAPH`→`USE GRAPH`+`MATCH…RETURN`；ORDER BY/SKIP/LIMIT；RETURN 含 GROUP BY | README 必选特性表 14.4–14.12 |
| 聚合/过滤/迭代 | GROUP BY/HAVING（含隐式分组）→`WITH keys, aggs WHERE…RETURN`；FILTER→`WITH * WHERE`（#18）；FOR→UNWIND | README 16.15/14.6/14.8 |
| 写入 | INSERT→CREATE（结构性发射，字面量 `'INSERT ME'` 安全）；SET 含无序赋值快照（#2）；REMOVE 属性→`SET n.prop=NULL` 近似（△，#1）；DELETE / DETACH DELETE | README 13.2–13.5 |
| 目录/会话/事务 | CREATE/DROP GRAPH；`CREATE GRAPH TYPE`→`CREATE NODE/REL TABLE` DDL（合成主键 `_gql_id SERIAL PRIMARY KEY`，#11）；SESSION SET GRAPH→USE GRAPH（会话粘滞，#3）；START TRANSACTION/COMMIT/ROLLBACK | README 12.4–12.7、7、8 |
| 路径 | 量词 `*`→`[e*0..]`（GQL `*`=**0** 起，裸 Cypher `*`=1 起）、`+`/`{m,n}`/`?`；单跳 QPPI；ANY/ALL SHORTEST；**路径模式精确**——TRAIL 多跳 + 每个 ACYCLIC 模式绑路径变量加 `IS_TRAIL`/`IS_ACYCLIC` 全路径谓词（引擎 `*ACYCLIC` 单用只约束中间节点，闭合走 1→2→1 会漏入，翻译层补齐） | README G035/G005/G010 行、#7 |
| 标签表达式（G074） | `:A&B`/`:A\|B`/`:!A` 按**图型分流**：ANY 图→`list_contains(labels(v),'X')`（合取），表图→`labels(v)='X'`；`:` 拼写绝不复用于复合表达式——`:A:B` 在 ANY 图是 AND、表图是 OR（表并集），同拼写反语义 | README #13；`gql_transformer.cpp` 标签段 |
| 函数 | 别名表 `COLLECT_LIST`→`COLLECT`、`CHAR_LENGTH`→`SIZE`、`PATH_LENGTH`→`LENGTH`、`ELEMENT_ID`→`internal_id`、`\|\|`→`+` 等 | README "Function-name mapping" |
| 宽容归一化 | `FROM GRAPH x`、`CREATE GRAPH g TYPE t` 预解析归一；`AS COPY OF` 文法歧义重解释 | README #14；`gql_function.cpp` normalize* |

自测分布（`_HANDOVER_GQL.md`，与 .test 文件 CASE 计数逐一相符）：basic 11 / select 7 / groupby 4 /
write 7 / routing 4 / path 18 / labels 2 / schema 6 / unsupported 27 = **86/86**。

### ③ 诚实性工程

- **无静默透传**：visitor 整树漫游改为显式逐语句分派，未映射构造一律抛
  `GQL feature not supported: <构造>`（`gql_transformer.cpp` 中 127 处 `unsupported(` 命中）；
  GQL 解析失败也不会兜给 Cypher 解析器（仅 Ladybug 私有 DDL 按白名单透传，README "Notes"）。
- **Phase 6 静默错清零**：`scanValueShapes` 挂 Transform 入口，map 值与异构列表**字面量**
  静态拒绝（README #17）——此前 `[1,'a',…]` 会被引擎列表归一静默改写后让 max/min 给出错误结果。
  README 明言"the layer has **no silent wrong-answer class**"；残余（非字面量元素运行时才归一）
  主动记入 #17，不藏。
- **误归因翻案有文档纪律**：Phase 5→6 的"布尔不校验"翻案、Agg2 [6] 从"过"改"挂"（旧口径过是巧合）
  均留档 `_HANDOVER_GQL.md` 与 README #19。
- **评估过程消灭了一处矩阵虚标**：G115 `PROPERTY_EXISTS` 原记 ✓（"parsed as-is"），实测解析后
  原样透传到 Cypher，执行时死在 `function PROPERTY_EXISTS does not exist`（transformer / Cypher.g4 /
  binder 三处均 0 支持）。评估中当场修复：按 Neo4j `PropertyExistsToIsNotNull`（Apache-2.0）
  抄写为 `PROPERTY_EXISTS(v, prop)` → `(v.prop IS NOT NULL)`——与 #1 的 REMOVE≈SET NULL 同属
  固定 schema 模型；补双跑用例 `PropertyExistsParity`，自测 85→86 全绿，矩阵同步更正。

### ④ 上游复用与署名

抄写优先于自研：函数别名表抄 Neo4j `GQLAliasFunctionNameRewriter`、量词边界抄
`AddElementUniquenessPredicates`、图类型规范形抄 `GraphTypeCanonicalizer`、
`GraphTypeSpec` 拼写实证来自 opengql/tck——全部在 `extension/gql/THIRD_PARTY_NOTICES.md`
逐文件登记（Apache-2.0，含改写说明）；tck vendored 保留 NOTICE.md 与 Neo 署名头。

### ⑤ 引擎侧最小必要改动

只做了翻译层做不了的事，未动查询语义：
- `standalone_call_rewriter.cpp`：`rewriteQuery` 不清空导致多语句批互相污染的**真 bug 修复**
  （也是"CALL GQL 必须单独成句"的根因）；
- `Catalog::setFunctionFallback`：图 catalog 函数查找回退 main，否则 `USE GRAPH` 后
  `CALL GQL` 报 function 不存在（`catalog.h:246`、`database_manager.cpp:141/341`）；
- `ExtensionManager::setData/getData`：每库扩展状态槽，承载图类型注册表（`extension_manager.h:39-40`）。

### ⑥ 知识沉淀

README（分级矩阵 + 19 条差异 + 图型映射表）、`_HANDOVER_GQL.md`（逐阶段交付、引擎实证事实、
"勿重复调研"备忘）、`test/tck/REPORT.md`（逐场景分类 + methodology + unchecked 全列）、
`docs/gql_compat_plan.md`（目标与决策）——四份文档互引，数字互相咬合。

## 3. 缺点（按痛感/严重度排序）

### ① 结构性天花板（模拟器的固有上限）

- **值模型不匹配**：无 map 值（表达式中 `{}`/`{k:v}` 拒绝，#17）；无 TIME 类型（LOCAL/ZONED TIME
  全拒，#15）；异构列表字面量静态拒但**非字面量元素无法静态判别，运行时仍可能归一**（#17 残余）；
  GQL 跨类型全序（MIN/MAX/SUM over list）未实现——这是 TCK "other 3" 的根因。
- **固定 schema 不匹配**：REMOVE≈`SET NULL`，`PROPERTY_EXISTS`/`properties()` 行为与 ISO 不同（#1）；
  NOT NULL 接受后丢弃（#12）；每个节点表带合成主键 `_gql_id`（#11）。
- **程序模型**：`CALL GQL` 必须是批内唯一语句；事务包裹体（`START TRANSACTION <body> COMMIT`）
  拒绝（#4）；NEXT 语句组合不支持（#5）；生成的多条 Cypher 中**只有最后一条的结果对外可见**
  （`_HANDOVER_GQL.md` 关键事实）。
- **会话副作用**：`USE GRAPH` 会话粘滞，`CALL GQL` 返回后当前图不切回（#3）。
- **成本**：双重解析（GQL 文本→Cypher 文本→引擎再解析一遍），表达式改写是文本级
  `replaceExprs`（字面量感知、边界感知，但终究是文本替换，`gql_transformer.cpp:445`）；
  **无 GQL plan cache**——每次 `CALL GQL` 都完整走 parse+transform。

### ② 功能缺口清单（响亮拒绝面，按 TCK 影响排序）

失败分类总账（REPORT.md 尾行）：**rejected-by-layer 23 + parse-error 4 + other 3 = 30，全部响亮**。构成：

| 缺口 | 场景数（30 挂的构成） | 性质 |
|---|---|---|
| SCHEMA 命名空间（CREATE/DROP SCHEMA） | **13**（12 例 CREATE + 1 例 DROP，最大单块；按原因计，与 README "create+drop schemas" 特征行的 13 挂口径不同但同数） | 设计内拒绝 |
| 事务包裹体、qualified graph name | 1 + 1 | 设计内拒绝（#4；schemas 未映射） |
| 异构列表字面量（map 值同类） | 4 | 静态拒绝（#17，Phase 6 起） |
| `AS COPY OF <graph>`、多标签节点类型 | 2 + 2 | 设计内拒绝 |
| MIN/MAX/SUM over list（binder 无列表聚合） | other 3 | 引擎函数重载缺失 |
| TCK 语料/文法问题 | parse-error 4：3 例 setup 用 openCypher `CREATE (…)`/`UNWIND`（GQL 应为 INSERT/FOR）+ 1 例 `CREATE GRAPH ANY AS COPY OF` 文法歧义 | 语料自身问题 |
| GQLSTATUS 错误码 | 影响全部异常场景的断言深度（不计失败数） | 未实现 |
| 其余拒绝面（自测 unsupported 27 例钉死，不进 TCK） | — | 多跳 QPPI、SIMPLE 路径模式、counted `SHORTEST k`/`SHORTEST GROUP(S)`、DIFFERENT EDGES、`WHERE IS [NOT] LABELED`、`%` 标签通配、边标签表达式、SESSION SET SCHEMA/TIME ZONE/PARAMETER、`FOR … WITH ORDINALITY/OFFSET`、`SELECT *`+GROUP BY、SET 加标签、复合查询（UNION/EXCEPT/INTERSECT）——逐条响亮报错 |

（rejected-by-layer 23 = SCHEMA 13 + 事务包裹 1 + qualified graph name 1 + 异构列表 4 + AS COPY OF 2 + 多标签 2；加 other 3、parse-error 4 = 30。）

### ③ 语义近似残余（能跑但与 ISO 有差）

- REMOVE 属性=置 NULL 而非删除（#1）；SET 仅在赋值项相互干扰时插 `WITH` 快照，
  否则直翻（等价性是论证而非机械保证，#2）。
- 图类型注册表**不落 WAL**：重启丢类型、需重跑 `CREATE GRAPH TYPE`（#10；图名与其 schema 持久，
  仅类型注册表不持久）。Phase 7 评估后主动推迟——扩展加载本身就不持久，先做引擎工程，
  `_HANDOVER_GQL.md` 明注"勿重复评估"。
- 无向/混向边拼写折叠为 `-[e]-`（#9）；`CREATE GRAPH g TYPE t` 等非 ISO 拼写被宽容接受（#14）。

### ④ 质量基建薄弱点

- **回写 harness 不可靠**：`E2E_REWRITE_TESTS=1` 可把实际值回写期望，但回写实现自述低效、
  不处理并行 CASE、需 `TEST_JOBS=1`（`test/runner/e2e_test.cpp` rewriteTestFile 注释），
  实操会写坏 .test 文件——失败时应直接从失败输出读取实际值，勿依赖回写。
- **TCK 异常只验存在性**：无 GQLSTATUS 码断言，错误码回归当前测不出（REPORT methodology）。
- **规模盲区**：全部测试在极小图上进行（如 basic.test 每 CASE 仅 1–3 节点），无规模/并发/性能验证，
  翻译层与注册表的开销、`replaceExprs` 在大查询上的正确性均未压测。
- **TCK 只跑了 untyped 图模式**（README "Measured … untyped-graph mode"；REPORT 同注）——
  表图/类型化图语境下的 TCK 表现是未知数。
- 4 例 parse-error 属语料自身问题（3 例 openCypher setup 语法 + 1 例文法歧义），短期不可修。

## 4. 结论与适用边界

**适合**：AI/工具按标准 GQL 生成的常见查询与写入（MATCH/SELECT/GROUP BY/路径模式/标签表达式/
schema DDL），在 LadybugDB 上要**经双跑验证、错了会喊**的场景。它把"支持 GQL"从错觉变成
可度量的承诺——85/85 双跑 + 165/206 TCK + 失败全响亮，是当前最诚实的口径。

**绝不可声称**：
- **ISO GQL 合规认证**——165/206 ≠ 通过认证，19 条语义差异与第 3 节拒绝面客观存在；
- **原生 GQL 引擎**——这是 Cypher 方言上的翻译模拟器，双重解析、无 plan cache、
  程序模型与值模型均有天花板；
- 静默兜底的"基本都支持"——本层的立场是宁可响亮拒绝，不给错误答案。

**按成本排序的 backlog**（均在翻译层内做，不动引擎语义）：

| 顺位 | 项 | 理由 |
|---|---|---|
| 1 | 扩展标量/聚合函数的跨类型 max/min（LIST(ANY) 支持） | 解 TCK "other 3"，只加函数不动算子 |
| 2 | GQLSTATUS 错误码信封 | 让 TCK 异常场景从"有错即可"升级为真断言 |
| 3 | SCHEMA 命名空间模拟 | 解 TCK 最大失败块（13 场景） |
| 4 | 原生 GQL 执行 / 双向互通 | **大工程**，≈重写半个 planner；维持远期选项 |

（图类型注册表 WAL 持久化不在表内：已评估并有意推迟，前置依赖是"扩展附着持久化"引擎工程，
见 `_HANDOVER_GQL.md` Phase 7 记录。）
