# GQL 兼容层第三轮咨询简报 — Phase 11 前：GQLSTATUS / 异构残余 / SCHEMA 命名空间

> 2026-10-02。求：对开放问题的利弊判断与最小落地切片；回帖格式见 §7。
> 前情：Q2 咨询回帖经我方独立验证（`gql_consult_q2_reply_verify.md`）后已按修正落地 Phase 10。

## 1. 背景与硬约束（不可谈判）

- 项目 = GQL→Cypher 翻译层（`CALL GQL("...")` → vendored opengql `GQL.g4` ANTLR → `GqlToCypherTransformer` → Cypher 文本 → LadybugDB 引擎管线）。**不是**原生 GQL 引擎。
- **不改** `GQL.g4`/`Cypher.g4`/引擎语义——翻译层本身是贡献；抄写优先（Apache-2.0 署名/NOTICE 保留），Neo4j 主引擎 GPL **不可抄**（只有其 front-end 解析器仓库是 Apache-2.0）；够用即可，不过度设计。
- **红线：0 静默错答**——响亮拒绝优于静默错答；TCK 语料 vendored 冻结不改（问题语料走 harness 例外+脚注披露）。
- 验收口径：GQL 语句翻译成等价 Cypher 双跑对照一致；门禁表述「TCK 172 不降」。

## 2. 事实表（已探机/读码钉死，请勿重复验证）

| # | 事实 | 证据 |
|---|---|---|
| 1 | 当前自测 112/112、TCK **172 过 / 23 挂 / 11 跳**（206 场景）；23 挂 = rejected-by-layer 19 + parse-error 4 + other 0 | 2026-10-02 实测 |
| 2 | 19 个 rejected 里 **13 个是 SCHEMA 命名空间**（CREATE/DROP SCHEMA 8+5），另 2 多标签节点、其余 LIKE/AS COPY OF/qualified graph name | `tck/REPORT.md` |
| 3 | vendored TCK 语料**不含任何 GQLSTATUS 码**（无 5 位码字样）；异常场景当前断言=「有错误抛出即可」（`error(regex)` `[\s\S]+`） | grep 全语料 |
| 4 | 我方错误路径三类：翻译层 `unsupported(...)` 抛 `RuntimeException: GQL feature not supported: X`（19 例 rejected 的来源）；ANTLR parse 报错（4 例 parse-error 的来源）；引擎异常（BinderException/ConversionException/RuntimeException）原样透传 | gql_transformer.cpp / gql_function.cpp |
| 5 | 引擎异常对象**没有** GQL 状态码字段；翻译层对引擎错误只截获文本 | 引擎源码（扩展 API 面） |
| 6 | schema 场景是**路径语义**：`CREATE SCHEMA /myschema`、`/dir/myschema`，副作用断言 `+schemas/+directories` 计数；DROP 钉「非空 schema 拒绝」「schema 名与 directory/graph/graph type 同名报错」 | Create1.feature [1]-[9]、drop1.feature [1]-[7] |
| 7 | harness 副作用校验只对空图可观察的 `+nodes/+edges` 生效（+schemas 不校验） | run_tck.py Phase 5 口径 |
| 8 | 引擎 catalog **无命名空间**：图/图类型是顶级条目；我方图类型注册表是每库扁平 map（ExtensionManager data 槽） | Phase 4/5 实现 |
| 9 | 异构列表**静态**防线：字面量元素类型类不一致（INT/DOUBLE 异类、忽略 null）→ `unsupported("heterogeneous list literal")`；map 值全拒 | scanValueShapes，gql_transformer.cpp:266-280 |
| 10 | 异构列表**运行时残余**：非字面量列表无法静态判别；引擎 bind 期把混合类型列表**静默同化**（STRING 是万能汇）——类型抹除=潜在静默错 | README #17（探机实证） |
| 11 | 语料无 `collect()` 场景；ANY 图属性列统一 JSON 类型（同列元素天然同型），typed 图表达式混合类型出现面窄 | grep 语料；Phase 9/10 事实 |

## 3. Phase 10 成果摘要（已落地，供对齐状态，勿重议）

- 比较/排序全序桥已落地（B1 `c894d91` + B2 `e7c51a9`）：六算子 `_gql_lt/le/gt/ge/eq/ne`(ANY,ANY→BOOL) + `_gql_sortkey`(ANY→STRING)，翻译层在 ANY 图比较上下文 splice、ORDER BY 键包装。数字=精确十进制文本比较（任意长、零 double 兜底）；裸文本形态判别（解析失败=字符串）；`True`/`False` 引擎拼写特判 bool；SQL NULL→NULL 三值；对象异内容/DATE/UUID/…/非有限 real 响亮拒。
- 已知残余（全部响亮或存储消型内在不歧义，README #21）：CASE WHEN 简写比较（`CASE x WHEN > 5`）与聚合实参内比较不 splice；ORDER BY DESC 下 NULL 排最前（引擎惯例）；嵌套数组内 >int64 的 int vs real 谓词仍走 Phase 8 double 比较器；裸文本恰巧拼成 JSON（字符串 'true'）是存储消型的内在歧义，parse-first 规则胜出。

## 4. 开放问题（重点）

### Q3-A GQLSTATUS 状态码：值得做吗？怎么做最省？

- **现状**：语料不断言码（事实 3），所以 TCK 无压力；我方错误三类（事实 4），引擎异常无码字段（事实 5）。
- **问 ① 性价比裁决**：语料只断言「有错」的今天，贴 GQLSTATUS 的收益是纯规范合规。做/不做/缓做，请给判断与理由。
- **问 ② 码表来源**：ISO 39075 码表只能从规范重推导（Neo4j 主引擎 GPL 不可抄，Apache-2.0 先例未知——你方若知道可抄的开源码表实现，请点名+许可证）。逐类贴码的映射表（19 个 rejected 类别各归哪个码族）是否值得全做，还是先做「语法错 vs 语义错 vs 目录错」三族粗分？
- **问 ③ 注入点**：候选 A=翻译层抛错时在消息前缀贴码（只覆盖 rejected 19，parse-error 4 与引擎透传错误不覆盖）；候选 B=harness 层把错误类别映射到码（不碰产品，纯测试自欺，价值最低）；候选 C=扩展 API 层面给错误加元数据槽（要动引擎 API，越界）。你方的意见与最小切片？

### Q3-B 异构列表运行时残余：红线守卫还是接受挂账？

- **现状**：静态防线已覆盖字面量（事实 9）；非字面量列表在引擎 bind 期静默同化（事实 10）。出现面窄但不可静态判。
- **问 ① 面到底多大**：typed 图上哪些表达式形态会产生运行时异构列表（`collect` 混合分支？函数返回列表？）——值不值得建守卫，还是残余挂账（README 已记）？
- **问 ② 守卫形态**：候选 A=运行时守卫函数（`_gql_guard_list(list)` 检查元素类型类一致，不一致响亮抛——包在哪些发射位？FOR 源非字面量、ORDER BY/sortkey 前、聚合前？）；候选 B=接受残余+文档。有无 Apache-2.0 先例可抄？
- **问 ③ 与 Phase 10 的交互**：桥已把比较/排序做成语义正确的——一个被同化的列表喂给 `_gql_sortkey` 会怎样（元素全 STRING 化后 rank 判断会错类）？这是否改变 ①② 的权衡？

### Q3-C SCHEMA 命名空间模拟：13 个 TCK 挂账值不值得解？

- **现状**：语料钉的是 `/path` 层级语义 + 同名互斥（事实 6）；引擎 catalog 扁平（事实 8）；harness 不校验 +schemas（事实 7）。
- **问 ① 模拟深度**：候选 A=注册表层级模拟（扩展注册表加 schema→成员映射，图名/图类型名带 schema 前缀存储，`CREATE/DROP SCHEMA` 改注册表，同名互斥/DROP 非空语义在翻译层校验）——能过多少场景？`/dir/myschema` 路径语义要不要真建模目录？候选 B=继续响亮拒 13 例挂账。
- **问 ② 副作用断言**：+schemas 计数要不要在 harness 扩展校验（run_tck 加副作用种类）？还是维持「只有 +nodes/+edges 校验」口径？
- **问 ③ 名字改写碰撞**：若走前缀改写（`s.g`→`s__g`），与用户自建 `s__g` 的碰撞怎么处理（保留字前缀？转义？）——业界扁平目录模拟命名空间的通行做法？

## 5. 已定事项（勿重议）

- Phase 10 比较桥的设计决策（含 parse-first 歧义规则、对象响亮拒、十进制精确比较）已定。
- 引擎不动（含错误对象加字段）；语法不动；TCK 语料冻结；registry WAL 持久化暂缓（勿重复评估）。
- Q6 plan cache、Q7 语料约定是纯内部工程，不咨询。
- 门禁口径「TCK 172 不降」+ 自测全绿。

## 6. 代码锚点

- 翻译层拒绝面：`extension/gql/src/gql_transformer.cpp`（`unsupported()`、scanValueShapes :266-280、splice :2826-2976）
- 错误抛出与 normalize：`extension/gql/src/function/gql_function.cpp`（bindFunc :299 起）
- 桥函数：`extension/gql/src/function/gql_json_functions.cpp`（:983-1582）
- 图类型注册表：`gql_transformer.cpp` GraphTypeSpec 段 + `ExtensionManager::setData/getData`
- TCK 转换器：`extension/gql/test/tck/run_tck.py`（异常场景口径、HEADER_DRIFT_SCENARIOS）
- schema 语料：`extension/gql/test/tck/features/statements/catalog-modifying/{create/schemas/Create1,drop/drop1}.feature`

## 7. 回帖格式

1. 每条断言尽量带探机/行号证据；纯外部事实（ISO 码表、开源先例）标来源+许可证。
2. 每个开放问题给「推荐方案 + 理由 + 最小落地切片 + 不做什么」。
3. 若发现本简报事实有误，请先指出（我们可独立复验）。
4. 篇幅 ≤ 200 行。
