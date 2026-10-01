# GQL 翻译模拟器 · 残留问题与关联资讯（外部咨询简报）

> **读者**：未接触本仓库的外部 AI 顾问。**目的**：就第 4 节开放问题征求可落地的方案建议。
> **状态快照**：2026-01-10。自测 **87/87**（双跑对照全绿）；opengql/tck **165 过 / 30 挂 / 11 跳**（共 206 场景），失败全为响亮报错。
> **回帖期望**：建议落到「翻译层内、不动引擎语义」的粒度；凡声称可行，请给出可双跑验证的形态（见 §7）。

---

## 1. 背景与硬约束（1 分钟）

**这是什么**：LadybugDB（Cypher 方言图数据库）上的 ISO GQL **兼容桥 / 翻译模拟器**——
`CALL GQL("...")` 把 GQL 语句翻译成等价 Cypher 文本，在引擎执行，结果与等价 Cypher 双跑一致。
**不是**原生 GQL 引擎；GQL 语义全部由翻译层在 Cypher 方言上模拟。

**验收口径**：*GQL 翻译成等价 Cypher 执行、双跑对照结果一致*——每条 GQL 用例旁挂等价 Cypher、
断言同一期望结果。红线是 **0 静默错答案**：未映射构造一律响亮抛
`GQL feature not supported: <构造>`，宁可拒不给错答案。

**硬约束（建议请勿违反）**：

1. **不改** vendored `GQL.g4`、`Cypher.g4`、引擎查询语义——翻译层本身就是本项目的贡献。
2. **抄写优先于自研**（Neo4j Cypher front-end 为范例，Apache-2.0 署名留档）；**够用即可**、不过度设计。
3. **不可声称** ISO GQL 合规认证或原生 GQL 引擎。
4. 引擎侧只做翻译层做不了的事（迄今仅 3 处：脏 rewrite 真 bug 修复、函数查找回退、扩展状态槽）。

**管线**：

```
GQL 文本 → ANTLR（vendored opengql GQL.g4，caseInsensitive）
        → GqlToCypherTransformer 显式逐语句分派（未映射构造响亮拒）
        → Cypher 文本 → 引擎 standalone-call rewrite → 正常管线
```

表达式经统一漏斗改写（函数别名 / 标识符映射，文本级、字面量与边界感知）；
模式翻译是结构化的（节点/边事件链发射），非整段透传。

---

## 2. 关联资讯：引擎与语言事实（讨论前必备，均经探机/实证，勿重复调研）

### 2.1 图模型（LadybugDB）

| 事实 | 影响 |
|---|---|
| **表 = 标签**的固定 schema：节点表一张一标签，节点恰一标签（=表名） | 多标签合取 `:A&B` 在表图不可满足（语义正确）；多标签 INSERT 表图必须拒 |
| 开放 **ANY 图**例外：labels 存 `STRING[]` 列，真多标签 | `labels(n)` 表图标量 / ANY 数组——谓词形态按图型分流 |
| **ANY 图动态属性列是 JSON 类型**（报错实证：`Function SUM did not receive correct arguments: Actual: (JSON)`） | 数值聚合/比较在 ANY 图属性上撞 binder 重载缺口（见 Q2） |
| 节点表必须有主键 → 图类型展开合成 `_gql_id SERIAL PRIMARY KEY` | GQL 节点类型本无键；`_ID` 是引擎保留字 |
| 无 REMOVE（→`SET n.prop = NULL` 近似）、无 map 值、无 TIME 类型、无 STDDEV | 已记 README 已知差异 |
| 列表字面量绑定时**同构归一**（混合类型强行 promote，STRING 万能汇） | 异构列表会静默改写值 → 我们静态拒绝字面量（#17） |
| 聚合后 ORDER BY 只认投影别名；裸 WHERE 只能挂 MATCH/WITH（FILTER→`WITH * WHERE`） | 翻译层补 `AS \`源文本\``、补 `WITH *` |

### 2.2 路径语义（实证）

- `*TRAIL` = 全边互异；`*ACYCLIC` = **仅中间节点**互异（首尾自由）——与 ISO"全节点互异"有差，
  翻译层对**每个 ACYCLIC 模式**与多跳 TRAIL 绑路径变量加 `IS_TRAIL(p)`/`IS_ACYCLIC(p)` 全路径谓词补齐。
- 路径值 nodes/rels 首尾各一次。GQL 量词 `*` = **0**..∞（裸 Cypher `*` = 1..）——下界翻译要显式 `*0..`。

### 2.3 GQL 语言侧（文法级实证）

- 数据写入是 **INSERT**（无 CREATE 造数据）；无 MERGE / FETCH / REPEAT…UNTIL / 裸 `TIME` / `LENGTH()`（是 PATH_LENGTH）；属性名 `at` 是关键字。
- **标签表达式**：ISO `<label conjunction>` 只有 `&`，filler 单 label 槽——`:A:B` **是 GQL 语法错**。
  三方分叉全在目标侧：Cypher 25 冒号合取（AND）/ 引擎表图 `:A:B` = 表并集（OR）/ 引擎 ANY 图 = AND。
  本层输入侧 parse 即拒，发射侧 match/谓词位只出 `&|!` 的 WHERE 谓词（运算符语义图型无关）。
- `CALL GQL` 执行链：bind（ANTLR parse+translate）→ 整句 rewrite → 重解析；**只有最后一条 Cypher 语句的结果对外可见**；必须单独成句。
- 扩展函数只注册在 main catalog（已加图 catalog 回退）；扩展状态槽每库一个（图类型注册表挂其上，不落 WAL）。

---

## 3. 残留问题总览（按痛感排序；数字均可回溯 `test/tck/REPORT.md`）

### 3.1 功能缺口（TCK 30 挂构成，全部响亮）

| 缺口 | 场景数 | 性质 |
|---|---|---|
| SCHEMA 命名空间（CREATE/DROP SCHEMA + 名称冲突错误条件） | **13**（最大块） | 设计内拒绝（见 Q4） |
| 异构列表字面量（INT/DOUBLE 混合，如 `[1, 2.0, 5, null, 3.2, 0.1]`） | 4 | 静态拒（见 Q1/Q5） |
| `CREATE GRAPH ... AS COPY OF <graph>` | 2 | 设计内拒绝 |
| 多标签节点类型（CREATE GRAPH TYPE 里 `(:A&B)`） | 2 | 设计内拒绝 |
| qualified graph name（`sch.gr`） | 1 | 设计内拒绝（schemas 未映射） |
| min/max over 列表值（字典序，`max([[1],[2],[2,1]])` → `[2,1]`） | 2 | binder 无列表聚合重载（见 Q1） |
| ANY 图 JSON 属性数值聚合（`sum(p.age)`） | 1 | **根因新发现**：属性列 JSON、SUM 无 JSON 重载（见 Q2） |
| TCK 语料问题 | 4 | 3 例 setup 用 openCypher `CREATE (...)`/`UNWIND`（非 GQL）+ 1 例 `CREATE GRAPH ANY AS COPY OF` GQL.g4 文法歧义 |

（另：GQLSTATUS 错误码未实现——不计失败数，但全部异常场景只能断言"有错"，见 Q3。）

### 3.2 结构性天花板（模拟器固有上限）

- **值模型**：无 map 值 / TIME 类型；异构列表字面量静态拒但**非字面量元素无法静态判别**（残余）；跨类型全序未实现。
- **固定 schema**：REMOVE≈SET NULL；NOT NULL 接受后丢弃；合成主键。
- **程序模型**：`CALL GQL` 必须是批内唯一语句；事务包裹程序拒绝；多语句只有末句结果可见。
- **会话副作用**：`USE GRAPH` 粘滞，返回后当前图不切回。
- **成本**：每次 CALL 双重解析（GQL→Cypher 文本→引擎再 parse）；表达式改写是文本级；无 GQL plan cache。

### 3.3 语义近似残余（能跑但与 ISO 有差）

REMOVE 属性=置 NULL 而非删除；SET 无序赋值快照仅在赋值项相互干扰时插 `WITH`（等价性是论证而非机械保证）；
图类型注册表不落 WAL（重启丢类型、重跑 DDL 即可，失败面响亮）；无向边拼写折叠为 `-[e]-`；
`CREATE GRAPH g TYPE t` 等非 ISO 拼写被宽容接受（#14 清单）。

### 3.4 质量基建薄弱点

TCK 异常只验"有错"（无码断言）；全部测试在极小图上（无规模/并发压测）；TCK 只跑 untyped 图模式
（表图语境下的 TCK 表现未知）；`E2E_REWRITE_TESTS=1` 回写不可靠（勿依赖）；Aggregation3 [1] 的语料
期望列名与查询不符（`n.name|sum(n.num)` vs `p.name, sum(p.age)`——语料噪音，见 Q7）。

---

## 4. 征求建议的开放问题（重点）

### Q1 GQL 跨类型全序与列表值 min/max（TCK 6 例相关）

**现状**：
- `FOR x IN [1, 2.0, 5, null, 3.2, 0.1] RETURN max(x)` 期望 `max=5, min=0.1`（INT/DOUBLE 混合**数值序**，
  胜出元素保留原类型）。现被我们**响亮拒**（`heterogeneous list literal`）——因为引擎列表绑定会把混合字面量
  归一到 STRING，max 比较会错（旧口径"过"是巧合：`'5'` 字符串序恰好对）。
- `FOR x IN [[1], [2], [2, 1]] RETURN max(x)` 期望 `max=[2,1], min=[1]`（列表**字典序**）。
  binder 拒（MIN/MAX 无列表重载）。

**约束**：优先扩展标量/聚合函数，不动引擎聚合算子与 binder。硬点：混合类型列表**字面量在进引擎绑定时
就被同构化**——扩展函数收到的实参可能已经不是原始元素。

**想问**：
1. GQL 值全序（跨 INT/DOUBLE/STRING/列表/null）的权威定义在哪抄最稳（ISO 比较章节 / Neo4j GQL 合规附录 / 其他实现源码）？
2. min/max over LIST(ANY) 以扩展聚合实现的最小接口建议？（输入如何保住异构元素类型？）
3. 混合数值字面量有没有**保类型**的翻译绕法（例如展开成 UNION/生成行的 UNWIND、或逐元素显式 CAST 后再聚合），
   以及胜出元素类型保真（期望 `5` 不是 `5.0`）怎么处理？

### Q2 ANY 图 JSON 属性的数值聚合/比较（根因新发现，TCK 1 例）

**现状**：ANY 图动态属性列类型是 **JSON**。`MATCH (p) RETURN p.name, sum(p.age)`（age 有的节点有、有的无）
被 binder 拒：`Function SUM did not receive correct arguments: Actual: (JSON)`。GQL 语义是跳过 null 求和
（期望 75）。同理 `max(p.age)`/`avg(p.age)`/比较运算在 ANY 图属性上都会撞 JSON 重载缺口。

**已排除**：改引擎函数目录加 JSON 重载 = 动引擎表面，与约束 1 冲突（除非论证它属"翻译层做不了的最小必要"）。

**想问**：
1. 翻译层对 ANY 图上的聚合/比较自动插 CAST 的可行性与坑？（`CAST(p.age AS INT64)` 对 JSON 值存在吗、语义？null 怎么走？）
2. 或扩展标量桥（如 `_gql_num(json) -> DOUBLE/INT64`）由翻译层在 ANY 图谓词/聚合位自动包一层——优劣？
3. 有没有第三条路（INSERT 时让数值属性落强类型列等）——注意 ANY 图 schema-less 是产品取舍。

### Q3 GQLSTATUS 错误码信封的最小可行

**现状**：全部异常场景只能断言"有错误抛出"；错误是 RuntimeError 文本。想升级为 GQLSTATUS 断言，
让错误契约可回归。

**想问**：neo4j-gql-status 的分类/码表可抄性；最小信封字段集（GQLSTATUS / STATUS_RECORDS / 诊断记录取舍）；
最先该实现的码集合（语法错 / 不支持特性 / 命名冲突 / 约束违例）；以及文本错误 → 码的映射表从哪抄。

### Q4 SCHEMA 命名空间模拟（TCK 最大失败块，13 例）

**现状**：`CREATE SCHEMA` / `DROP SCHEMA` / qualified graph name（`sch.gr`）全拒。Ladybug catalog 无 schema 层。
TCK 要求的错误条件不少（重复创建、名与 graph/type/目录冲突、DROP 非空 schema、IF EXISTS）。

**想问**：在无 schema 的 catalog 上模拟 schema 的方案权衡——命名前缀（`sch.gr` → 图名 `sch__gr`）vs
扩展内 schema 注册表 vs 别的；qualified name 解析规则与冲突语义怎么实现最省；DROP SCHEMA 级联语义
（13 例里有 4 例是 DROP 侧错误条件）最小怎么做。**这是最大的单一收益块**，但要求的错误语义面很宽。

### Q5 异构列表运行时残余

**现状**：字面量静态拒（Phase 6）；**非字面量元素**（列值/表达式，如 `[x, 1.0]`）无法静态判别，
运行时引擎列表归一仍会 erase 类型 → max/min 可能错答（已记 README #17 残余，属已知）。

**想问**：运行时拦截点的可行性（包装 max/min 为扩展函数先做类型检查？列表构造 hook？）；
或业界对这类残余的接受度先例——值不值得继续投入。

### Q6 双重解析与 plan cache

**现状**：每次 `CALL GQL` 完整 parse+transform，产出 Cypher 文本再被引擎 parse 一遍；无缓存。

**想问**：GQL 文本 → 产物缓存的失效策略（图 schema 变更 / `USE GRAPH` 切换 / 图类型注册表变化 / 扩展重载）；
键设计；或者 AST 级翻译到引擎 Cypher AST 的性价比评估（文本级 `replaceExprs` 的天花板在哪）。

### Q7（可选）TCK 语料污染的业界惯例

3 例 setup 用 openCypher `CREATE (...)`/`UNWIND`（GQL 应为 INSERT/FOR）；Aggregation3 [1] 期望列名
`n.name|sum(n.num)` 与自己的查询 `p.name, sum(p.age)` 对不上；1 例 `CREATE GRAPH ANY AS COPY OF`
是 GQL.g4 文法歧义。其他 GQL 实现怎么处理这类语料——转译 setup / 标 skip / 上游提 issue？
`CREATE GRAPH ANY AS COPY OF` 值不值得加预解析归一（我们已有同类宽容层）？

---

## 5. 已决事项（请勿重复讨论）

1. **图类型注册表 WAL 持久化 = 暂缓**：扩展加载本身不跨重启，前置是"扩展附着持久化"引擎工程；
   失败面响亮（重启后重跑 CREATE GRAPH TYPE）。已评估、有意推迟。
2. **`:A:B` 三方分叉已钉死**（2026-10-01）：输入侧是 GQL 语法错（parse 即拒，已加回归钉子）；
   发射侧 match/谓词位只出 `&|!`；唯一 `:A:B` 发射是 ANY 图 INSERT 创建位（设双标签=事实非谓词）。
   勿再假设存在"静默语义翻转"路径。
3. **G115 `PROPERTY_EXISTS`** 已修（→ `(v.prop IS NOT NULL)`，固定 schema 模型）。
4. **"布尔运算不校验操作数"是误报**已翻案（binder 有真类型检查；当年是 harness 正则不跨行）。
5. **TCK 语料 openCypher setup 不当产品 bug 修**（3 例）。
6. 引擎侧 3 处最小改动已封盘（standalone_call_rewriter / setFunctionFallback / ExtensionManager::setData）。

---

## 6. 参考资料与代码锚点

| 路径 | 内容 |
|---|---|
| `extension/gql/README.md` | 分级兼容矩阵 + 19 条已知语义差异（**#n 编号体系**） |
| `docs/gql_compat_review.md` | 成果/缺点评估全文（数字总账） |
| `docs/gql_compat_plan.md` | 原始目标与决策 |
| `_HANDOVER_GQL.md` | 逐阶段交付 + "勿重复调研"事实库 |
| `extension/gql/test/tck/REPORT.md` | TCK 逐场景明细 + 方法学（失败分类以它为准） |
| `docs/gql_ref/README.md` | 第三方参考源索引（Neo4j .scala / Cypher25Parser.g4 / ISO BNF；ISO 件不入库） |
| `extension/third_party/opengql/GQL.g4` | 输入文法（vendored，不改） |
| 代码锚点 | `extension/gql/src/gql_transformer.cpp`（翻译核心）、`src/function/gql_function.cpp`（bind/注册表）、`test/tck/run_tck.py`（TCK 跑批） |

---

## 7. 建议回帖格式（方便对照落地）

每条建议请尽量给出：

1. **落点**：翻译层哪个机制/文件（如 `translateLabelExpression`、`mapIdentifiersInto`、schema 桥）；
2. **是否动引擎**：不动 / 动哪一处、为什么属"翻译层做不了的最小必要"；
3. **双跑验收形态**：GQL 用例 + 等价 Cypher 用例长什么样（我们用 `.test` 声明式跑批）；
4. **抄写来源**：可抄的上游文件/规范条文（我们按 Apache-2.0 署名留档）；
5. **响亮性**：失败路径是否仍满足"0 静默错答案"。

---

## 8. 核验附录（2026-01-10，首轮外部方案落地前实探引擎）

供下一轮讨论直接引用的新事实（均已读源码/实证）：

| # | 事实 | 依据 | 影响 |
|---|---|---|---|
| 1 | **扩展 API 没有聚合注册口** | `src/include/extension/extension.h` 的 `ExtensionUtils` 仅有 `addTableFunc`/`addStandaloneTableFunc`/`addScalarFunc`/`addExportFunc` 系；无 aggregate | 扩展聚合（如 `_gql_max`）**今天做不了**；任何"扩展聚合"方案要么改引擎加 `addAggFunc` 扩展点（按约束 4 论证），要么改走标量桥+内建聚合/结构改写 |
| 2 | **扩展标量注册可行，且 `extension/json` 已有全套 JSON 标量** | `extension/json/src/main/json_extension.cpp`：`to_json`/`cast_to_json`/`json_extract`/`json_array_length` 等 | `_gql_to_json`/`_gql_num` 类标量桥零障碍，甚至可别名复用现有 JSON 函数 |
| 3 | **CAST FROM JSON 已存在**（走 string-cast 路径） | `vector_cast_functions.cpp:839`：`sourceTypeID == STRING \|\| JSON → bindCastFromStringFunction` | `CAST(p.age AS INT64/DOUBLE)` 大概率即探即过 → Q2 可零新函数先做翻译插 CAST；注意字符串解析语义（JSON 非数值→响亮 ConversionException，好） |
| 4 | **ANY 图 JSON 列的等值/排序今天就工作** | labels.test：ANY 图 `ORDER BY n.v`、`{v:1}` 属性匹配均过 | Q2 缺口集中在**聚合重载**（binder 签名表无 JSON），比较位不必自动包——包装面收窄到聚合位 |
| 5 | **输出保真坑比预想深** | TCK 期望 `75`；harness 期望格式走 `Value::toString`（double 是 `%.6f`） | `sum(CAST(... AS DOUBLE))` 会印 `75.000000` ≠ `75`——数值归一 harness 或 INT 保真必须先解决 |
| 6 | **许可证按文件头，不按仓库** | `docs/gql_ref/` 已留档的 6 个 neo4j/neo4j 文件全是 Apache-2.0 文件头（front-end 为 openCypher 血统） | "neo4j/neo4j 全是 GPL 勿抄"的说法过度；正确规则=逐文件看头。`org.neo4j.values` 比较器类的模块需单独核头；CIP2016-06-14 是规范语义，实现无许可问题 |
| 7 | 异构列表拒的落点是 **Transform 入口扫描**（`scanValueShapes`，`gql_transformer.cpp:188`）；FOR 发射在 `translateForStatement`（`:707`） | 读码确认 | 列表保型包装应落在 FOR/列表字面量翻译位，不是扫描器 |
