# 第五轮咨询回帖 — 多跳 QPPI / 小语义包 / 异构残余终裁 / 相对限定名

> 对应 `docs/gql_consult_q5.md`（2026-10-02）。回帖 2026-10-02。事实表核验：**无误**（SIMPLE 等语义以四个独立外部源交叉钉死，见 Q5-2）。

---

## Q5-2（先答，它是四问里唯一有外部事实增量的）小语义包：**全做**，因为每一项都有"原生语义孪生"

### ① SIMPLE 语义钉死——且映射成本是四项中最低的（惊喜）

ISO 定义（四源互证）：**SIMPLE = 节点不重复，但允许首尾重合（simple cycle）；ACYCLIC = 节点全不重复（含首尾）**。

- SurrealDB `gql::ast::PathMode`（docs.rs）："Simple: no repeated node, **except the path may close on its start**; Acyclic: no repeated node"；
- Ultipa GQL 文档、RageDB GQL 文档、Microsoft Kusto GQL reference 同义（"SIMPLE: Same as ACYCLIC but allows the first and last nodes to be the same"）。

对照你们事实 7：**引擎 `*ACYCLIC` = 仅中间节点互异、首尾自由 = ISO SIMPLE 的精确定义**；`IS_ACYCLIC(p)` = 全节点互异含首尾 = ISO ACYCLIC（你们已用作 GQL ACYCLIC 补丁）。所以：

| GQL | 翻译 |
|---|---|
| ACYCLIC | 引擎 `*ACYCLIC` + `IS_ACYCLIC(p)` 补丁（现状，不动） |
| **SIMPLE** | **裸引擎 `*ACYCLIC`，不加任何谓词补丁** |

你们猜的"SIMPLE = TRAIL ∧ ACYCLIC"**不对**：节点互异（除首尾）蕴含边互异（边重复必致两端点重复），SIMPLE ⊆ TRAIL 天然成立，无需 TRAIL 包装。多跳绑路径变量同样零包装。**开路径闭路径同一条规则覆盖，没有例外分支**——这是四件里最便宜的一件。

### ② DIFFERENT EDGES：单模式 = TRAIL 原生；多模式 ≠ 逐路径 IS_TRAIL

- matchMode 作用于**整条 pathPatternList**（你们事实 6 已钉）：DIFFERENT EDGES = 整个匹配结果里**跨模式**边互异（Microsoft Kusto GQL 参考的 mode 组合表：`DIFFERENT EDGES → cycles=unique_edges`，含 multi-path star 场景——同一语义读法）。
- 你们猜的"对每个路径变量包 IS_TRAIL"**只在单模式成立**；多模式（逗号列表）需要**成对边不相交**谓词（`rels(p1) ∩ rels(p2) = ∅`），IS_TRAIL 管不着跨模式。
- **最小切片**：单模式 pathPatternList 的 DIFFERENT EDGES → 发引擎 TRAIL 语义（`*TRAIL` 原生，事实 7）；**多模式响亮拒**（子集边界，README 记）；REPEATABLE ELEMENTS = 引擎默认（可重边）→ **接受为 no-op**（显式关键字配默认语义，丢弃无静默差）。

### ③ IS LABELED / `%`：复用 Phase 7 标签谓词机器

- `v IS [NOT] LABELED <label expr>` = 对 v 的标签集合求值标签表达式——**与 match 位标签过滤同一条发射路**（你们已定"match/谓词位只出 `&|!` WHERE 谓词"），把 IS LABELED 的操作数接进现有 `translateLabelExpression` 谓词出口即可，表图/ANY 图分流原样继承。
- `%` = 通配标签表达式：**表图恒真**（节点恰一标签=表名，事实）；**ANY 图 = `size(labels(v)) > 0`**（labels 存 STRING[]，可能空）。IS LABELED % 同理。
- 一处注意：`IS NOT LABELED` = 谓词整体取反，不是标签表达式内 `!`——发射时别混。

### ④ 价值裁决：**全做**。零 TCK 压力但四件各有原生孪生，合计 <1 天的估计合理；每一件的失败面都是响亮拒子集边界，红线无损。

**不做什么**：多模式 DIFFERENT EDGES 的跨模式边不相交谓词（工程量与收益不配，响亮拒）；为 SIMPLE 加谓词包装（引擎原生已是精确语义，加了反而错）。

---

## Q5-1 多跳 QPPI：**缓做（挂账）**——子集设计备好，触发条件写死

- **价值裁决**：零 TCK 压力 + 无真实用户需求驱动 + 全账无红线欠款——纯覆盖面补全，**缓做**。触发条件：出现第一个真实查询需要多边量化时开。
- **无界 `{m,}`/`*` = 必然天花板（永久拒）**：多边重复单元的重复次数不定 → 内部绑定列表长度不定 → 单条 Cypher 语句无法表达（var-length 只有单 rel 型槽 + 递归槽，你们事实 2 的拒绝因正是这个）。这不是工程量问题，是表达力问题，挂"模拟器固有上限"。
- **若做，唯一可行子集 = 有界 `{m,n}` + UNION 展开**（路线 A）：
  - 每个长度 k ∈ [m, n] 展开为定长模式（单元重复 k 次），`UNION ALL` 拼接；
  - **内部绑定不是障碍**：展开后每跳的内部变量是定长模式里的显式变量，投影位显式构造列表 `[b1, ..., bk]`——路线 B（var-length + 谓词）才是死路（拿不到中间节点，你们自己已点破）；
  - 爆炸控制：`n - m ≤ 4`（或总展开分支 ≤ 8），超限响亮拒；
  - 抄写源：**neo4j/opencypher front-end**（Scala，Apache-2.0）的 QPP desugar 语义可参考展开形态（只抄语义，不抄规划）；⚠️ SurrealDB 的 gql 模块是 **BSL 1.1 不是 Apache-2.0，不可抄**；未找到其它干净的 Apache-2.0 QPPI 展开实现。
- **内部绑定子集**：匿名内部先行（现单边策略延伸）；带内部绑定的展开技术上可行（上文）但与匿名同批做掉更省——**永久拒的只有无界**。

---

## Q5-3 异构列表运行时残余：**做——但形态是 bind 期守卫，不是我前两轮判死刑的 exec 运行时守卫**（终裁）

**先诚实修正我自己的判决**：Q3-B/Q4 我判"守卫死刑"，理由是"守卫函数拿到的已是同化后列表"——对 **exec 期**成立。但你们 ② 的 `_gql_hetero_check` 有一个我当时没点破的可行变体：**守卫放在扩展函数的 bindFunc 里**——`ScalarBindFuncInput.arguments` 携带每个实参表达式**引擎推导后的静态类型**（`bind_function_expression.cpp:89-92`，`to_json` 的 bindFunc 是先例），**bind 期在列表构造同构化（`list_creation.cpp:41-52`）上游**。检测点位置问题就此消解。

**最小切片（~1 天，做完永久关闭此账）**：

1. 扩展变长标量 `_gql_list_checked(ANY...) -> LIST`：`bindFunc` 逐实参查 `dataType`，类型类不一致（沿用 scanValueShapes 的类划分，null 跳过）→ **bind 期响亮抛**；一致 → bindFunc 定 `LIST(<该型>)` 返回类型；exec = 照抄 list_creation 的列表构造（N 个 vector 拼 LIST）。
2. 翻译层发射位：**typed 图**（你们 ③ 的切法正确——ANY 图属性是 JSON 动态类型，静态判型会误拒合法动态查询，**ANY 免检**）内含 ≥1 个非字面量元素的列表字面量 → `[e1, e2]` 改写为 `_gql_list_checked(e1, e2)`。FOR 源位**不改**（Phase 8 的 `_gql_to_json` 保型出口已覆盖且语义更优）。
3. 效果：可静态判型的异构（`[intCol, 1.0]`、CASE 字面量异型分支——引擎 binder 自己会把分支型统一，判型用引擎的真理）→ 响亮拒；同型 → 守卫零开销语义透明。残余从"静默错"变"响亮拒"，红线账清零。
4. 双跑：typed 图 `FOR x IN [1, p.age] RETURN x` → 期望响亮拒；`FOR x IN [p.a, p.b]（同 INT 列）RETURN x` → 正常过。

**不做什么**：exec 期逐行守卫（信息已抹除，原理死刑维持）；ANY 图判型（动态类型误拒）；为 CASE/UDF 等复杂形态自建类型推断（bindFunc 直接用引擎推导结果，一行推断都不用写）。

---

## Q5-4 相对限定名 `dir.name`：**做 A（根解析）——它不是近似，在你们的不变量下是精确语义**

红线顾虑（"用户意图图 ≠ 解析图 = 静默指错"）成立的前提是**会话 schema 上下文可变**。在你们的模拟器里它**不可变**：会话 schema 永远是 root（前提是 `SESSION SET SCHEMA` 保持响亮拒——GQL.g4:39-41 有此子句，**先核实你们的拒绝消息已就位**；若尚未拒，本切片第一刀就是补拒）。不变量成立 ⇒ `dir.name` → `/dir/name` 是 ISO 相对解析在你们支持子集内的**精确结果**，不是近似。

**最小切片**：
1. 核实/补 `SESSION SET SCHEMA` 响亮拒（不变量守卫）；
2. `(objectName PERIOD)+` 相对名（`catalogObjectParentReference` 第二产生式，`GQL.g4:1471`）按 root 前缀拼路径后走**现有绝对路径 mangling 同一条码路**（`_gqlsch__` 改写 + 注册表反查复用，无新机制）；
3. 顺带把拒绝消息 `qualified graph name (schemas are not mapped)` 的遗留措辞改掉（schemas 已映射）。
4. 双跑：`CREATE SCHEMA /dir` + `CREATE GRAPH dir.name ANY` ↔ 等价 Cypher 对 `_gqlsch__dir__name` 的 DDL；`dir.name` 与 `/dir/name` 互指同一物理图。

**不做什么**：选项 C（会话 catalog 状态机——为 "够用即可" 所拒，且无不变量破坏者就没有服务对象）；裸 B 永久拒（A 的成本与 B 几乎相同，账面还能清零"残余只剩"清单的最后一项）。

---

## 汇总裁决

| 问 | 裁决 | 一句话 |
|---|---|---|
| Q5-1 多跳 QPPI | **缓做** | 无界=表达力天花板永拒；有界 UNION 展开子集备好（n-m≤4），触发条件=首个真实需求 |
| Q5-2 小语义包 | **全做（<1 天）** | SIMPLE=裸引擎 `*ACYCLIC` 零包装；DIFFERENT EDGES 单模式=TRAIL/多模式拒/REPEATABLE=no-op；IS LABELED 接现有标签谓词路；`%`=表图恒真/ANY 非空 |
| Q5-3 异构残余 | **做，bind 期守卫，做完永久关闭** | `_gql_list_checked` 的 bindFunc 用引擎推导类型在抹除点上游检查；typed 图限定；FOR 源不动 |
| Q5-4 相对限定名 | **做 A** | 会话 schema 不可变（先核实 SESSION SET SCHEMA 响亮拒）⇒ 根解析是精确不是近似；复用现有 mangling |

顺序建议：Q5-2（最大覆盖面/最小成本）→ Q5-3（红线账清零）→ Q5-4（残余清单清零）→ Q5-1 挂账等触发。全部落地后"可做未做"清单真正收官。
