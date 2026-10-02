# GQL 兼容层第五轮咨询简报 — 收官前盘点：多跳 QPPI / 小语义包 / 异构运行时残余 / 相对限定名

> 2026-10-02。求：对 4 个决策点的做/不做裁决与最小切片；回帖格式见 §7。
> 前情：Q3/Q4 咨询回帖经我方独立验证后已按修正落地（Phase 11 + Q4 轮，`gql_consult_q3_reply_verify.md`
> 附二）。红线完好：0 静默错答、wrong-GQLSTATUS=0。本轮是「可做未做」清单的收官盘点——
> **这批功能 TCK 语料压力全部为 0**（事实 5），做与不做都不动门禁，故纯按价值裁决。

## 1. 背景与硬约束（不可谈判）

- 项目 = GQL→Cypher 翻译层（`CALL GQL("...")` → vendored opengql `GQL.g4` ANTLR → `GqlToCypherTransformer` → Cypher 文本 → LadybugDB 引擎管线）。**不是**原生 GQL 引擎。
- **不改** `GQL.g4`/`Cypher.g4`/引擎语义——翻译层本身是贡献；抄写优先（Apache-2.0 署名/NOTICE 保留），Neo4j 主引擎 GPL **不可抄**（只有其 front-end 解析器仓库是 Apache-2.0）；够用即可，不过度设计。
- **红线：0 静默错答**——响亮拒绝优于静默错答；宁可挂账不可近似到静默指错。
- TCK 语料 vendored 冻结不改；验收口径双跑对照；门禁表述「TCK 190 不降」+ 自测全绿。

## 2. 事实表（已探机/读码钉死，请勿重复验证）

| # | 事实 | 证据 |
|---|---|---|
| 1 | 当前门禁：自测 **124/124**、TCK **190 绿（70 过+120 note）/ 9 挂 / 7 跳**（206 场景），wrong-GQLSTATUS=0；9 挂 = parse 4 + rejected-by-layer 5（LIKE 1、AS COPY OF 2、多标签 2），全部响亮 | 2026-10-02 实测 + REPORT.md |
| 2 | **QPPI 边界**：单边匿名 `( ()-[]->() ){m,n}` 已映射（外层量词落递归关系槽）；**多边** `( ()-[]->()-[]->() ){m,n}` → `quantified path pattern with multiple edges` 响亮拒；嵌套量词/内部节点绑定/量词节点模式/子路径变量/括号内 pathMode/WHERE/并集交替均响亮拒 | gql_transformer.cpp:1436-1509 |
| 3 | **四个小语义全响亮拒**：SIMPLE → `SIMPLE path mode (LadybugDB has WALK/TRAIL/ACYCLIC only)`；DIFFERENT EDGES（match mode）→ `DIFFERENT EDGES match mode`；`IS [NOT] LABELED` → tagged 拒；`%` → `% label wildcard` | :1234、:1591、:3238、:3202 |
| 4 | **相对限定名** `dir.name`（文法 `(objectName PERIOD)+`）响亮拒（消息 `qualified graph name (schemas are not mapped)`，措辞遗留、行为=拒）；绝对 `/a/b` 已通（注册表物理名改写） | GQL.g4:1469-1472 + gql_transformer.cpp:281-283 |
| 5 | **TCK 语料压力 = 0**：features/ 全树 grep `SIMPLE`/`DIFFERENT`/`IS LABELED`/`%` 标签/多边量词括号模式——零命中 | 2026-10-02 grep 复验 |
| 6 | 语法两层（勿混）：**matchMode**=REPEATABLE ELEMENT(S)/DIFFERENT EDGE(S)，作用于 pathPatternList；**pathMode**=WALK/TRAIL/SIMPLE/ACYCLIC，作用于模式前缀 | GQL.g4:807-820、:903-912 |
| 7 | 引擎路径面（Phase 3/7 实证）：MATCH 默认可重边（≈WALK）；`*TRAIL`=边互异；`*ACYCLIC`=**仅中间节点**互异（首尾不受限）；`IS_TRAIL(p)`=全 rel 互异、`IS_ACYCLIC(p)`=**全节点互异含首尾**（= GQL 精确语义，谓词 wrap 实现）；SHORTEST/ALL_SHORTEST 要求 lower=1 | handover Phase 3/7 记录 |
| 8 | **异构列表三层现状**：静态防线只吃字面量（元素类型类不一致拒）；非字面量列表引擎 bind 期**静默同构化**（STRING 万能汇，list_creation.cpp:41-52）= 潜在静默错（README #17）；Q3-B 已判静态守卫死刑（守卫拿到的是同化后列表） | scanValueShapes；README #17；Q4 回帖 Q3-B 复核 |
| 9 | 异构面窄的结构原因：ANY 图属性列统一 JSON 类型（同列天然同型）；typed 图表达式混合类型出现面窄；语料无 `collect()` 场景 | Phase 9/10 事实 + grep 语料 |
| 10 | Phase 8 起 FOR 异构/嵌套**字面量**列表逐元素包 `_gql_to_json`（保型出口）；比较/排序/聚合经 `_gql_*` 桥按 LogicalType 分类——被同化的全 STRING 列表喂给桥会**错类排名**（静默） | gql_transformer.cpp FOR 发射 + gql_json_functions.cpp |

## 3. 已落地状态摘要（Phase 11 + Q4，勿重议）

- SCHEMA 命名空间注册表（路径集合+目录=非空真前缀+物理名 `_gqlsch__` 改写+保留前缀拒）、
  限定名 roundtrip、GQLSTATUS **窄贴码**（42000/25G03/22G0N/22G0P，其余无码）+ 三档码断言
  （码匹配/无码 note/错码挂——graph-types [6] wrong-gqlstatus 现行犯是它抓的）。
- USE 真形态 = 查询前缀从句 `USE /foo/g MATCH …`（独立 USE 非语句；`USE GRAPH x` 勿写）；
  层外建图冲突检查并查引擎 catalog。残余只剩：注册表不落 WAL、相对限定名（即 Q5-4）。

## 4. 开放问题（重点）

### Q5-1 多跳 QPPI：做/不做/怎么做？（最大决策点）

- **现状**：单边匿名 QPPI 已通；多边拒绝（事实 2）。GQL 语义上多边重复单元的内部绑定是**列表**
  （Cypher var-length 只出一条 rel 列表，无对应物——这是一期拒绝的因）。
- **问 ① 价值裁决**：零 TCK 压力（事实 5）、挂账账面为 0，纯覆盖面补全。做/不做/缓做？
- **问 ② 若做，翻译路线候选**：A=有界 `{m,n}` 固定长度 UNION 展开（组合爆炸，m/n 大怎么办）；
  B=单条 var-length + 边列表/中间节点列表谓词模拟（引擎 var-length 只有单 rel 型槽+递归槽，
  列表谓词从哪拿中间节点？）；C=其它你知道的路线。Neo4j front-end（Apache-2.0）的
  `QuantifiedPathPatternConverters`/`AddPathPredicates` 语义能抄到什么程度（只抄语义与展开式，
  不抄引擎规划）？有无其它 Apache-2.0 的 QPPI 展开实现可点名？
- **问 ③ 有界 vs 无界**：`{m,}`/`*` 无界多跳是否**必然天花板**（无法用单条 Cypher 语句表达
  多边重复单元）？若是，「只做有界 {m,n}」的子集值得吗？
- **问 ④ 内部绑定子集**：是否只支持匿名内部（现单边策略的延伸），带内部绑定的多跳永久拒？

### Q5-2 小语义包：SIMPLE / DIFFERENT EDGES / `IS LABELED` / `%` —— 语义确认 + 组合配方

- **现状**：四者全响亮拒（事实 3）；Phase 7 的 `_gql_pp{N}=body` + `WHERE IS_TRAIL/IS_ACYCLIC`
  谓词 wrap 机器已就位（事实 7）。
- **问 ① SIMPLE 语义**：ISO SIMPLE path mode 精确定义——是否 = 无重复元素（TRAIL）∧
  节点互异（ACYCLIC）？**闭合行走首尾重合是否例外**？若是，开路径 `IS_TRAIL∧IS_ACYCLIC`、
  闭路径怎么表达（`IS_TRAIL∧(首尾外节点互异)` 有现成谓词吗）？
- **问 ② DIFFERENT EDGES**：match mode 与 path mode 是两层（事实 6）——DIFFERENT EDGES 是否
  = 整条 pathPatternList 每个元素边互异 = 对每个路径变量包 `IS_TRAIL`？REPEATABLE ELEMENTS
  = 引擎默认（可重边）直接丢弃？
- **问 ③ IS LABELED / %**：`v IS LABELED expr` = 标签表达式求值谓词？`%` = 任意标签/恒真？
  Phase 7 的 `labels(v)` 分流（ANY 图 list_contains / 表图标量等值）能否原样覆盖——
  `%` 在表图是恒真还是"非空标签"？
- **问 ④ 价值裁决**：零 TCK 压力；四件估计合计 <1 天。值得全做、只做组合配方可覆盖的、还是全拒？

### Q5-3 异构列表运行时残余：第三条路，还是永久挂账？（Q-D 重开裁决）

- **现状**：静态字面量防线已住；非字面量 bind 同构化静默（事实 8）；静态守卫已判死刑
  （Q3-B）。残余贴着静默错红线——是全账里唯一未决的红线项。
- **问 ① 出现面**：typed 图上哪些表达式形态真会产生运行时异构列表（CASE 两分支 INT/STRING？
  函数返回列表？`+` 列表拼接？）——引擎 list_creation 同构化规则（:41-52，STRING 万能汇）
  的边界怎么读？值得先探机还是直接挂账？
- **问 ② 运行时 wrapper**：`_gql_hetero_check(list)`（元素类型类不一致响亮抛）包在哪些发射位
  （FOR 源非字面量 / 比较实参 / sortkey / 聚合实参）？每行一次校验的开销 vs 收益？
  有无 Apache-2.0 先例（值模型带运行时类型守卫的）？
- **问 ③ 范围收窄选项**：ANY 图属性列天然同型（事实 9）——残余面只在 typed 图表达式。
  只对 typed 图包守卫、ANY 图免检，是否就是正确切法？
- **问 ④ 裁决联动**：Q4 已把 Q-D 判「挂账」。本轮是给它一次重裁：做 ②/③ 小切片，或
  **永久挂账勿再开**。请明确表态。

### Q5-4 相对限定名 `dir.name`：近似解析还是永久拒？

- **现状**：绝对 `/a/b` 通；相对/点分形态响亮拒（事实 4）。GQL 相对名解析依赖会话
  catalog/schema 上下文（SESSION SET SCHEMA 一类）——我方翻译层**无会话 catalog 状态**。
- **问 ① ISO 解析规则**：`(objectName PERIOD)+` 相对名解析到什么（当前 schema？会话上下文
  从哪来、谁维护）？
- **问 ② 选项**：A=根解析近似（`dir.name` → `/dir/name` 一类）；B=维持永久拒；
  C=实现会话 schema 上下文 + 解析（多大工程？是否违反"够用即可"）。
- **问 ③ 红线裁决**：A 的失败模式是**用户意图图 ≠ 解析图 = 静默指错图**（红线类别）——
  是否意味着安全路只有 B/C？

## 5. 已定事项（勿重议）

- Q-E1（引擎错误文本映射）不做、Q-E2（AS COPY OF 贴码）维持无码——Q4 已决，勿重开。
- 注册表 WAL 持久化=暂缓（前置"扩展附着持久化"引擎工程），勿重复评估。
- 引擎/语法不动；TCK 语料冻结；问题语料 4 例走 harness 例外+脚注（等上游，不咨询）。
- 原生执行/双向互通=远期大工程，本轮不议。
- 门禁口径「TCK 190 不降」+ 自测全绿 + wrong-GQLSTATUS=0。

## 6. 代码锚点

- QPPI/小语义拒绝面：`extension/gql/src/gql_transformer.cpp`（translatePathTerm/flatten
  :1436-1509、SIMPLE :1234、DIFFERENT EDGES :1591、IS LABELED :3238、`%` :3202、
  相对名 :281-283）
- Phase 7 谓词 wrap 机器：同文件 `_gql_pp{N}` + `WHERE IS_TRAIL/IS_ACYCLIC` 段
- 异构列表静态防线：scanValueShapes（同文件 :266-280）
- 桥函数（运行时分类逻辑参照）：`extension/gql/src/function/gql_json_functions.cpp`
- 语法：`extension/third_party/opengql/GQL.g4`（matchMode :807、pathMode :907、QPPI :978/:1088、
  catalogObjectParentReference :1469）
- 语料（压力核对）：`extension/gql/test/tck/features/` 全树 grep 零命中，勿重复验

## 7. 回帖格式

1. 每条断言尽量带探机/行号证据；纯外部事实（ISO 语义、开源先例）标来源+许可证。
2. 每个问题给「推荐方案 + 理由 + 最小落地切片 + 不做什么」；Q5-3/Q5-4 请给出**明确裁决**
   （做/永久不做）。
3. 若发现本简报事实有误，请先指出（我们可独立复验）。
4. 篇幅 ≤ 200 行。
