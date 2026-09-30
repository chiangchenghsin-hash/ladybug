# GQL ↔ openCypher 衔接补全 — 交接文档（2026-09-30 收工存档）

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

## 三、当前测试战果（最后一轮）

**通过**：basic.* 全部回归绿（10 个）、unsupported 中 LET/NEXT/SET label/REMOVE label/
label 表达式/量词路径/事务包裹/MERGE-not-GQL、routing 中 DropGraphIfExists/TransactionRouting、
write 中多数（InsertLiteralSafety/Delete/DetachDelete/SetProperty/InsertThenMatch）。

**未过（根因已全部定位，修复方案明确）**：
1. **SELECT 全挂（parse error）**：ISO GQL 语法是 `FROM <图名> MATCH`（**无 GRAPH 关键字**，
   LDBC 官方示例 `FROM friends MATCH …`；`FROM CURRENT_GRAPH MATCH …`）。
   修复：(a) bindFunc 前做字面量安全的 `FROM GRAPH x`→`FROM x` 宽容归一化（AI 常写错）；
   (b) 测试改用 `FROM main MATCH` / `FROM CURRENT_GRAPH MATCH`，保留一条 `FROM GRAPH main` 测归一化。
   ——transformer 本身不用动（它接 selectGraphMatch）。
2. **聚合查询 ORDER BY 报 "Variable n is not in scope"**：Ladybug 聚合后 ORDER BY 只能引用投影别名。
   修复：隐式聚合 RETURN 形态下，把 ORDER BY 里与 select item 表达式相同的 span 换成其别名
   （WITH 形态已做，buildOrderPage(reps)；给 RETURN 形态补 item→alias 的 reps）。
   双跑 Cypher 期望也改成 `ORDER BY age`（别名）。
3. **SetSnapshotOrderParity 测试自身设计错**：双跑 Cypher 把已更新的行又改了一遍（得 99|99）。
   改两行数据（v:1 GQL 跑、v:2 Cypher 跑）再对比。
4. **布尔渲染是 `True` 不是 `true`**：RemovePropertyParity 期望改 `True`。
5. **unsupported 两条语法写错**：
   - CREATE GRAPH TYPE 要写 `{ NODE Person {name STRING} }`（elementTypeSpecification 形态），
     不是 `{name STRING}`；
   - `SESSION SET SCHEMA main` 过不了 parse（schemaReference 不是裸名）——查
     `relativeCatalogSchemaReference/predefinedSchemaReference` 后改写法，或改断言为
     error(regex) 接受 parse 错误。

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

## 五、Phase 3+ 路线（本期不做）

路径模式（QPPI/路径模式/搜索前缀映射，抄 Neo4j QuantifiedPathPattern* rewriter 套路）、
CREATE GRAPH TYPE→NODE/REL TABLE schema 桥、opengql/tck 通过率、原生执行/双向互通。
