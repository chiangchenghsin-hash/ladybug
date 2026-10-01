# GQL 翻译模拟器 · 外部咨询回帖（Q1–Q7 方案建议）

> **对应简报**：`docs/gql_consult_brief.md`（2026-01-10 快照，自测 87/87，TCK 165/30/11）
> **回帖日期**：2026-10-01。**总原则**：全部建议落在翻译层内，不动引擎语义；每条给出可双跑验证形态。
> **预期收益**：TCK 30 挂 → ~8（Q4 消 13、Q1 消 6、Q2 消 1；语料 4 例经 harness 适配后可能再消 3）。

## 优先级总览

| 优先级 | 问题 | 场景收益 | 手段一句话 | 动引擎 |
|---|---|---|---|---|
| P0 | Q4 SCHEMA | 13 | 扩展注册表 + `sch__gr` 名称改写 | 不动 |
| P0 | Q1 全序 + min/max | 6 | `_gql_to_json` 逐元素保型 + `_gql_max/min` 扩展聚合 | 不动 |
| P1 | Q3 GQLSTATUS | 断言质量全局升级 | 码表挂 `unsupported()` 单一漏斗 | 不动 |
| P1 | Q2 ANY 图 JSON 聚合 | 1 + 隐性面 | `_gql_num(json)` 标量桥自动包装 | 不动 |
| P2 | Q6 plan cache | 性能 | 扩展状态槽 LRU + 本地 epoch 失效 | 不动 |
| P3 | Q5 运行时残余 | 收敛 | Q1 机制外溢 + 文档化 | 不动 |
| P3 | Q7 语料污染 | 3~4 | harness 转译 + 上游 issue | 不动 |

**关键地基**：Q1 的 `_gql_to_json` / `_gql_max` / `_gql_min` 三件套同时是 Q2、Q5 的公共机制，建议最先实现。

---

## Q1 跨类型全序与列表 min/max（6 例）—— v2，基于真 API 重排（2026-10-01）

**核心点子不变**：异构列表字面量逐元素包装进 JSON——JSON 是"保型的同构槽位"，同构化目标统一为 LIST(JSON) 但原值与原类型完整保留在 JSON 内部。**变的是实现路径：全部按仓内真实 API 钉死，且原"案 b（bind 期钉返回类型保真）"经实证推翻，见 §3。**

### 0. 真 API 依据（均已 file:line 核实，勿重复调研）

| # | 事实 | 出处 |
|---|---|---|
| A1 | `AGGREGATE_FUNCTION_ENTRY = 20` 存在；`extension::addFunc<T>(db, name, type)` 模板公开、幂等（`containsFunction` 检查）——**扩展可直接注册聚合函数** | `src/include/extension/extension.h:64-72`、`src/include/catalog/catalog_entry/catalog_entry_type.h:17` |
| A2 | `AggregateFunction` 构造接受 `bindFunc`（`scalar_bind_func`）；聚合绑定路径调用之并**采纳其 `FunctionBindData::resultType`**；聚合路径**不对参数插隐式转换**（JSON 输入直达 exec，无 ANY-cast 风险） | `src/include/function/aggregate_function.h:49-53`、`src/binder/bind_expression/bind_function_expression.cpp:177-183` |
| A3 | 参数匹配：`paramTypeID == LogicalTypeID::ANY` 即通配（聚合匹配与 cast 代价表同口径）；`to_json(ANY) -> JSON` 是同仓现成先例（ScalarFunction + bindFunc 形态） | `src/function/built_in_function_utils.cpp:97,119`、`extension/json/src/functions/creation_functions/to_json.cpp:38-45` |
| A4 | `LogicalTypeID::JSON = 60` 在 core，物理变长（STRING 族）——聚合状态照抄仓内 MIN/MAX-over-STRING 的变长状态管理模式（`InMemOverflowBuffer`） | `src/include/common/types/types.h:235,262`、`src/function/aggregate/`（min/max 实现） |
| A5 | 注册落 main catalog——与"扩展函数只注册 main catalog + 已有图 catalog 回退"现状一致，`USE GRAPH` 下解析由既有 `setFunctionFallback` 兜住 | 简报 §2.3；`gql_extension.cpp:11-14`（注册点） |

### 1. 落点（三个新函数 + 两处翻译改动）

- **`_gql_to_json(ANY) -> JSON` 标量**：签名与 bind 形态照抄 `to_json.cpp:38-45`（同仓 Apache-2.0）；**自托管于 GQL 扩展**（`_gql_` 前缀避免与 JSON 扩展重名；`addFunc` 幂等，JSON 扩展已加载也不冲突）。不直接 emit `to_json(...)` 文本——那会引入对 JSON 扩展加载顺序的隐式依赖。注册：`GqlExtension::load` 加 `ExtensionUtils::addScalarFunc<GqlToJsonFunction>(db)`（`extension/gql/src/main/gql_extension.cpp:11-14`）。
- **`_gql_max(JSON) -> JSON` / `_gql_min(JSON) -> JSON` 聚合**：`AggregateFunction` 五回调（initialize/updateAll/updatePos/combine/finalize，见 A2）；状态 = 变长 JSON 文本 + isNull（照抄 A4 模式）；update 内解析 JSON（复用 vendored yyjson，`extension/json/src/utils|common`）按 GQL 全序比较，**null 跳过**（聚合语义）。注册走 A1 通用模板：`extension::addFunc<GqlMaxFunction>(db, name, catalog::CatalogEntryType::AGGREGATE_FUNCTION_ENTRY)`。
- **翻译层**：`gql_transformer.cpp:192` `ListValueConstructorByEnumeration` 分支——撤掉 `heterogeneous list literal` 拒绝（`:206`），元素静态混合或类型不可判时逐元素发射 `_gql_to_json(ei)`；`translateFor`（`:715` FOR→UNWIND）不动（UNWIND 吃 LIST(JSON) 出 JSON 标量，聚合输入即每行一个 JSON——所以聚合签名是 `(JSON)` 而非 `(LIST(JSON))`）；聚合调用点重写 `max/min → _gql_max/_gql_min`，触发条件（结构化知识，不靠静态判别）：实参可回溯到被包装 FOR 字面量的变量，或实参本身是列表类型表达式。

### 2. 全序抄写来源（三源交叉，⚠️ 许可证陷阱）

- ISO 39075 比较/排序章（规范口径）；**openCypher CIP2016-06-14**（comparability/orderability，Apache-2.0 可署名留档）；opengql TCK 期望值（可执行判定）。
- ⚠️ **Neo4j 主仓（含 `org.neo4j.values` 比较器）是 GPLv3，不是 Apache-2.0**——只有 `opencypher/*` 组织产物可抄，勿抄 neo4j/neo4j 源码。
- 跨 INT/DOUBLE 按**数值序**比较、胜出元素以 JSON 原样保留（`5` 的 JSON 文本是 `5`）；列表按字典序逐元素（解 `max([[1],[2],[2,1]])` → `[2,1]`，同一比较器递归即可）。

### 3. 胜出元素类型保真 —— ⚠️ 结论被真 API 推翻后重述

- **原案 b 证伪**：bindFunc 确能钉返回类型（A2），但 UNWIND 通道下聚合实参静态类型恒为 JSON，胜出者是 INT 还是 DOUBLE **绑定期不可知**；而一切"保原始元素"旁路（平行列表、struct 配对、UNION 分支逐元素返回）都会再次撞列表同构化或列类型合一——此路不通。
- **现主案（唯一不动引擎的路线）**：`_gql_max` 返回 JSON（JSON 文本层 `5` 与 `5.0` 天然有别，保真到 JSON 层），**run_tck.py 比较器做数值/JSON 归一**（`5 == 5.0`、JSON 数组 `[2,1]` 与 LIST `[2,1]` 视同相等）。
  - 探机项①：run_tck.py 现有比较逻辑是否已有数值归一；没有则加（harness 侧，非产品语义）。
  - 探机项②：`.test` 双跑 harness 对 JSON 结果的展示/比较形态（等价 Cypher 侧同样走 `_gql_max`，天然一致）。
- **完整保真（返回引擎 INT64/DOUBLE）只有引擎 variant/UNION 类型一条路 = 动引擎，出局**；记为模拟器固有上限，追加 README 已知差异条目（与 §3.2 值模型残余同栏）。

### 4. 双跑验收形态

- GQL：`FOR x IN [1, 2.0, 5, null, 3.2, 0.1] RETURN max(x)` ↔ Cypher：`UNWIND [_gql_to_json(1), _gql_to_json(2.0), _gql_to_json(5), _gql_to_json(null), _gql_to_json(3.2), _gql_to_json(0.1)] AS x RETURN _gql_max(x)`，期望 5（经 harness 归一）；
- GQL：`FOR x IN [[1],[2],[2,1]] RETURN max(x)` ↔ 同构 Cypher，期望 `[2,1]`；
- 回归钉子：原 `heterogeneous list literal` 拒绝用例翻转为通过用例（`test/tck/REPORT.md` 对应 6 例转绿）。

### 5. 响亮性

全序未覆盖的类型对（JSON object / bool 与数值跨界等 TCK 未定义对）→ update 内响亮抛 `GQL feature not supported: total order over <type>`；非列表/非包装位不触发重写，无新增静默路径。

---

## Q2 ANY 图 JSON 属性的数值聚合/比较（1 例 + 隐性面）

1. **先探机（5 分钟）**：`MATCH (p) RETURN CAST(p.age AS DOUBLE)` 在 ANY 图上的可用性与 null 行为（CAST 实现集中在 `src/function/vector_cast_functions.cpp`）。若可用，案 1（翻译层插 CAST）只需几十行——但**仍主推案 2**。
2. **主推案 2：`_gql_num(json)` 扩展标量桥**。理由：CAST 语义引擎定义，JSON `"abc"` / JSON bool 撞进去行为不可控；`_gql_num` 语义全由扩展掌控：
   - JSON number → DOUBLE/INT64；JSON null / 属性缺失 → NULL（sum 自然跳过，正对期望 75）；非数值 → **响亮抛**。
   - 比较位（`p.age > 30`）同样自动包装。
3. **落点**：表达式统一漏斗（`replaceExprs`）增加 ANY 图模式——翻译期查图类型注册表（已有，挂扩展状态槽），当前图为 ANY 时，聚合实参与比较操作数中的属性访问自动包 `_gql_num(...)`。裸投影 `RETURN p.age` **不包**，记为 JSON 展示差异残余（追加 README 已知差异条目）。
4. **案 3（INSERT 落强类型列）否决**：与 ANY 图 schema-less 产品取舍冲突，且只管新数据。
5. **双跑验收**：GQL `MATCH (p) RETURN p.name, sum(p.age)` ↔ Cypher `MATCH (p) RETURN p.name, sum(_gql_num(p.age))`，期望 75。
6. **响亮性**：非数值 JSON 进 `_gql_num` 响亮抛；包装仅发生在"ANY 图 + 聚合/比较位"两个可判定条件同时成立处，无静默路径。

---

## Q3 GQLSTATUS 错误码信封（最小可行）

1. **最小信封**：`GQLSTATUS`（5 字符）+ `message` + 可选 `position`。STATUS_RECORDS 链暂缓（TCK 异常断言只需首码）。
2. **落点（注入点天然存在）**：`GqlToCypherTransformer::unsupported()` 是所有"设计内拒绝"的单一漏斗——在此挂码表，一次覆盖整个"不支持特性"大类；ANTLR error listener 处挂语法错码；引擎穿透错误用**文本前缀映射表**兜底，映射不上 → 默认码 + 原信息保留（响亮）。异常类型升级为携带码的结构化异常，run_tck.py 断言从"有错"升级为"码相等"。
3. **首批码集**：语法错（42xxx 族）、feature not supported（0A 族）、命名冲突/约束违例（GQL 特有族）、数据异常（22xxx）。**具体码值以 opengql TCK 场景声明的 error condition 为可执行规范逐条对齐，不自创**。
4. **抄写来源**：ISO 39075 GQLSTATUS 结构 + TCK 条件名；Neo4j 状态码文档表可参考分类思路（文档 CC 可取，代码 GPLv3 不可取——只抄表不抄码）。
5. **动引擎**：不动。
6. **双跑验收**：每个拒绝路径补 `.test` 码断言；87 例自测全量补码。响亮性：本就是错误路径强化，天然满足。

---

## Q4 SCHEMA 命名空间模拟（13 例，最大单一收益块）

1. **方案：扩展内 schema 注册表 + 名称改写混合**。
   - 注册表挂现有扩展状态槽（与图类型注册表同生命周期、同"不落 WAL"已知限制，与已决事项 1 一致）；
   - catalog 真实图名改写：`sch.gr` → `sch__gr`。
2. **解析规则**：qualified name → 查注册表，sch 不存在 → 响亮错；存在 → 改写后走正常管线。裸名 → 默认 schema，行为同现状（兼容现有 87 例）。
3. **错误条件覆盖**（TCK 13 例几乎全在断言这些）：
   - CREATE SCHEMA 重名 → 错；schema 名与现有 graph/type 冲突 → 错；
   - CREATE GRAPH sch.gr 且 sch 不存在 → 错；
   - **DROP SCHEMA 非空 → 错**（注册表记录成员图，4 例 DROP 侧错误条件由此免费获得）；
   - IF EXISTS / IF NOT EXISTS 按标志旁路。
4. **改写碰撞防护**：注册表同时登记改写名；用户裸建字面 `sch__gr` → 拒或转义，防伪装。
5. **落点**：新模块 `gql_schema.{hpp,cpp}`；`gql_function.cpp` 注册表初始化；transformer 五处钩子（CREATE/DROP SCHEMA、CREATE/DROP/USE GRAPH 的 qualified name 位）。估 ~400–600 行。
6. **动引擎**：不动。
7. **双跑验收**：GQL `CREATE SCHEMA s; CREATE GRAPH s.g ...` ↔ Cypher 对改写名 `s__g` 的等价 DDL 序列；13 例错误条件逐例 `.test` 码断言（依赖 Q3 落地，故排 Q3 后联调）。
8. **响亮性**：全部错误条件显式检查显式抛，天然满足。

---

## Q5 异构列表运行时残余

**建议：收敛，不追理论完备。**

1. Q1 机制落地后顺手解掉大半：**FOR / min/max 实参位的列表一律逐元素 `_gql_to_json` 包装**（这些是结构化翻译点，信息翻译期已知，不依赖静态判别）——非字面量元素 `[x, 1.0]` 在这些位置同样保型。
2. 其余位置（`=` / IN 等谓词位）全包会连锁要求重写所有列表谓词，投入产出不匹配——维持现状，README #17 残余条目更新为"仅谓词位残余"。
3. **业界先例**：同构化是 Kuzu 系引擎根因；Memgraph 等对混合列表直接报错。模拟器文档化残余属常规接受度。
4. **红线核算**：包装位之外不新增静默路径；已包装位消灭已知错答面。满足"0 静默错答案"的实际口径。
5. **动引擎**：不动。双跑随 Q1 用例覆盖。

---

## Q6 双重解析与 plan cache

1. **做缓存，跳过 AST 级翻译**。关键观察：**所有 GQL DDL 都过 `CALL GQL`**——扩展自己看得到每一次 schema 变更，失效问题从"全局监听"退化为"本地 epoch 自增"。
2. **设计**：扩展状态槽挂 LRU（~256 项）；key = GQL 归一化文本 + 当前图名 + epoch；任何 DDL 翻译（CREATE/DROP GRAPH/TYPE/SCHEMA、USE 切图）epoch++；命中即跳过 ANTLR + transform，直接出 Cypher 文本。
3. **已知盲区**：原生 Cypher 侧直改 catalog 不触发失效——文档化；若引擎暴露 catalog 版本号可并入 key（探机项）。
4. **AST 级翻译评估为负**：文本漏斗残余风险在边角（奇异标识符/字面量）而非性能，你们已做边界感知；AST 级需引引擎内部 API，撞硬约束 1/4，性价比不成立。
5. **落点**：`gql_function.cpp` bind 路径前置缓存查询。动引擎：不动。
6. **验收**：同语句双跑（冷/热）结果一致；epoch 递增用例（DDL 后旧缓存不得命中）。

---

## Q7 TCK 语料污染（业界惯例）

三条组合拳，均为业界标准做法：

1. **4 例全部上游提 issue**（TCK 权威性靠消费者反馈维护，这是惯例中的惯例）；
2. **openCypher setup（3 例）→ harness 侧机械转译**：run_tck.py 预处理 CREATE→INSERT、UNWIND→FOR，逐条对应、注释标明 harness-only。属测试基建，不违反已决事项 5（不当产品 bug 修）；
3. **Aggregation3 [1] 列名不符** → 本地 patch 期望值 + issue 登记；
4. **`CREATE GRAPH ANY AS COPY OF` 文法歧义（1 例）** → **不值得**加预解析归一：单例收益不抵宽容层复杂度（#14 清单已够长），本地 skip-with-reason + issue。

---

## 落地顺序建议

```
Q1 三件套（_gql_to_json / _gql_max / _gql_min）   ← 公共地基，Q2/Q5 依赖
  → Q4 SCHEMA 注册表（最大收益块）
  → Q3 GQLSTATUS（此后所有新拒绝路径带码，Q4 错误条件断言依赖它，可提前并行）
  → Q2 _gql_num 标量桥
  → Q6 缓存
  → Q7 harness 适配（随时可插）
```

注：Q3 与 Q4 有依赖（Q4 错误条件断言要码），实际执行建议 **Q1 → Q3 → Q4 → Q2 → Q6 → Q7**。
