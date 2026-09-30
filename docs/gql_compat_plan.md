# LadybugDB GQL 兼容层补全计划（Plan）

> 目标：把 `extension/gql` 从"薄翻译 + 子集透传"补成**诚实、可用的 ISO GQL（ISO/IEC 39075:2024）兼容层**，让按标准 GQL 生成代码的 AI / 外部工具能直接打 Ladybug。
> 动因：一门查询语言的价值越来越取决于"AI 能否生成它"；当前半吊子 `CALL GQL()` 制造"支持 GQL"错觉却对 SELECT / GROUP BY / 类型化图直接报错，比不做更糟。

---

## 0. 现状（问题定义，含源码证据）

| 项 | 现状 | 证据 |
|---|---|---|
| 主查询语言 | openCypher 谱系 + 私有 DDL，引擎母语 | `src/antlr4/Cypher.g4` |
| GQL 入口 | `CALL GQL("…")` 表函数 | `extension/gql/src/include/function/gql_function.h:9` (`name="GQL"`) |
| 解析 | 用 opengql 的 `GQL.g4` 生成 ANTLR C++ parser | `extension/third_party/opengql/GQL.g4` |
| 翻译 | `GqlToCypherTransformer` 把 GQL AST → Cypher 文本再执行 | `extension/gql/src/gql_transformer.cpp` |
| 已覆盖 | MATCH(透传) / INSERT→CREATE / CREATE GRAPH(剥修饰符) | `gql_transformer.cpp:54-139` |
| 直接拒绝 | DROP GRAPH / SESSION SET GRAPH | `gql_transformer.cpp:145-162` |
| 静默崩 | SELECT…FROM GRAPH / GROUP BY / 类型化图（无 visitor，兜底透传给 Cypher 报错） | `gql_function.cpp:140-145` 兜底逻辑 |
| 编译 | `gql` 已在默认 `EXTENSION_LIST` | `extension/extension_config.cmake:1` |

**核心缺陷**：transformer 只实现了 3 个 visitor，其余全靠"整段文本透传"兜底——GQL 真正独有的构造（SELECT 查询根、GROUP BY/HAVING、FETCH、REPEAT…UNTIL、类型化图、ELEMENT 绑定）完全没有映射，落到 Cypher 引擎即失败。

---

## 1. 设计决策：翻译层加厚 vs 原生执行

**结论：Phase 1–5 走"加厚翻译层"（GQL AST → Cypher AST/文本）**，理由：
- Ladybug 的 Cypher 引擎已经内建 GQL 风格路径量词（`SHORTEST`/`ALL SHORTEST`/`TRAIL`/`ACYCLIC`，见 `Cypher.g4: iC_RecursiveType`），大量 GQL 语义有原生对应物。
- 原生执行（GQL → 直接建 Ladybug logical plan）工程量≈重写一半 planner，ROI 低；留作 Phase 6 远期选项。
- 翻译层的关键改造：**把"整段透传兜底"改成"逐构造显式映射 + 显式 unsupported 报错"**，消除静默崩。

**边界**：GQL 的"类型化图（typed graph）/ CREATE GRAPH TYPE"在 Ladybug 无原生对应，需经一层 **schema 映射桥**（GQL graph type → Ladybug `CREATE NODE TABLE`/`CREATE REL TABLE`），这是计划里唯一需要"理解语义"的非平凡桥。

---

## 2. 兼容矩阵（当前 → 目标）

`✓`=已实现  `△`=部分/透传  `✗`=缺失  `→`=目标映射方式

| ISO GQL 特性 | 当前 | 目标 | 映射方式 |
|---|---|---|---|
| MATCH 模式（Cypher 式） | △ 透传 | ✓ | 直接透传 + 归一化 |
| `SELECT … FROM GRAPH` 查询根 | ✗ | ✓ | 改写为 `MATCH … RETURN …`（丢弃 GRAPH 引用，Ladybug 图=当前图） |
| INSERT | ✓ | ✓ | `INSERT`→`CREATE`（已有） |
| DELETE | ✗ | ✓ | GQL DELETE → Cypher `DELETE`/`DETACH DELETE` |
| SET / REMOVE | ✗ | ✓ | GQL SET/REMOVE → Cypher SET/REMOVE |
| MERGE | ✗ | ✓ | 借 Cypher `MERGE`（GQL.g4 未含，需扩 grammar 或直接放行） |
| CREATE GRAPH / PROPERTY GRAPH | △ | ✓ | 补全 `PROPERTY`/`ANY`/`IF NOT EXISTS`/`OR REPLACE` 语义 |
| CREATE GRAPH TYPE（类型化图） | ✗ | △ | schema 映射桥 → `CREATE NODE/REL TABLE` |
| DROP GRAPH | ✗ 拒绝 | ✓ | 路由到原生 `DROP GRAPH`（Ladybug 已有该 DDL） |
| SESSION SET GRAPH / SCHEMA | ✗ 拒绝 | ✓ | 路由到 `USE GRAPH` / 会话图选择 |
| START/COMMIT/ROLLBACK | ✗ | ✓ | → Ladybug `BEGIN/COMMIT/ROLLBACK`（`Cypher.g4: iC_Transaction`） |
| 路径量词 TRAIL/ACYCLIC/SHORTEST | △ | ✓ | 已可透传到原生 `iC_RecursiveType` |
| REPEAT … UNTIL（路径循环） | ✗ | △ | 扩 GQL.g4 含 UNTIL；映射为 `*min..max` 范围或原生递归 |
| GROUP BY / HAVING | ✗ | ✓ | 改写为 `WITH … AS … RETURN …` 聚合模式（GQL 隐式分组 → Cypher 按非聚合键分组） |
| FETCH | ✗ | △ | 映射为 RETURN 属性 / 附加绑定（有限） |
| ELEMENT 绑定 / `?` `*` 基数 / `::` 类型转换 | ✗ | △ | 有限支持，超出部分显式 unsupported |
| 标签表达式 `&` `!` `%` | ✗ | △ | `\|`/`:` 已有；`&`/`!`/`%` 映射或 unsupported |

---

## 3. 实施路线（分 Phase，含文件级任务）

### Phase 0 — 工程基建（必须先做，否则无法验证）
- **任务 0.1** 引入 GQL TCK 风格的 conformance 测试骨架：在 `extension/gql/test/` 新建 `gql_conformance_test.cpp`，每条用例 = `(gql_input, expected_cypher_or_error)`。
- **任务 0.2** 把 `gql_function.cpp:140-145` 的"兜底透传"改为：**未显式映射的 GQL 构造一律抛 `GQL feature not supported`**，杜绝静默崩。
- **任务 0.3** 在 `extension/scripts/generate_gql_grammar.cmake` 确认 ANTLR 版本锁 `4.13.1`，并把 `GQL.g4` 缺失的 `MERGE`/`UNTIL` 等按需补回（注意保持 opengql 上游同步）。

### Phase 1 — 查询核心对齐（最高优先级，覆盖最常见 AI 生成语句）
- **文件**：`gql_transformer.cpp` / `.hpp`，新增 `visitSelectQuery`、`visitGroupByClause`、`visitHavingClause`、`visitFetchClause`。
- **关键映射**：
  - `SELECT a, b FROM GRAPH g MATCH (n) WHERE …` → `MATCH (n) WHERE … RETURN a, b`。
  - `GROUP BY x HAVING count(*) > 3` → `WITH x, count(*) AS c WHERE c > 3 RETURN x, c`（GQL 隐式分组规则）。
- **验收**：TCK 中"简单 MATCH+SELECT""GROUP BY 聚合"用例全绿。

### Phase 2 — 更新 / 写语义统一
- 新增 `visitDeleteStatement`、`visitSetStatement`、`visitRemoveStatement`，分别映射到 Cypher `DELETE`/`SET`/`REMOVE`。
- `CREATE GRAPH` 补全：`PROPERTY`/`ANY` 透传语义、`IF NOT EXISTS`（已有 rewriteFunc 模拟）、`OR REPLACE`（当前拒绝 → 改为原生替换或显式 unsupported）。
- **验收**：GQL 的 INSERT/DELETE/SET/REMOVE 样例与 Cypher 等价语义一致。

### Phase 3 — 路径模式
- 扩 `GQL.g4` 支持 `REPEAT … UNTIL`、量词路径（QPPI）；在 transformer 加 `visitRepeatPathPattern` 映射为 Cypher `*min..max` 或原生递归。
- 校验 `TRAIL`/`ACYCLIC`/`SHORTEST` 已正确落到 `iC_RecursiveType`。
- **验收**：最短路径 / 无环路径 / 定长循环样例通过。

### Phase 4 — 图类型 / Schema 桥（最难）
- 新增 `visitCreateGraphTypeStatement`、`visitCreatePropertyGraphStatement`，设计 **GQL graph type → Ladybug DDL 映射器**：
  - GQL 节点类型 + 属性 → `CREATE NODE TABLE … (props) PRIMARY KEY`；
  - GQL 关系类型 + 端点约束 → `CREATE REL TABLE … FROM … TO …`；
  - GQL `PROPERTY GRAPH` 无类型 → 映射为 Ladybug `CREATE GRAPH` + 隐式表。
- **验收**：给定一份 GQL graph type DDL，能产出等价 Ladybug schema 并可加载数据。

### Phase 5 — 会话 / 事务 / 收尾
- `SESSION SET GRAPH` → `USE GRAPH`；`DROP GRAPH` → 原生；`START TRANSACTION` 等 → `BEGIN/COMMIT/ROLLBACK`。
- **文档**：把 `extension/gql/README` 从"支持 GQL"改为**分级声明**（完全支持 / 部分支持 / 不支持清单），对齐本矩阵。
- 从 `extension_config.cmake` 保留默认编译，但文档明确标注兼容级别。

### Phase 6（远期，可选）— 原生执行
- 若翻译层在 GROUP BY / 类型化图等处出现语义裂缝，评估 GQL → Ladybug logical plan 直连，绕开 Cypher 文本层。

---

## 4. 文件级任务清单（速查）

| 文件 | 改动 |
|---|---|
| `extension/gql/src/gql_transformer.cpp` | 新增 ~10 个 visitor（select/group/having/fetch/delete/set/remove/graphtype/repeat/session） |
| `extension/gql/src/gql_transformer.hpp` | 声明新增 visitor + 状态字段 |
| `extension/gql/src/function/gql_function.cpp` | 改造兜底逻辑为显式 unsupported；DROP/SESSION 路由 |
| `extension/third_party/opengql/GQL.g4` | 补 `MERGE`/`UNTIL`/QPPI（同步上游） |
| `extension/scripts/generate_gql_grammar.cmake` | 锁 ANTLR 版本、重生成 |
| `extension/gql/test/gql_conformance_test.cpp` | 新建 TCK 风格测试 |
| `extension/gql/README.md` | 分级兼容声明 |
| `src/antlr4/Cypher.g4` | 仅确认 `iC_RecursiveType` / `iC_Transaction` 已覆盖（不改） |

---

## 5. 验收 / 测试策略
- **单元级**：每条 GQL 构造有 `(input → expected_cypher)` 对，跑 `make extension-test`。
- **端到端**：在编译出的 0.21.1 shell 跑 `CALL GQL("SELECT n.name FROM GRAPH g MATCH (n:Person)")` 等真实语句，核对结果与等价 Cypher 一致。
- **回归**：确保现有 `CALL GQL("MATCH …")` / `INSERT` 行为不被破坏。
- **合规声明**：发布前用 opengql 自带的 GQL TCK（README 提及的 future work）跑通过率，对外只宣称通过率对应的级别。

---

## 6. 风险与权衡
1. **GQL 隐式分组语义** vs Cypher 聚合差异，可能在边界用例产生不同结果 → Phase 1 必须写对照测试。
2. **类型化图**在 Ladybug 无原生模型，schema 桥是"近似"而非"等价" → 文档必须标注限制。
3. **维护税**：GQL 标准会演进，翻译层需持续同步 opengql 上游 → 建议把 `GQL.g4` 作为 submodule 跟踪，而非 vendored 死拷贝。
4. **AI 识别率悖论**：即便补全 GQL，GQL 公开语料仍少于 openCypher；对"自用 agent"场景，仍建议核心查询走 openCypher + 知识库注入，GQL 层用于对外/标准化互操作。

---

## 7. 给上游 / 社区的 PR 建议
- 先合 Phase 0 + Phase 1（消除静默崩 + 解决最常见 SELECT/GROUP BY），作为最小可用补丁。
- 文档话术从"ISO GQL 兼容"改为"ISO GQL 兼容（分级，见 README 矩阵）"，先止血"假兼容"口碑问题。
- 把 `GQL.g4` vendored 死拷贝改为 tracked submodule，降低长期同步成本。

---
*生成于 2026-09-30，基于本地源码 `C:/Users/chian/Documents/trae_projects/ladybug-0.21.1` 静态分析。*
