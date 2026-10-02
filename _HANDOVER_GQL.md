# GQL ↔ openCypher 衔接补全 — 交接文档（持续更新至 2026-10-02 Q4 轮收工）

> 会话目标：把 `extension/gql` 从"薄翻译+整段透传"补成诚实可用的 ISO GQL 兼容层——
> **GQL 语句翻译成等价 Cypher 在引擎执行，结果与等价 Cypher 一致**（双跑对照验收）。
> 策略定调（用户）：抄写优先于自研（Neo4j Cypher front-end 范例，Apache-2.0 署名），
> 够用即可、不过度设计。LadybugDB 是 Cypher 方言引擎，**GQL→Cypher 翻译层是我们的贡献**。
> 完整计划：`docs/gql_compat_plan.md`（调研+规划全文）。

## 一、已完成（代码在工作树，已编译通过）

### 核心翻译层重写（`extension/gql/src/gql_transformer.cpp/.hpp`，~1100 行）
- **显式逐语句分派**（不再是 visitor 整树漫游），消除"静默透传兜底"：
  未映射构造一律抛 `GQL feature not supported: <构造>`。
- 已映射：MATCH/RETURN/ORDER BY/SKIP/LIMIT、**SELECT…FROM MATCH**（→ `USE GRAPH`+`MATCH…RETURN`）、
  **GROUP BY/HAVING 隐式分组**（→ `WITH keys, aggs WHERE … RETURN …`）、FILTER→WHERE、
  FOR→UNWIND、INSERT→CREATE（**结构性发射，不再全局词替换**——字符串字面量 `'INSERT ME'` 安全）、
  DELETE/DETACH DELETE、SET（含**无序赋值快照** `WITH n, rhs AS __v… SET …`）、
  REMOVE 属性→`SET …=NULL` 近似（标签 REMOVE 显式报错）、
  CREATE GRAPH（IF NOT EXISTS 保留 rewriteFunc 模拟）、DROP GRAPH→原生、
  SESSION SET GRAPH→USE GRAPH、START TRANSACTION/COMMIT/ROLLBACK→BEGIN TRANSACTION/COMMIT/ROLLBACK。
- 函数映射表（抄 Neo4j `GQLAliasFunctionNameRewriter` 重定向到 Ladybug 函数目录）：
  COLLECT_LIST→COLLECT、PERCENTILE_*→PERCENTILE*、CHAR_LENGTH→SIZE、LOCAL_DATETIME/ZONED_DATETIME→TIMESTAMP、
  PATH_LENGTH→LENGTH、ELEMENT_ID→internal_id；`||`→`+`。UPPER/LOWER/CEILING/LN 等 Ladybug 已内建同名。
- pattern 安全检查：量词路径/路径模式/搜索前缀/标签表达式 `&!%`→显式 unsupported。

### Phase 4 schema 桥（`gql_transformer.cpp` "Graph types" 段，2026-10-01）
- `GraphTypeSpec` 规范模型 + 进程内图类型注册表；`CREATE GRAPH TYPE`（嵌套规格/
  COPY OF/IF NOT EXISTS/OR REPLACE/DROP）与 typed `CREATE GRAPH`（裸类型引用/
  内联规格）→ `CREATE NODE TABLE`/`CREATE REL TABLE` DDL 展开；`ANY` → 原生 ANY 图。
- 合成主键 `_gql_id SERIAL PRIMARY KEY`；属性类型映射表（GQL predefined types →
  Ladybug 类型，NOT NULL 丢弃，TIME 系拒绝）。
- 预解析宽容：`normalizeCreateGraphTypeRef`（`CREATE GRAPH g TYPE t` → 裸引用）；
  `CREATE GRAPH TYPE t AS COPY OF u` 文法歧义误解析的检测与重解释。
- **引擎侧**：`Catalog::setFunctionFallback`（图 catalog 函数查找回退 main——
  否则 USE GRAPH 后 CALL GQL 报 function 不存在）。

### 引擎侧两处小修
- `src/parser/visitor/standalone_call_rewriter.cpp`：**脏 rewrite 真 bug 修复**（`rewriteQuery` 不清空→
  多语句批互相污染，即"CALL GQL 必须单独成句"的根因）。
- `extension/CMakeLists.txt`：VS 多配置生成器输出目录钉死到 `extension/<name>/build/`
  （否则 DLL 落在 `build/Release/`，测试加载路径找不到）。

### 测试与文档
- 新建双跑对照 conformance 测试：`extension/gql/test/test_files/{select,groupby,write,routing,unsupported}.test`
  （每条 GQL 用例 + 等价 Cypher 断言同一期望结果；错误口径用 `---- error`/`---- error(regex)`）。
- `extension/gql/README.md`：分级兼容矩阵（Neo4j 合规附录格式）+ 已知语义差异。
- `extension/gql/THIRD_PARTY_NOTICES.md`：Neo4j/opengql/ISO 来源登记（Apache-2.0 署名）。
- `_build_gql.bat`：聚焦构建脚本（VS2026，`-DBUILD_EXTENSIONS="gql"`，target e2e_test+lbug_gql_extension）。
- `docs/gql_ref/`：参考资料目录（Neo4j 5 个 .scala、Cypher25Parser.g4、ISO 官方 BNF + 索引 README）。
  Apache-2.0 部分随库；ISO BNF 是 ISO 版权数字工件，内部参考、不入库（.gitignore 排除）。
  成果/缺点评估：`docs/gql_compat_review.md`（2026-10-01）。

## 二、构建与测试方法

```bat
_build_gql.bat
:: 产出：build_v0211t/src/Release/e2e_test.exe + extension/gql/build/libgql.lbug_extension
```
```sh
# 仓库根执行（组名是路径风格，不是文件名）
E2E_TEST_FILES_DIRECTORY=extension ./build_v0211t/src/Release/e2e_test.exe --gtest_filter="gql~test~test_files~*"
```

## 三、当前测试战果

**✅ 137/137 全绿**（2026-10-02 Phase 11 + Q4 + Q5 收工）：
basic 11（回归）/ select 7（+PROPERTY_EXISTS 双跑）/ groupby 4 / write 7 / routing 4 / **path 18**（+多跳 TRAIL/ACYCLIC 双跑）/
**labels 3**（G074 双跑 + `:A:B` 拒绝钉子）/
**orderability 5**（Q1 全序聚合双跑：混合数值/列表值/混合值/原生显示/DISTINCT）/
**jsonagg 9**（Q2 双跑：ANY 图 sum/avg/max/min、混合保型、空组 NULL、DISTINCT、乘数、native SUM 对拍、非数值响亮拒）/
**comparebridge 11**（Q2-A 双跑：哨兵序算子、跨类序、三值 NULL/NOT、typed 操作数、sortkey ASC/DESC/多键、>2^53 精度、bool rank、等值桥、数组字典序、object 响亮拒）/
**schema 6**（CREATE GRAPH TYPE/typed CREATE GRAPH 双跑、注册表生命周期、TYPE 关键字宽容）/
**schemapath 12**（Phase 11+Q4：CREATE/DROP SCHEMA+IF EXISTS、目录语义、九错条件 [42000]、限定名 roundtrip、
组合拒、READ ONLY [25G03] vs 无码、`_gqlsch__` 保留前缀拒、`_gql_schemas()` 形态、USE 前缀从句双跑、层外图冲突）/
**smallmodes 8**（Q5-2：SIMPLE 闭三角/红线钉 m0→m1→m0→m2=0 行+裸 `*ACYCLIC`=1 行、DIFFERENT EDGES 单/多模式、
REPEATABLE no-op、IS LABELED/`%` 双图型）/ **listguard 3**（Q5-3：`_gql_list_checked` bind 拒/同型过/ANY 免检）/
**relname 3**（Q5-4：`dir.g`↔`/dir/g` roundtrip、点分 graph type、SESSION SET SCHEMA 拒钉）/
**unsupported 27**（+map 值/异构列表字面量拒绝，标签剩余拒绝面）。
**TCK 合规：206 场景 = 190 绿（70 过 + 120 pass-with-note）/ 9 挂 / 7 跳**（明细 `extension/gql/test/tck/REPORT.md`；
三档 GQLSTATUS 码断言已启用，wrong-GQLSTATUS=0）。

### Phase 5（合规收尾）交付记录（2026-10-01）
- **opengql/tck 落地**：整套 14 个 feature + sample data vendored 进
  `extension/gql/test/tck/`（Apache-2.0 + openCypher Neo 署名头保留，NOTICE 已登记）；
  `run_tck.py` 把 Gherkin 场景转成 .test 用例走 e2e 跑批 + 生成分类通过率报告。
  - 转换器要点：程序切分（FOR/MATCH/FILTER/WITH 链到结果部前是一条语句；CREATE/INSERT 等另起）、
    结果表→Value::toString 格式（True/False、%.6f、空串=null）、Scenario Outline 展开、
    能力标签过滤（@MinNodeLabelsZero 等=实现能力参数，不适用即跳过）。
  - **异常场景断言"有错误抛出"即可**（GQLSTATUS 码未实现）；副作用只在空图可观察时校验 +nodes/+edges。
- **TCK 打出的真 bug 修复**（翻译层）：
  - `FILTER` 原来翻成裸 `WHERE`（跟在 FOR 后是 Cypher 语法错）→ `WITH * WHERE`；
  - 未别名结果列名没按 GQL 惯例取源文本（引擎会把 `max(x)` 大写成 `MAX(x)`）→ 全部投影出口补 `AS \`源文本\``。
- **图类型注册表改每库**：原进程级静态表会跨场景泄漏（TCK 场景独立性直接被打破——
  `CREATE GRAPH TYPE mygraphtype` 第二次报 already exists）。新增引擎
  `ExtensionManager::setData/getData`（每库 k/v 槽），注册表挂库生命周期，序列化存储。
- **TCK 失败分类**（Phase 5 口径 44，Phase 6 复核更正见下）：rejected-by-layer 19（CREATE/DROP SCHEMA
  命名空间、LIKE/AS COPY OF、多标签节点——设计内不支持）、expected-exception-not-raised 15（**归因错误！**见
  Phase 6 记录）、parse-error 4（TCK 填充脚本用了 openCypher `CREATE (...)`，GQL 只有 INSERT——TCK 语料
  自身问题）、result-mismatch 3（异构列表 max/min 类型语义）。
- 跳过 11：能力标签（MinNodeLabelsZero/MaxNodeLabelsGTOne 等）+ TCK 未随包的 catalog-1 样例数据。

### Phase 6（测量修正 + 静默错响亮化）交付记录（2026-10-01）
- **推翻 Phase 5 的"布尔不校验"归因**：`123 AND true` 实际正确抛 BinderException（binder 对
  AND/OR/XOR/NOT 操作数强制 cast 到 BOOL，`bind_boolean_expression.cpp:26`）。15 个
  expected-exception-not-raised 全是 **harness bug**：`run_tck.py` 生成的 `error(regex)` 是
  `.+`，`std::regex_match` 全文匹配下 `.` 不跨行，ANTLR 多行 caret 消息匹配失败被误记"未抛异常"。
  失败的真实输入全是 GQL map 字面量（`RETURN {} AND true`）——错误抛了，只是消息多行。
- **修复**：`run_tck.py` error 正则改 `[\s\S]+`；分类器把"有错但正则不中"改记
  error-regex-mismatch（不再冒充未抛异常）。
- **静默错答案清零**：翻译层新增 parse-tree 扫描（`scanValueShapes`，Transform 入口）：
  map 值（`{}`/`{k: v}`）→ `unsupported("map value")`；未标注类型且元素类型类不一致的列表字面量
  （忽略 null，INT/DOUBLE 异类）→ `unsupported("heterogeneous list literal")`。此前
  `[1,'a',null,[1,2],...]` 会被 Ladybug 列表归一静默改写后 max/min 出错误结果。
  非字面量元素无法静态判别，残余差异记 README #17。pattern 属性表 `(n {k: v})` 不受影响
  （elementPropertySpecification 是另一条语法规则）。
- **TCK 新口径：206 场景 = 165 过 / 30 挂 / 11 跳**。失败全为响亮报错：
  rejected-by-layer 23（19 设计拒绝 + 4 异构列表）、parse-error 4（TCK 语料/文法）、
  other 3（min/max 无列表重载 2 例 + ANY 图 JSON 属性聚合 1 例——**2026-10-01 根因修正**：
  `sum(p.age)` 报 `Actual: (JSON)`，ANY 图动态属性列是 JSON 类型，SUM 无 JSON 重载，非"列表聚合"；
  binder 均响亮拒绝）。比计划 166 少 1：Agg2 [6] 旧口径
  "过"是巧合（min 的提升后结果恰与期望同字面），现被异构检查拦下——正是 0 静默错的本意。

### Phase 7（多跳路径唯一性）交付记录（2026-10-01）
- **多跳路径模式的 TRAIL/ACYCLIC 落地**：GQL 路径模式约束的是**整条路径**（ISO 20.5：
  TRAIL=全边互异、ACYCLIC=全节点互异）。引擎递归类型只管单个 var-length 槽，故翻译层
  对其表达不了的场景**绑定路径变量 + `IS_TRAIL`/`IS_ACYCLIC` 全路径谓词**（探针实证：
  路径值 nodes 含首尾各一次，IS_TRAIL=全 rel internalID 互异、IS_ACYCLIC=全节点互异，
  固定/变长段一致——正是 GQL 语义）。具体映射：
  - TRAIL：单 var-length 槽 → `*TRAIL`（已精确）；**多跳（≥2 边）→ `p = ... WHERE IS_TRAIL(p)`**；
  - ACYCLIC：**一律加 wrap**（引擎 `*ACYCLIC` 只约束中间节点，首尾自由——闭合走 1→2→1
    会漏进结果），var-length 槽另注 `*ACYCLIC` 做预过滤；
  - 路径变量：用户声明的 `p =` 复用其名；否则自动生成 `_gql_pp{N}`（每语句计数）；
  - WALK/无前缀不变（引擎默认=可重复边）；多跳 SHORTEST/SIMPLE 仍显式拒绝。
- **ACYCLIC 语义从近似升级为精确**：旧口径只用 `*ACYCLIC`（README #7 记近似），现
  闭合走/自环被正确排除（`AcyclicModeParity` 期望更新：2 跳 1→2→1、3 跳 1→2→1→2 均 0 行；
  单边自环 ACYCLIC 0 行 / TRAIL 1 行）。README #7 改写为精确语义说明。
- **新测试**：`MultiHopTrailParity`（自环双边重走/4 跳重边被滤、WALK 对照保留）、
  `MultiHopAcyclicParity`（自环/闭合走被滤、TRAIL 对照保留、固定+量词混合段双跑）。
  GQL 与等价 Cypher（`p = ... WHERE IS_TRAIL(p)` 形式）双跑一致，共 83/83。
- **实证备忘**：CREATE 链不按主键合并节点（`(:P{v:1})-[]->(:P{v:1})` 报重复主键）——
  环/重边要 `MATCH ... CREATE` 分句造；`ORDER BY 别名` 若与节点变量同名会解析到节点
  （报 Order by NODE is not supported），测试里别名避开模式变量名。
- **多标签标签表达式（G074 `&`/`|`/`!`）落地**：节点 pattern 的复合标签表达式翻译成
  `labels(v)` 上的 WHERE 谓词，**形态按图型分流**——ANY 图 `list_contains(labels(v),'A')`
  （labels() 返回 STRING[] 数组），表图 `labels(v) = 'A'`（labels() 返回标量表名）。
  简单标签保持 pattern 拼写 `:A`（表图保留表级裁剪）。**绝不复用 `:A:B` 拼写**：ANY 图它是
  AND（list_contains 合取）、表图它是 OR（表并集）——同拼写反语义，盲用=静默错答案。
  - INSERT 标签集（labelSetSpecification 文法本身就是 `A&B` 合取名集合）：单标签透传；
    多标签 ANY 图改写 `:A:B`（实证 CREATE 会设双标签）；表图**响亮拒绝**（引擎
    `CREATE (n:A:B)` 静默零行）；边 pattern 多标签拒绝。
  - 仍拒：`%` 通配、边标签表达式、WHERE 的 `IS [NOT] LABELED` 谓词（walker 显式拒绝）。
  - 图型判定：`GraphCatalogEntry::isAnyGraphType()`（具名图）/ `getDefaultGraphCatalog()`
    非空且含 `_nodes` 表（当前图；**main 时返回 nullptr = 表图**）。语句含多图目标且
    类型不一致/不可解析时复合标签响亮拒绝。`labels(n)` 在 ANY 图对无标签 pattern 也正确
    （节点属性全量预绑定，label 属性即 STRING[] 列）。
  - **译码坑（已修）**：splice 改写用 ANTLR 绝对下标，必须先减去 pattern 起点再作用于
    源文本子串；探机别忘 `load extension`（否则 "function GQL does not exist" 误判）。
- **图类型注册表 WAL 持久化——评估后暂缓**：扩展加载本身不跨重启
  （`loadLinkedExtensions` 只加载内建扩展，LOAD EXTENSION 无持久化钩子），注册表跟
  ExtensionManager 进程生命周期一致；要真持久得先做"扩展附着持久化 + 扩展数据 checkpoint"
  两项引擎工程（WAL typespec 新记录 + replayer + metadata 序列化）。当前失败面是响亮的
  （重启后 CREATE GRAPH TYPE 重跑即可），按够用即可原则推迟，勿重复评估。

### 评估日修正（2026-10-01）
- **G115 PROPERTY_EXISTS 矩阵虚标消灭**：README 原记 ✓（"parsed as-is"），实际原样透传到
  Cypher 后执行时死在 `function PROPERTY_EXISTS does not exist`（transformer/Cypher.g4/binder
  三处 0 支持）。已按 Neo4j `PropertyExistsToIsNotNull`（Apache-2.0）抄写改译
  `PROPERTY_EXISTS(v, prop)` → `(v.prop IS NOT NULL)`（`mapIdentifiersInto` 内，括号包裹防
  NOT/AND 优先级意外；与 REMOVE≈SET NULL 同一固定 schema 模型），select.test 增
  `PropertyExistsParity` 双跑。教训：矩阵里"parsed as-is"式 ✓ 必须过执行验证。
- 参考资料（Neo4j .scala / Cypher25Parser.g4 / ISO BNF）整理入 **`docs/gql_ref/`**（索引 README，
  ISO BNF 不入库）；成果/缺点评估成文 **`docs/gql_compat_review.md`**。
  **外部咨询简报 `docs/gql_consult_brief.md`**（残留问题 + 关联资讯 + 7 个开放问题，带去外部分析用；
  含 ANY 图 JSON 属性根因、TCK 失败构成勘误）。
- **#13 误读修正（外部分析触发，2026-10-01）**：外部分析把 README #13 读成"输入 `:A:B` 会按图型
  静默翻转求值"（ANY=AND / 表图=OR），建议加运行时提示、归入 #14、只出 `&|!`。对照两份文法原件 +
  代码复核后确认**前提不成立**：`:A:B` 在输入侧就是 GQL 语法错——ISO `<label conjunction>` 只有
  `&`、filler 单 `<is label expression>` 槽；GQL.g4 同构（`labelExpression` 仅 `&|!`、filler 单
  `isLabelExpression`、`labelSetSpecification : labelName (AMPERSAND labelName)*`），parse 层即响亮
  拒绝（`getNumberOfSyntaxErrors` → Failed to parse）。发射侧 match/谓词位从不生成 `:` 链（复合表达式
  只从 `&|!` 渲染 WHERE 谓词，运算符语义图型无关：`&`=AND、`|`=OR 两侧一致）；唯一 `:A:B` 发射是
  INSERT 创建位（仅 ANY 图、设双标签=事实非谓词）。三方分叉（ISO/GQL.g4=语法错、Cypher 25=AND、
  引擎表图=OR）全是**目标方言侧**事实。处置：#13 成因句改写为"三方分叉 + 输入侧命运 + 发射侧保证"；
  labels.test 增 `ColonLabelChainNotGqlSyntax` 钉住输入侧拒绝（MATCH/INSERT 各一）。分析里 `%`
  通配"翻译无坑"也不准——文法一致但本层仍响亮拒（unsupported）。教训：差异条目凡涉及"同拼写反语义"，
  必须写明病灶在**输入侧还是目标侧**，否则读者会脑补出不存在的静默执行路径。

### Phase 8（Q1 全序聚合）交付记录（2026-10-01，外部咨询驱动 + 3 subagent 并行实现）

- **扩展聚合三件套**（`extension/gql/src/function/gql_json_functions.{h,cpp}`，注册于
  `gql_extension.cpp`）：`_GQL_TO_JSON(ANY)→JSON` 标量；`_GQL_MAX`/`_GQL_MIN` 聚合（ANY 参数 +
  bindFunc 钉返回类型=输入类型，**isDistinct 双重载必配**）。**仓内首个扩展聚合**——走
  `extension::addFunc` 公开模板 + `AGGREGATE_FUNCTION_ENTRY`（与内建聚合同一 FunctionCatalogEntry
  路径，无需引擎改动）。state 平凡析构 tagged blob + overflow buffer（抄 min_max.h）。
- **全序实现**：JSON 值按 `null < bool < array < string < number < object`（**TCK [11][12] 钉的**，
  数组/字符串/数字三类与 CIP2016-06-14 相反——TCK 是可执行规范）；数字跨 INT/DOUBLE 按数值、
  列表字典序（短前缀小）、串字节序；不可比对（如两个结构不同的 JSON object）**响亮抛**。
  胜出元素保原类型（`max([1,2.0,5])`=INT `5`）。
- **翻译层**（gql_transformer.cpp）：`translateForStatement` 对**异构字面量**或**元素全为列表的
  字面量**逐元素包 `_gql_to_json`（字面量 null 保持裸 null）；`scanValueShapes` 对 FOR 源子树豁免
  异构拒绝，但发射前红线复查——包装元素里嵌混合列表仍拒（`_gql_to_json([1,'a'])` 会先被引擎
  同构化=静默错，必须拒）。`max(`/`min(` → `_gql_max(`/`_gql_min(` 在 finishExpr 漏斗做
  call-position 文本 splice（保护 `n.max` 属性、别名、字面量；GQL.g4 只有 MAX/MIN 拼写）；
  **别名安全**：`RETURN max(x)` 列名仍是 `` `max(x)` ``（GQL 源文本惯例）。
- **harness 裸数值归一**（test_runner.cpp `canonicalizeBareNumber`）：`0.100000`→`0.1`、
  `5.000000`→`5.0`（**int `5` vs real `5.0` 严格区分**）——解 TCK 转换器 `%.6f` 与 JSON 短打印的
  错位；两侧同纯函数归一不破坏任何现有用例。
- **TCK 翻绿 6 例**：Aggregation2 [5][6] 混合数值、[9][10] 列表值、[11][12] 混合值。
  剩 24 挂 = rejected 19（SCHEMA 13 含事务包裹 1、AS COPY OF 2、多标签 2、qualified 1）+
  parse-error 4（语料 3 + 文法歧义 1）+ other 1（`sum(p.age)` ANY 图 JSON 属性——**Q2 未做**）。
- 实现分工：3 个 general-purpose subagent 并行（函数/翻译/harness+测试），集成编译一次过、
  92/92 首跑全绿。测试包纠正了任务书笔误（字符串 max 应为 `c`——TCK [8] 钉代码序）。
- **勿重复调研**：扩展聚合注册链/API 面见 `docs/gql_consult_brief.md` §8（双 subagent 核验）。

### Phase 9（Q2 聚合桥）交付记录（2026-10-01，4 subagent 并行 + 集成收网）

- **扩展聚合 `_GQL_SUM`/`_GQL_AVG`**（gql_json_functions.{h,cpp}，+407 行）：ANY 参数 +
  bindFunc 钉型——整数族 SUM→INT128/UINT128、浮点→DOUBLE、AVG 恒 DOUBLE（与原生
  `appendSumOrAvgFuncs` 对齐）；**JSON 进 JSON 出保型**（SUM 全整数→`75`、含实数→`3.5`/
  整值 real 补 `.0`；AVG 恒 real `2.0`）。multiplicity 循环加（抄 SumFunction）；空组/全 NULL
  →NULL（AggregateStateWithNull，非 Cypher 的 0）；跳过 SQL NULL 与 JSON `null`；非数值/
  非有限→响亮抛。**bindFunc 同样钉参数类型**（ANY 占位延迟），SERIAL 纳入、DECIMAL 拒
  （对齐原生注册表）。DISTINCT 去重在 executor（distinct hash table），state 不去重、
  仅注册 isDistinct 双重载。
- **翻译 splice 扩容**（gql_transformer.cpp）：`rewriteMaxMinAggCalls`→`rewriteGqlAggCalls`，
  call-position 词表 `max|min|sum|avg` → `_gql_*`（iequals，字面量/词边界/点号属性安全）；
  列名保护链验证无污染（`` RETURN _gql_sum(p.age) AS `sum(p.age)` ``）。
- **jsonagg.test 9 组双跑**：含**与 native SUM/AVG 直接对拍**（typed 显示/空组 NULL 一致性的
  硬证明）、笛卡尔乘数钉（(33+42)*3=225）、JSON 保型显示钉（`3` 非 `3.0`）、非数值响亮拒钉。
  首跑全绿。
- **TCK 171→172**：Aggregation3 [1]（`sum(p.age)` ANY 图 JSON 属性）翻绿。语料期望列名
  自相矛盾（`n.name|sum(n.num)` vs 查询 `p.name, sum(p.age)`——无实现能过列名校验）：
  **不改 vendored 语料**，run_tck.py 加 `HEADER_DRIFT_SCENARIOS` values-only 例外 + REPORT
  methodology 脚注透明披露。失败构成 23 = rejected 19 + parse-error 4 + **other 0**。
- **红线探机结论（重要，勿重复探）**：ANY 图 JSON 属性的**比较/排序=文本序=静默错**
  （`>=100` 命中 33/9/33.5；ORDER BY 出 10,100,33,33.5,9；`33 = 33.0` False）；算术**无静默错**
  （按另一侧类型数值化，不匹配响亮 ConversionException，如 `"33.5"→INT64`）；CAST 不静默截断。
  属性相等是 `EQUALS(prop, CAST(literal, JSON))` 文本等（同形字面量碰巧对）。
  **粗粒度响亮拒比较被 TCK 约束排除**（TCK expressions 149 场景跑在 ANY 图上、靠幸运文本序全绿，
  一刀切拒会砍掉 ~50 绿场景）——修法必须是语义正确的全序比较桥，分叉与咨询中，
  见 `docs/gql_consult_q2.md` Q2-A（六算子 `_gql_gt` 系 + ORDER BY 多键 rank/numkey/strkey）。
  README 已记差异 #21（红线缺口显式挂账）。
- **探机工具坑（二连撞）**：subagent 连续两次撞 32000 output token 上限（单次大 Write/大报告）；
  任务书要钉「小步写、报告 ≤40 行」。e2e 测试文件名**下划线开头不注册**（`_q2probe.test`
  0 tests，改名 `q2probe.test` 才进组）；GQL **无 `IN` 谓词**（`WHERE x IN [...]` parse 拒）；
  GQL INSERT **同变量名多 pattern 会合并成单节点**（六节点探机只活一个——测试造数必须变量互异）。

### Phase 10（Q2-A 比较/排序全序桥）交付记录（2026-10-02，外部咨询回帖 + 验证 + 3 subagent 并行 + 集成收网）

- **前置**：咨询回帖 `docs/gql_consult_q2_reply.md` 经我方独立验证
  （`docs/gql_consult_q2_reply_verify.md`：探机表全复现、B1→B2 推荐序成立、裸文本存储等修正），
  按修正后计划执行。B1 commit `c894d91`、B2 commit `e7c51a9`。
- **扩展函数**（gql_json_functions.{h,cpp}）：`_GQL_LT/LE/GT/GE/EQ/NE`(ANY,ANY→BOOL) +
  `_GQL_SORTKEY`(ANY→STRING)。操作数按 LogicalType 分类：JSON 解析成功→按值归类、
  **解析失败→裸字符串**（属性写入器存 `x` 不带引号、`_gql_to_json` 产 `"x"`，两形态同列共存）；
  引擎 BOOL→JSON 拼写 `True`/`False` 特判 bool（GQL INSERT 经 `_gql_to_json` 存小写 `true`/
  `false`——两拼写都归 bool）。数字=**精确十进制**文本比较（任意长、零 double 兜底——
  对 compareJsonNumbers 的关键升级；>2^53 有测试钉）。SQL NULL→NULL（三值，NOT(NULL)=NULL
  引擎原生）。对象异内容/DATE/UUID/INTERVAL/SERIAL/DECIMAL/STRUCT/MAP/非有限 real→响亮拒。
  sortkey：rank 前缀 a/b/c/d/e + 数组 **FDB tuple 式自定界**（0x00 终结+00 FF 转义；8 位十六进制
  长度前缀会**静默错序**——任务书原方案有 bug，subagent 抓出并修正，Python 移植版 fuzz
  49,455 嵌套数组对 0 错）+ number=符号+8 位偏置指数+40 位尾数（负 9 补码、-0≡0、5≡5.0 同键）。
- **翻译 splice**（gql_transformer.cpp）：`emitValueExpression`/`emitExpr` 在
  `ComparisonExprAltContext`（GQLParser.h:7561，left/compOp/right 子树天然切分）按
  `labelGraphIsAny==true` 才改写（typed/nullopt 快路=sourceText 逐字节不变）；`=`/`<>` 由
  `kSpliceEquality` 门控（B2 翻 true；门禁=语料分叉清单——vendored TCK 全部 `=`/`<>` 是
  bool 定律（Boolean1-5）或裸串=串字面量（Boolean4 [1]），清单干净）。ORDER BY 键**别名映射后**
  包 `_gql_sortkey`（renderOrderBy，ASC/DESC/NULLS 后缀保留）。接线全部查询路径
  finishExpr 站点；FOR 源/pattern 属性表/显式 GROUP BY key/OFFSET-LIMIT/SET RHS 不接（有据）。
- **comparebridge.test 11 组双跑**：哨兵序算子（>30→{33,33.5,100}、<30→{9,'x'}）、跨类序、
  三值 NULL/NOT、typed 字面量、sortkey ASC/DESC/多键、2^53 精度、bool rank、等值桥
  （33=33.0 语义真、<>）、数组字典序、object 响亮拒。首跑 10/11（BoolRank 显示大小写），
  修后 11/11。
- **红线缺口关闭**：README #21 由「挂账缺口」改写为「已修+已知残余」（CASE WHEN 简写比较/
  聚合实参内比较不 splice；DESC 下 NULL 排最前=引擎惯例；数组内 >int64 的 int vs real 谓词
  仍走 Phase 8 double 比较器；裸文本恰巧拼成 JSON（字符串 'true'）是存储消型的内在不歧义，
  parse-first 规则胜出）。
- **战果**：自测 **101→112**（comparebridge 11 新增）、TCK **172/23/11 零回退**（B1/B2 两刀
  分别验证）；B2 门禁清单提前扫过=干净，B2 与 B1 同阶段落地。
- **咨询验证自查纠错（重要）**：验证报告曾判「149 boolean 场景」为笔误（grep Scenario 数出
  36）——**误判**：149 是 run_tck 展开后的**生成用例数**（Boolean1-5:30+30+30+51+8），
  场景级 36 是另一口径。两口径都对，勿再混。验证报告修正 3 已撤回并注明。

### Phase 11（Q3 SCHEMA 命名空间 + GQLSTATUS 贴码）交付记录（2026-10-02，Q3 咨询回帖 + 验证 + 2 subagent 并行 + 主会话集成）

- **驱动链**：Q3 简报（62a53a9）→ 外部回帖 → 独立验证（513e9b1，`docs/gql_consult_q3_reply_verify.md`）：
  ① 回帖事实勘误**成立**——简报事实 #3 错误，语料**含 20 处 GQLSTATUS 码断言**
  （42000×16、25G03、22G0N、22G0P、G2000），是 run_tck.py 主动丢弃码断言；Q3-A 由缓做转做。
  ② 验证给回帖 4 处修正：G2000 脚枪（无差别贴 42000 会砸 Create2 [7] 绿场景→贴码面收窄）、
  [8] 光贴码转不绿（when_exception 前导 OK 期望 + START/COMMIT 切分→需 harness 整体化）、
  [9] 本已绿（NEXT 不切分）、22G0N/22G0P 在跳过场景无执行契约（"20 处升级"高估≈13 处）。
- **产品侧（subagent + 主会话补自测）**：`SchemaCatalog`（ExtensionManager data 槽同址，
  逻辑路径集合 + 目录=非空真前缀 + 逻辑↔物理双向映射，物理名 `_gqlsch__`+段拼 `__`）；
  CREATE/DROP SCHEMA（IF [NOT] EXISTS、九错条件）→ 注册表变更 + EMPTY_RESULT_CYPHER；
  限定名改写（CREATE GRAPH/GRAPH TYPE、DROP、SESSION SET GRAPH；USE GRAPH 裸 `/path` 是
  **GQL.g4 文法面**不收，SESSION SET GRAPH 的 graphExpression 收——测试用后者）；
  **收窄贴码**：schemaError→`[42000] `、READ ONLY 事务包裹→`[25G03] `、其余含 AS COPY OF
  一律无码（G2000 脚枪规避）；非 READ ONLY 事务包裹消息一字未改（unsupported.test 钉子）；
  `_gql_schemas()->STRING`（`{"schemas":[...],"directories":[...]}` 字典序，契约供 harness）。
- **harness 侧（subagent）**：run_tck.py——when_exception 整段单 CALL GQL（修 [8] 分类）；
  **三档码断言**（码正则→过 / 无括号码→passed-with-note / 异码→failed wrong-GQLSTATUS，
  分类器带"首失败守卫"，5/5 单测用真实 MSVC gtest 块格式）；catalog-1.gql fixture
  （补语料引用的缺失输入数据 + 能力探测）；±schemas/±directories 经 `_gql_schemas()` 对拍
  harness 迷你模型（真校验，非 unchecked）；Create1 [7] 语料自相矛盾（When 漏 IF NOT EXISTS
  而标题/期望要）走 allowlist 重释 + 脚注（Aggregation3 values-only 同款先例）。
- **战果**：自测 **112→122**（schemapath 10）、TCK **172→188 绿**（68 过 + 120 note）/
  9 挂 / 9 跳，wrong-GQLSTATUS=0；9 挂=语料 parse 4 + 多标签 2 + LIKE 1 + AS COPY OF 2（全部响亮，
  含预期账：Create2 [4] 改名通后死在 LIKE、仍红）。
- **残余（README #22）**：注册表不落 WAL；相对限定名（dir.name）不支持。

### Q4 轮（Q-F/Q-G/Q-E3 落地）交付记录（2026-10-02，外部回帖 + 验证 + 1 subagent + 主会话集成）

- **Q-F 三层反转（头条）**：回帖文法读对（`useGraphClause : USE graphExpression`，USE 后无 GRAPH
  关键字；GRAPH 在 nonReservedWords 会被当图名吃掉），但「USE /path 今天就能跑（独立语句）」被探机
  **证伪**——useGraphClause 是查询/数据修改语句的**前缀从句**（GQL.g4:382-386/:539-551），独立 USE
  构不成 statement；我方原「名位不收 /path」机制归因也错（名位经 graphReference→…→absoluteDirectoryPath
  收 /path）。**真形态已通**：`USE /foo/g MATCH …`（+双跑用例）；`SESSION SET GRAPH /path` 为会话拼写；
  `USE GRAPH x` 勿写（GRAPH 被当图名）；不做 USE GRAPH x→USE x 宽容（会吃掉名叫 graph 的合法图）。
- **Q-G**：CREATE/DROP SCHEMA 的「名字是图」冲突检查并查引擎 catalog（`getGraphEntries` 点查物理名
  +根路径裸名），层外建图盲区闭合（schemapath EngineGraphConflict 用例钉）。
- **Q-E3**：run_tck 模板替换 `$(randomLabelSet(minNodeLabels-1|+1))`→空串/`:L0&L1`（cardinality 1/1
  脚注），Given「randomly generated label set」no-op；graph-types [7][8] 2 跳→2 跑。
- **贴码延伸**：匿名节点类型→`[22G0N]`、多标签→`[22G0P]`（语义与 [7][8] 钉码相符），[7][8] 升真契约。
- **新问题语料第 3 例（三档机制在线上抓到）**：graph-types [6] 题名「重复属性名」但 body 多标签+属性
  无一重复（[4] body copy-paste），钉 42000 与实际拒因 22G0P 冲突→wrong-gqlstatus 挂；按先例软化档
  （该场景码检查降 note）+ 脚注保绿。**wrong-GQLSTATUS 首次线上生效=抓真 bug 的证据**。
- **战果**：自测 **122→124**（USE 前缀从句、层外图冲突）；TCK **188→190 绿（70 过+120 note）/ 9 挂 / 7 跳**，
  wrong-GQLSTATUS=0。

### Q5 轮（Q5-2/3/4 落地 + Q5-1 挂账）交付记录（2026-10-02，Q5 简报 + 回帖验证 + 2 subagent 串行 + 主会话集成）

- **Q5-2① 证伪修正（头条）**：回帖称「SIMPLE=裸引擎 `*ACYCLIC` 零包装」，探机**证伪**——
  `*ACYCLIC` 只保中间点两两互异（m0→m1→m0→m2 放行），ISO SIMPLE 禁止该行走；照抄=静默错答。
  修正切片落地：`_gql_is_simple(ANY)→BOOL`（RECURSIVE_REL 取 nodeIDs，节点互异、仅首尾可重合）
  + `*ACYCLIC` 超集预过滤 + WHERE wrap；红线钉 m0→m1→m0→m2=0 行、裸 `*ACYCLIC`=1 行（证明 wrap 必要）。
- **Q5-2②③④**：DIFFERENT EDGES 单模式走 TRAIL 机器（forcedRecType）、多模式响亮拒；REPEATABLE
  ELEMENTS 本就 no-op（零开发补钉）；IS [NOT] LABELED 接标签谓词出口（IS NOT 整体取反）、
  `%`=表图恒真/ANY `size(labels)>0`。
- **Q5-3 bind 期守卫（红线账清零）**：`_gql_list_checked(ANY...)→LIST` 变长标量，bindFunc 在
  **隐式 cast 之前**逐实参查引擎推导型（类不一致响亮抛 `heterogeneous list element types`）；
  typed 图含 ≥1 非字面量元素的列表字面量才改写，ANY/nullopt 免检、FOR 源不动（Phase 8 覆盖）。
  残余（非静默）：聚合实参内列表不包、ORDER BY/SET 属性值走 sourceText。做完永久关账。
- **Q5-4 相对限定名**：`dir.name` 根解析（SESSION SET SCHEMA 已拒=会话 schema 恒 root ⇒ 精确非近似），
  点分段 ANTLR 子树拼 `/seg1/seg2` 走现有 mangling，7 处接线；旧措辞
  `qualified graph name (schemas are not mapped)` 消除；`/dir/x.g` 归一 `/dir/x/g`（拼写变化）。
- **Q5-1 多跳 QPPI 挂账**：无界=表达力天花板永拒；有界 `{m,n}` UNION 展开子集备好（n-m≤4），
  触发条件=首个真实需求；SurrealDB gql 模块 **BSL 1.1 不可抄**。
- **战果**：自测 **124→137**（smallmodes 8 + listguard 3 + relname 3，净 +13）；TCK **190 绿不变**
  /9 挂/7 跳（语料压力 0），wrong-GQLSTATUS=0。2 subagent 串行（同触 gql_transformer.cpp 不并行）。

### Phase 4（schema 桥）交付记录（2026-10-01）
- **CREATE GRAPH TYPE → node/rel table DDL**：图类型规范化为
  `GraphTypeSpec`（节点类型 = 名字+属性类型；边类型 = 名字+端点对+属性类型，别名剥除——
  抄 Neo4j `GraphTypeCanonicalizer.scala` 规范形，Apache-2.0 已署名登记）。
  `CREATE GRAPH g t` / `CREATE GRAPH g { … }` 展开为
  `CREATE GRAPH g; USE GRAPH g; CREATE NODE TABLE …; CREATE REL TABLE …` 多语句。
  拼写实证来自 opengql/tck：`(Person :Person {…})`、`(Person)-[:KNOWS]->(Person)`、
  `CREATE GRAPH mygraph mygraphtype`（**图类型引用是裸名字**，无 TYPE 关键字）。
- **合成主键 `_gql_id SERIAL PRIMARY KEY`**：GQL 节点类型无键、Ladybug 节点表必须有主键；
  INSERT 自动填充。`_ID` 是引擎保留字（property lookup 隐藏）故不能用 `_id`。
- **图类型注册表在扩展进程内存**（Ladybug 无 graph-type catalog 对象）：同进程跨 CALL/跨库
  存活，进程重启丢失（README 已记）。CREATE/IF NOT EXISTS/OR REPLACE/COPY OF/DROP 语义齐全。
- **引擎侧修复（重要）**：扩展函数只注册进 main catalog，`USE GRAPH` 到别的图后
  `CALL GQL` 报 "function GQL does not exist"。修复：`Catalog::setFunctionFallback`——
  图 catalog 函数查找回退 main catalog（`catalog.h/.cpp` + `database_manager.cpp` 两处挂钩）。
- **GQL 语言事实（勿重复踩）**：
  - **属性名 `at` 是关键字**（AT SCHEMA）——`{at INT64}`/`e.at` 都是语法错，测试用 `ts`。
  - **没有裸 `TIME` 类型**（只有 LOCAL TIME / ZONED TIME / TIME WITH|WITHOUT TIME ZONE）；
    Ladybug 无 TIME 类型，全部显式拒绝。
  - **`CREATE GRAPH TYPE t AS COPY OF u` 文法歧义**：TYPE 是 nonReservedWord，会被
    解析成"名为 TYPE 的图"（ANTLR 取 createGraphStatement 优先）——已在 transformer
    检测误解析并重解释为 CREATE GRAPH TYPE（否则会静默建出名为 TYPE 的图！）。
  - ISO 引用图类型用裸名 `CREATE GRAPH g t`；工具常写 `CREATE GRAPH g TYPE t`——
    预解析宽容归一化删掉 TYPE 一个词（同 FROM GRAPH 套路，`normalizeCreateGraphTypeRef`）。
  - GQL 图类型支持多标签节点类型（`:A&B`）——Ladybug 单标签，显式拒绝；NOT NULL 静默丢弃。
- 测试拼写注意：edgeTypePhrase 的 `DIRECTED|UNDIRECTED` **强制**（`REL KNOWS CONNECTING (…)`
  无 DIRECTED 是语法错；`REL` 也不是合法 edgeSynonym，只用 EDGE/RELATIONSHIP）。

### Phase 3（路径模式）交付记录（2026-10-01）
- **量词映射**（bounds 抄 Neo4j `AddElementUniquenessPredicates.getLowerBound/UpperBound`）：
  GQL `*`→`[e*0..]`（**0 起跳**，裸 Cypher `*` 是 1..！）、`+`→`*1..`、`{n}`→`*n`、`{m,n}`→`*m..n`、
  `{m,}`→`*m..`、`?`→`*0..1`。单边量词与单跳 QPPI `( ()-[]->() ){m,n}` 都落到递归关系槽。
- **路径模式/搜索前缀** → `iC_RecursiveType`：TRAIL/ACYCLIC 直落；WALK=引擎默认（省略）；
  `ANY SHORTEST`→`*SHORTEST`、`ALL SHORTEST`→`*ALL SHORTEST`（SHORTEST 自动是 trail/acyclic，
  附带 mode 丢弃）。SIMPLE、`SHORTEST k`、`SHORTEST GROUP(S)`、`ALL/ANY PATHS`、多跳+模式 → 显式报错。
- **模式翻译是结构化的**（`translatePathTerm` flatten→节点/边事件→链发射），不再是整段透传；
  无向/混向边 `~[e]~` 等折叠为 `-[e]-`；`IS Label`→`:Label`；元素内联 WHERE 提升到语句 WHERE；
  MATCH/SELECT 的 WHERE 合并为单条（修双 WHERE 拼接隐患）。
- **引擎语义实证**（探针验证，勿重复实验）：Ladybug MATCH **允许边重复**（≈GQL REPEATABLE
  ELEMENTS/WALK，故 DIFFERENT EDGES 拒绝、REPEATABLE 丢弃）；`*0..` 下界 0 可用（0 跳行 start=end）；
  `*TRAIL` = 边互异（与 GQL 一致）；**`*ACYCLIC` = 仅中间节点互异**（首尾不受限，闭合行走得通——
  与 GQL 全节点互异有差；Phase 7 起翻译层用 `IS_ACYCLIC(p)` wrap 补成精确，见上）；
  SHORTEST/ALL_SHORTEST 要求 lower=1。
- **GQL 语言事实**：ISO GQL **无 `LENGTH()`**（只有 PATH_LENGTH/CHAR_LENGTH 等 lengthExpression），
  `length(e)` 在 GQL 层就是语法错；GQL 也**无裸 `SHORTEST` 前缀**（只有 ANY/ALL SHORTEST 或 counted）。
- 引擎侧小改进：`gql_function.cpp` 解析错误现在带 ANTLR 明细（`Failed to parse ... (line 1:8 ...)`）。
- 路径量词/唯一性参考了 Neo4j `AddPathPredicates`/`AddElementUniquenessPredicates`/
  `AddVarLengthBoundPredicates`/`QuantifiedPathPatternConverters`（**查阅未留档**，
  上游路径见 `extension/gql/THIRD_PARTY_NOTICES.md` 与 `docs/gql_ref/README.md` 的
  "Consulted but not kept"；要抄细节时按那里的路径重取）。

## 四、关键事实备忘（调研结论，勿重复调研）

- **GQL.g4（vendored opengql，3774 行）语法已齐**：SELECT/GROUP BY/HAVING/DELETE/SET/REMOVE/
  FILTER/FOR/QPPI/事务/会话全可 parse——**瓶颈全在 transformer，不改语法不换 parser**。
- GQL.g4 **无**：MERGE（非 ISO GQL！Cypher 扩展）、FETCH（分页是 LIMIT/OFFSET）、REPEAT…UNTIL。
- ISO 标准 GROUP BY 分组键**只允许绑定变量**（`GROUP BY n`），表达式分组靠 SELECT 列表隐式分组。
- Cypher.g4 **无 REMOVE 规则**；有 `iC_Transaction`（BEGIN TRANSACTION，无裸 BEGIN）、
  `iC_RecursiveType`（SHORTEST/TRAIL/ACYCLIC/WSHORTEST）、USE/DROP/CREATE GRAPH 原生 DDL。
- CREATE GRAPH（GQL）**强制要求类型子句**：`CREATE GRAPH g ANY` 或 type spec——裸 `CREATE GRAPH g` 不是合法 GQL。
- `CALL GQL` 执行链：bindFunc（ANTLR 解析+翻译）→ rewriteFunc（整句替换为 Cypher 文本）→
  ClientContext 重解析执行；**只有最后一条 Cypher 语句的结果对外可见**（事务包裹体因此被拒）。
- ANTLR 锁 4.13.1（4.13.2 parse-tree 层级不兼容会 downCast 崩）。
- Ladybug 函数目录：UPPER/LOWER/CEILING/LN/COLLECT/PERCENTILECONT/PERCENTILEDISC/SIZE/LENGTH
  （路径）/internal_id/TIMESTAMP/DATE/DURATION 均内建；**无 STDDEV、无 TIME 类型**（LOCAL_TIME/ZONED_TIME 显式拒）。
- 测试框架：`.test` 声明式（`-CASE`/`-STATEMENT`/`---- ok|N|error|error(regex)`/`-CHECK_ORDER`），
  组名 = 路径 `~` 连接（`gql~test~test_files~select`）；gtest_filter 用这个。
- 外部参考已整理入 **`docs/gql_ref/`**（含索引 README）：Neo4j 4 个 astRewriter .scala
  （函数别名表/标签表达式归一/PROPERTY_EXISTS）、`GraphTypeCanonicalizer.scala`、
  ISO `ISO_IEC_39075.bnf.txt`、`Cypher25Parser.g4`。Neo4j GQL 合规附录（网页）是语义
  差异清单的权威参考。

## 五、未完成待办（截至 2026-10-02 Q5 收工时点）

> Phase 0–11 + Q4 + Q5 已交付面见上面各阶段记录。下面只列**尚未做**的账。
> 已交付勿再列：多跳 TRAIL/ACYCLIC（P7）、标签表达式（P7）、跨类型全序 min/max（P8）、
> 聚合桥 sum/avg（P9）、比较/排序全序桥（P10）、GQLSTATUS 窄贴码+三档断言（P11）、
> SCHEMA 命名空间注册表（P11）、USE 前缀从句/层外图冲突/模板替换（Q4）、
> SIMPLE/DIFFERENT EDGES/REPEATABLE/IS LABELED/`%`（Q5-2）、bind 期异构列表守卫（Q5-3）、
> 相对限定名根解析（Q5-4）。

### 可做未做（按需排期，今天响亮拒绝）

1. **多跳 QPPI**（`( ()-[]->()-[]->() ){m,n}`）：挂账等触发（触发条件=首个真实需求）。
   设计备好：有界 `{m,n}` UNION 展开是唯一可行 Cypher 侧子集（爆炸控制 n-m≤4、
   内部绑定展开后显式列表构造）；**无界 `{m,}`/`*` 多跳=表达力天花板，永久拒**。
   SurrealDB gql 模块是 **BSL 1.1 不可抄**；Neo4j front-end（Apache-2.0）QPP desugar
   只抄语义。
2. **原生 GQL 执行 / 双向互通**：大工程（≈重写半个 planner），远期选项，维持评估结论。

### 暂缓 / 挂账（有意不做，勿重复评估）

- **图类型 / schema 注册表 WAL 持久化**：暂缓——扩展加载本身不持久，前置依赖是
  「扩展附着持久化」引擎工程（P7 评估结论）。
- **Q-D typed-graph 异构列表残余：已由 Q5-3 关闭**（bind 期守卫，静默→响亮）。残余边界
  全非静默：聚合实参内列表不包（rewriter 在聚合边界止步）、ANY 图免检（Phase 8 路线）、
  ORDER BY/SET 属性值走 sourceText。
- **Q-E1 引擎错误文本映射 / Q-E2 AS COPY OF 贴码**：Q4 已决关闭——E1 不做（引擎错误
  文本照原样透传）、E2 维持无码（G2000 脚枪规避，见 P11 记录）。**勿重开**。
- **graph-types [6] 等问题语料**（4 例：Aggregation3 [1]、Create1 [7]、graph-types [6]、
  catalog-1 fixture）：语料 vendored 冻结不改，走 harness 例外 + REPORT 脚注披露（先例
  判定标准见第六节经验 3）。语料修复只能靠上游，本仓不改。
- **Q5-2 已知边界（未测/透传面）**：WHERE 中边变量 IS LABELED 无符号表可辨、行为取决于
  引擎 labels() 对关系的处理（未测）；CASE WHEN 的 whenOperand 直挂 labeledPredicatePart2
  仍透传到 Cypher 响亮失败。均为响亮面，不静默。

## 六、目标执行过程与问题处理经验（方法论沉淀，勿退行）

### 执行闭环（每轮照此，Q1→Q4 四轮验证有效）

简报（问题清单+可核事实）→ 外部 AI 回帖 → **独立验证再采纳**（探机复现 + 语料/源码交叉
核对，先纠错后采纳）→ 修正落地序 → 分片实现（subagent 小步写 + 主会话集成）→ 门禁
（`_build_gql.bat` + 自测全绿 + TCK 不降 + wrong-GQLSTATUS=0）→ 文档真相同步
（README / 本文件 / REPORT 脚注 / memory）→ **git 本地提交，绝不 push**。

### 问题处理经验（教训清单）

1. **归因必须过执行验证**——三次"读代码/读语法推断"都骗了人：G115 PROPERTY_EXISTS
   矩阵虚标（parsed-as-is 式 ✓ 未跑验证）；Phase 5「布尔不校验」翻案（实为 harness 正则
   bug）；USE「名位不收 /path」归因两轮皆错（我方一次、回帖一次，真因是前缀从句拼写）。
   探机复现才作数。
2. **回帖必先独立验证**——回帖也会错：Q3 简报事实 #3 自错（语料实含 20 处码断言）；
   Q4 回帖 Q-F 运营层断言错（USE 非独立语句）。验证轮的**修正比采纳更有价值**（Q3 的
   G2000 脚枪/[8] 半步/[9] 已绿、Q4 的三层反转，都是拦下的回归）。
3. **问题语料处置先例（4 例）**——判定标准：语料自相矛盾或输入缺失才算语料问题，
   产品缺口不算。处置固定三件套：**语料冻结不动 + harness 例外（values-only / When 重释
   allowlist / soft-code 降档 / fixture 补数）+ REPORT 脚注披露**。
4. **错码比无码更糟**——贴码面必须比语料钉码面窄或恰好（G2000 脚枪：无差别贴 42000 会
   砸 Create2 [7] 绿场景）。三档码断言（码匹配/无码 note/错码挂）既是保险也是探针：
   graph-types [6] 的 wrong-gqlstatus 现行犯就是它抓的。
5. **静默错红线贯穿**——0 silent wrong answers：新功能面先问"会不会静默错"（异构列表
   cast-erase、JSON 文本序比较都是线下抓的静默错）；宁响亮拒绝，不静默错答。
6. **subagent 纪律**——任务书钉死：单次 Write ≤400 行、报告 ≤40 行、**禁止粘贴全量测试
   输出**（32k output 上限两次击杀都是这个原因）；MSVC gtest 失败标记是 `(228): error:`。
7. **外部语义正确 ≠ 引擎映射正确**（Q5-2①）——回帖的 ISO SIMPLE 定义对、推理对（无需
   TRAIL 包装），但「引擎 `*ACYCLIC` = ISO SIMPLE」错：引擎只保中间点两两互异，端点可撞
   中间点，照抄会放出非 SIMPLE 行走=静默错答。四源互证管不到我方引擎侧；**引擎原语的
   精确语义必须本地探机**（本例 6 行探针拦下一个静默错答类，修正为
   `*ACYCLIC` 预过滤 + `_gql_is_simple` 谓词）。
