# GQL ↔ openCypher 衔接补全 — 交接文档（2026-10-01 Phase 3 收工存档）

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

**✅ 67/67 全绿**（2026-10-01 Phase 3 收工）：
basic 11（回归）/ select 6 / groupby 4 / write 7 / routing 4 /
**path 16**（QPPI/量词、WALK·TRAIL·ACYCLIC、ANY·ALL SHORTEST、无向边、IS 标签、内联 WHERE 提升）/
unsupported 19。

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

## 五、Phase 4+ 路线（Phase 3 已完成，后续未做）

CREATE GRAPH TYPE→NODE/REL TABLE schema 桥（抄 `GraphTypeCanonicalizer`）、
多跳路径模式的 TRAIL/ACYCLIC 唯一性谓词（抄 `AddElementUniquenessPredicates` 的
DifferentRelationships/NoneOfNodes 谓词生成——引擎无现成谓子，需评估）、
opengql/tck 通过率、原生执行/双向互通。
