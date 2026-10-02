# 第三轮咨询回帖 — GQLSTATUS / 异构残余 / SCHEMA 命名空间

> 对应 `docs/gql_consult_q3.md`（2026-10-02）。回帖 2026-10-02。每条：推荐 + 理由 + 最小落地切片 + 不做什么；证据带行号/探机。

---

## 0. 事实勘误（先行，可独立复验）

**简报事实 #3 有误：TCK 语料含 GQLSTATUS 码，共 20 处断言。**

```
$ grep -rn "exception condition should be raised:" extension/gql/test/tck/features | 统计
  16× 42000    1× G2000    1× 25G03    1× 22G0P    1× 22G0N
```

例：`create/schemas/Create1.feature:38`（`raised: 42000`）、`:103`（`raised: 25G03`）。是 harness（`run_tck.py:15-16` 注释自承）**主动丢弃了码断言**，降级为"有错即可"——不是语料没钉。

**这推翻 Q3-A 的前提**（"语料只断言有错，贴码是纯规范合规"）：语料里有 20 处现成的、可执行的码契约，且 harness 升级后可免费消费。Q3-A 从"缓做"变"做"。

**对账 13 挂的构成**（供核对）：Create1 9 例 + drop1 7 例 = 16，其中 drop1 [1][2] 用 `catalog-1 catalog` fixture——`run_tck.py:427` 只支持 `an empty catalog`，这两例应在 11 跳而非 19 rejected（13 = Create1 8 + drop1 5 与此吻合；[8]/[9] 之一应落在 parse-error 4 里）。

---

## Q3-A GQLSTATUS：做（前提已变），切片 = 产品贴码 + harness 恢复码断言（同批）

**推荐理由**：① 语料 20 处钉码是白捡的可执行规范——今天 19 个 rejected 场景"过"只证明了"有错"，没证明"错得对"（如把目录冲突错成语法错也过）；② 与 Q3-C 天然耦合：schema 落地时 9 个错误场景本来就要抛"对的错"，顺手贴码一次成型；③ 不做的隐性成本：错误契约不可回归，重构翻译层时"拒绝面"无网兜。

**码表来源**：**以语料钉的 5 个码为唯一契约**（42000 目录/语法泛化错 ×16、25G03 只读事务、22G0N/22G0P 数据异常子类、G2000）。SQLSTATE 类结构可参考 PostgreSQL `errcodes.txt`（PostgreSQL License，宽松，仅结构参考）；GQL 子类码全文只在 ISO 39075（付费）——**不自创细分码**，超出语料钉法的一律用最接近的已钉码。

**最小落地切片**（候选 A 扩展版，不碰引擎）：

1. 翻译层 `unsupported()` 与 ANTLR parse 报错两漏斗贴码前缀：`[42000] GQL feature not supported: ...`（语法/目录语义全用 42000——语料对目录冲突就用它）；"事务包裹程序"拒绝贴 `[25G03]`（顺带让 Create1 [8] 过，见 Q3-C）；"多语句/NEXT"拒绝贴 `[42000]`（让 [9] 过）。
2. harness `error(regex)` 升级为可选码断言：`error(regex, gqlstatus?)`。**分三档判**：码匹配=过；无码但有错=**过-with-note**（降级，保 TCK 172 不降）；有码但错码=**挂**（这才是契约的意义）。
3. 引擎透传错误（Binder/ConversionException）：不贴码走"过-with-note"档，或文本前缀映射表只覆盖 ConversionException→22xxx 一族——其余不映射。

**不做什么**：候选 B（harness 自欺映射，零产品契约价值）；候选 C（动引擎错误对象，越界已决）；19 类逐类贴细分码（无验收源=自创规范）；STATUS_RECORDS 信封。

---

## Q3-B 异构列表残余：接受挂账 + 一个缩小切片；**运行时守卫原理上不可行**

**关键洞察（问②候选 A 死刑）**：同化发生在**引擎 bind 期**——守卫函数 `_gql_guard_list(list)` 拿到实参时，列表**已经是**同化后的 `LIST(STRING)`，原始类型信息在进函数前就抹除了。检测点位于信息抹除点的下游，原理性无解。这不是工程难度问题，是位置错误。

**问① 面到底多大**（typed 图，读码+语料分析）：运行时异构只可能来自三种形态——
1. 列表字面量含非字面量元素 `[x, 1.0]`（列值/表达式混合）；
2. `CASE` 分支异型（`THEN 1 ELSE 1.5`）；
3. 显式 CAST 制造的混合。
`collect()` 单列收集天然同型；`split()/range()` 返回同型列表；ANY 图**免疫**（属性是 JSON 文本，元素类型在 JSON 内保真，不过 bind 同化）。语料无 collect（事实 11）→ 真实面 = 窄上窄。

**问③ 与 Phase 10 的交互**：不改变权衡。`_gql_sortkey` splice 在 ANY 图上下文，ANY 图 JSON 列表不经同化；typed 图 ORDER BY 不 splice，同化列表走引擎原生比较——结果是"同化后自洽、与 GQL 原义偏离"，即 #17 已记的残余本身，无新增面。

**最小落地切片（可选做，半周）**：翻译层对 **typed 图**的两个语义敏感位（FOR 源、`ORDER BY`/sortkey 实参）做**静态类型判别**——typed 图 schema 固定，翻译期查 catalog 可得列类型（`resolveAnyGraph` 已证明翻译期可查 catalog）：判得同型→放行；判得异型→响亮拒；判不出→放行并记 #17。把残余从"不可判"收窄到"判不出"。

**不做什么**：运行时守卫函数（见上，原理不可行）；全表达式位静态判别（边际收益为零）；为残余动引擎列表绑定。

---

## Q3-C SCHEMA 模拟：做，与 Q3-A 同批；16 场景全可过（含两例"贴码反转"）

**探机钉死的前置事实**：引号图名含 `/` 被引擎当**文件路径**处理（`CREATE GRAPH \`/foo/mygraph\`` → `IO exception: Cannot open file. path: :/foo/mygraph`）——**零 mangling 路线不存在**，必须前缀改写。

**场景逐格对账**（Create1 9 + drop1 7，全过路径）：

| 场景 | 性质 | 过法 |
|---|---|---|
| Create1 [1][2][7]、drop1 [1][7]（5 例成功路径） | 执行 + 副作用 | CREATE/DROP SCHEMA 只改扩展注册表，Cypher 侧发 `EMPTY_RESULT_CYPHER`（`gql_function.cpp` 已有先例）；drop1 [1][2] 需 harness 补 catalog-1 fixture（全语料仅这 2 例用，grep 实证） |
| Create1 [3][4][5][6]、drop1 [2][3][4][5][6]（9 例错误条件） | 钉 42000 | 注册表校验（重名/与目录同名/与图同名/与图类型同名/非空 DROP）→ 抛 `[42000]`，与 Q3-A 贴码同做 |
| Create1 [8]（事务包裹，钉 25G03） | 程序模型天花板 | **贴码反转**：层拒"事务包裹程序"时按内层语句贴 `[25G03]`——拒绝仍在，码对了就算过 |
| Create1 [9]（NEXT 多语句，钉 42000） | 同上 | 拒"多语句"贴 `[42000]` |

**目录语义不用真建模**：注册表存 schema 路径集合，"directory"= 任一已注册路径的**前缀**（`/foo/myschema` 存在 → `/foo` 是 directory，[4]/drop1[3] 的互斥由此推导）。无目录实体、无计数器，一个集合搞定。

**名字改写与碰撞（问③）**：物理名 = 保留前缀 + 路径段拼接，如 `_gqlsch__foo__mygraph`；注册表维护"逻辑路径 ↔ 物理名"双向映射；翻译层对**用户手写 `_gqlsch__` 前缀**的标识符响亮拒（保留字防御）。业界通行做法即此类保留前缀 + 注册表反查（`_schema`/`pg_` 同款思路），无双向映射的前缀方案（裸 `s__g`）不要做——无法反查就无法做 DROP 非空校验。

**副作用断言（问②）**：做。扩展加一个 `_gql_schemas()` 表函数吐注册表内容（路径 + 成员计数，~50 行），harness 调它校验 `±schemas/±directories`——把 5 个成功路径从"过但未验"升级为真验证；不做则按"unchecked side effect"脚注口径（run_tck.py:578 现有机制）挂账，与 HEADER_DRIFT 先例同级，可接受但明显弱。

**最小落地切片**：
1. 注册表 schema 集合 + 前缀目录推导 + 双向映射（扩展现有图类型注册表同槽，WAL 暂缓口径一致——重启丢 schema 重跑 DDL，响亮）；
2. CREATE/DROP SCHEMA 翻译：校验（9 例错误语义）→ 注册表变更 → `EMPTY_RESULT_CYPHER`；qualified 图名/图类型名改写接入（`CREATE GRAPH /foo/g` → 物理名，USE 同理）；
3. 贴码（Q3-A 切片 1 同批）；
4. harness：catalog-1 fixture（2 例）+ `_gql_schemas()` 校验。

**不做什么**：真目录实体/层级对象；引擎 catalog 改造；`+schemas` 走引擎可观察指标（做不到，别试）；裸前缀无映射方案。

---

## 优先级与依赖

```
Q3-C 切片 1+2（schema 注册表 + 翻译）
  ∥ Q3-A 切片 1（贴码——schema 9 例错误条件依赖它，务必同批）
  → Q3-C 切片 4（harness fixture + _gql_schemas 校验）
  → Q3-A 切片 2（harness 三档码断言，全局生效）
Q3-B：小切片随手做或纯挂账，不阻塞。
```

预期收益：TCK 172 → **~188**（16 schema 场景全过，其中 2 例从 11 跳转入），20 处码断言从"有错"升级为"错得对"，错误契约从此可回归。
