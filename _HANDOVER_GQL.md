# GQL ↔ openCypher 衔接补全 — 交接文档（2026-10-01 Phase 5 收工存档）

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
- `_tmp_gql_ref/`：下载的参考代码（Neo4j 4 个 rewriter .scala、ISO 官方 BNF、Cypher25Parser.g4）——未入库。

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

**✅ 81/81 全绿**（2026-10-01 Phase 6 收工）：
basic 11（回归）/ select 6 / groupby 4 / write 7 / routing 4 / path 16 /
**schema 6**（CREATE GRAPH TYPE/typed CREATE GRAPH 双跑、注册表生命周期、TYPE 关键字宽容）/
**unsupported 27**（+map 值/异构列表字面量拒绝）。
**TCK 合规：206 场景 = 165 过 / 30 挂 / 11 跳**（Phase 6 口径，明细 `extension/gql/test/tck/REPORT.md`）。

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
  other 3（MIN/MAX/SUM 无列表重载，binder 响亮拒绝）。比计划 166 少 1：Agg2 [6] 旧口径
  "过"是巧合（min 的提升后结果恰与期望同字面），现被异构检查拦下——正是 0 静默错的本意。

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
  与 GQL 全节点互异有差，README 已记近似）；SHORTEST/ALL_SHORTEST 要求 lower=1。
- **GQL 语言事实**：ISO GQL **无 `LENGTH()`**（只有 PATH_LENGTH/CHAR_LENGTH 等 lengthExpression），
  `length(e)` 在 GQL 层就是语法错；GQL 也**无裸 `SHORTEST` 前缀**（只有 ANY/ALL SHORTEST 或 counted）。
- 引擎侧小改进：`gql_function.cpp` 解析错误现在带 ANTLR 明细（`Failed to parse ... (line 1:8 ...)`）。
- 参考代码新增 `_tmp_gql_ref/astRewriters/{AddPathPredicates,AddElementUniquenessPredicates,
  AddVarLengthBoundPredicates}.scala` + `ir/QuantifiedPathPatternConverters.scala`（未入库）。

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
- 外部参考（已下载 `_tmp_gql_ref/`）：Neo4j `GQLAliasFunctionNameRewriter.scala`（函数别名表）、
  `PropertyExistsToIsNotNull.scala`、ISO `ISO_IEC_39075.bnf.txt`、`Cypher25Parser.g4`。
  Neo4j GQL 合规附录（网页）是语义差异清单的权威参考。

## 五、Phase 6+ 路线（Phase 5 已完成，后续未做）

多跳路径模式的 TRAIL/ACYCLIC 唯一性谓词（抄 `AddElementUniquenessPredicates` 的
DifferentRelationships/NoneOfNodes 谓词生成——引擎无现成谓子，需评估）、
图类型注册表 WAL 持久化（现为每库内存）、布尔操作数类型校验（TCK 15 例）、
GQLSTATUS 错误码信封、原生执行/双向互通。
