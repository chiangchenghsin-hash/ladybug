# LadybugDB Bitemporal Extension — 完整 API 规格

> **扩展名**: `libbitemporal.lbug_extension`
> **版本**: 1.1 (v2 — 支持可变维度 detect_turning_points + 纯 Cypher bitemporal_query)
> **平台**: Windows 11 x64 / MSVC 19.50
> **代码量**: ~290 行 C++（2 个 TableFunction）
> **验证**: 27/27 PASS (Node.js 19 + CLI 8)
>
> **v2 变更 (2026-07-24)**:
> - `detect_turning_points` 签名从 `(LIST<DOUBLE> N×4, LIST<INT64>, threshold:=)` 改为 `(flat_embeddings, N, chapters, D, threshold:=)` — 支持任意维度
> - `bitemporal_query` 从 C++ 占位符改为纯 Cypher 模板
> - 新增 `GRAMMAR_FACT` 关系表 + `cypher_templates/` 目录

---

## 1. 类型系统

### 1.1 LilybugDB 类型 → C++ 类型映射

| LadybugDB 类型 | C++ `LogicalTypeID` | C++ 值类型 | 用途 |
|----------------|---------------------|-----------|------|
| `DOUBLE` | `LogicalTypeID::DOUBLE` | `double` | 特征值、相似度、显著性 |
| `INT64` | `LogicalTypeID::INT64` | `int64_t` | 章节号、向量数量、维度 |
| `STRING` | `LogicalTypeID::STRING` | `std::string` | 方向标签、特征列表描述 |
| `LIST<DOUBLE>` | `LogicalTypeID::LIST` + child `DOUBLE` | 展平数组 `std::vector<double>` | 展平 embedding |
| `LIST<INT64>` | `LogicalTypeID::LIST` + child `INT64` | `std::vector<int64_t>` | 章节号序列 |
| `INTERNAL_ID` | `LogicalTypeID::INTERNAL_ID` | 内部引用（未使用） | — |

### 1.2 参数类型推断

- `detect_turning_points` 的 `LIST` 参数声明为 `LogicalTypeID::ANY`
- `inferInputTypes` 回调在绑定阶段将 `ANY` 解析为 `LIST<DOUBLE>`、`LIST<INT64>`
- `inferInputTypes` 在 binder 创建参数 literal 之后、`bindFunc` 之前执行

### 1.3 可选参数机制

| 机制 | 语法 | 取值方式 |
|------|------|----------|
| Legacy optional params | `threshold := 0.3` | `in->optionalParamsLegacy[i]->constPtrCast<LiteralExpression>()->getValue()` |
| 默认值 | 不传 `threshold:=` | C++ 内 `double thr = 0.3` |

> ⚠️ LadybugDB 0.18.3 不支持位置可选参数，仅支持命名可选参数 (`optionalParamsLegacy`)。

---

## 2. 函数 API

### 2.1 character_similarity

#### 接口签名

| 方向 | 索引 | 名称 | 类型 | 默认值 | 语义 |
|------|------|------|------|--------|------|
| **IN** | 0 | `ba_ratio_a` | `DOUBLE` | — | 角色 A 的"把"字句比例 |
| **IN** | 1 | `ba_ratio_b` | `DOUBLE` | — | 角色 B 的"把"字句比例 |
| **IN** | 2 | `bei_ratio_a` | `DOUBLE` | — | 角色 A 的"被"字句比例 |
| **IN** | 3 | `bei_ratio_b` | `DOUBLE` | — | 角色 B 的"被"字句比例 |
| **IN** | 4 | `sentiment_a` | `DOUBLE` | — | 角色 A 的情感值 [-1, 1] |
| **IN** | 5 | `sentiment_b` | `DOUBLE` | — | 角色 B 的情感值 [-1, 1] |
| **IN** | 6 | `dominance_a` | `DOUBLE` | — | 角色 A 的支配力 [0, 1] |
| **IN** | 7 | `dominance_b` | `DOUBLE` | — | 角色 B 的支配力 [0, 1] |
| | | | | | |
| **OUT** | 0 | `similarity_score` | `DOUBLE` | — | 4 特征均值相似度 [0, 1] |
| **OUT** | 1 | `ba_ratio_similarity` | `DOUBLE` | — | 单特征相似度 [0, 1] |
| **OUT** | 2 | `bei_ratio_similarity` | `DOUBLE` | — | 单特征相似度 [0, 1] |
| **OUT** | 3 | `sentiment_similarity` | `DOUBLE` | — | 单特征相似度 [0, 1] |
| **OUT** | 4 | `dominance_similarity` | `DOUBLE` | — | 单特征相似度 [0, 1] |
| **OUT** | 5 | `compared_features` | `STRING` | — | 固定 "ba_ratio,bei_ratio,sentiment,dominance" |

#### 核心算法

```
featureSim(a, b) = 1 − |a − b| / max(|a|, |b|, 0.001)
similarity_score  = (featureSim(ba) + featureSim(bei) + featureSim(sent) + featureSim(dom)) / 4
```

#### C++ 内部接口

| 方法 | 生命周期 | 职责 |
|------|----------|------|
| `getFunctionSet()` | 静态，扩展加载时调用 1 次 | 创建 `TableFunction` 对象，注册回调 |
| `bindFunc()` | 每次 CALL 调用 1 次 | 解析 8 个 DOUBLE → `CSBD` 绑定数据 |
| `tableFunc()` | 执行期调用 1 次 | 计算特征相似度 → 写入 `DataChunk`（1 行） |
| `initSharedStateFunc` | → `SimpleTableFunc::initSharedState` | 从 `bindData->numRows=1` 创建 morsel |
| `initLocalStateFunc` | → `TableFunction::initEmptyLocalState` | 空本地状态（无累加/缓存需求） |

#### 嵌套结构

```
TableFuncBindData (CSBD)
├── fa[4]:  double[4]    // 角色A特征 [ba_ratio, bei_ratio, sentiment, dominance]
├── fb[4]:  double[4]    // 角色B特征
├── columns: expression_vector (6 outputs)
└── numRows:  row_idx_t = 1

DataChunk (1行 × 6列)
├── ValueVector[0]: DOUBLE   similarity_score
├── ValueVector[1]: DOUBLE   ba_ratio_similarity
├── ValueVector[2]: DOUBLE   bei_ratio_similarity
├── ValueVector[3]: DOUBLE   sentiment_similarity
├── ValueVector[4]: DOUBLE   dominance_similarity
└── ValueVector[5]: STRING   compared_features
```

#### 错误行为

| 场景 | 行为 |
|------|------|
| 参数数量 < 8 | LadybugDB binder 报错 (参数不足) |
| 参数数量 > 8 | LadybugDB binder 报错 (参数过多) |
| 参数类型不是 DOUBLE | LadybugDB binder 报错 (类型不匹配) |
| NaN 输入 | ⚠️ 未防御 → `std::max(NaN, 0.001)` 行为未定义 |
| Inf 输入 | ⚠️ 未防御 → 计算结果可能为 NaN |
| 全零特征 | 正常 → 分母保护 `max(a,b,0.001)` = 0.001 → 1.0 |

---

### 2.2 detect_turning_points

#### 接口签名

| 方向 | 索引 | 名称 | 类型 | 默认值 | 语义 |
|------|------|------|------|--------|------|
| **IN** | 0 | `flat_embeddings` | `LIST<DOUBLE>` | — | N×D 个 double (row-major: e₁₁,…,e₁_D, e₂₁,…,e_N_D) |
| **IN** | 1 | `num_embeddings` | `INT64` | — | N，embedding 向量的个数 |
| **IN** | 2 | `chapters` | `LIST<INT64>` | — | N 个章节号，与 embedding 按索引 1:1 对应 |
| **IN** | 3 | `embedding_dim` | `INT64` | — | D，每个 embedding 的维度（如 768） |
| **IN** | opt | `threshold` | `DOUBLE` | `0.3` | 突变检测阈值 [0, 2] |
| | | | | | |
| **OUT** | 0 | `chapter_number` | `INT64` | — | 转折点发生的章节号 |
| **OUT** | 1 | `embedding_shift` | `DOUBLE` | — | 与前章的 cosine 距离 [0, 2] |
| **OUT** | 2 | `significance` | `DOUBLE` | — | min-max 归一化显著性 [0, 1] |
| **OUT** | 3 | `direction` | `STRING` | — | `"up"` (加速偏离) 或 `"down"` (减速) |

#### 核心算法

```
// 1. 输入验证
if (num_embeddings < 2) return 0 rows;
if (len(flat_embeddings) ≠ num_embeddings × embedding_dim) return 0 rows;
if (len(chapters) ≠ num_embeddings) return 0 rows;

// 2. 重组向量 + 逐对 cosine distance
for i = 1 to N−1:
    prev = flat_embeddings[(i−1)×D .. i×D−1]
    curr = flat_embeddings[i×D .. (i+1)×D−1]
    cosine  = dot(prev, curr) / (|prev| × |curr|)
    if cosine > 1: clamp to 1; if cosine < −1: clamp to −1
    dist[i−1] = 1 − cosine                           // range [0, 2]

// 3. Min-max 归一化
minD = min(dist), maxD = max(dist)
range = maxD − minD + 0.001
for each dist[j]:
    if dist[j] > threshold:
        significance = (dist[j] − minD) / range      // [0, 1]
        direction    = (j>0 && dist[j] > dist[j−1]) ? "up" : "down"
        output(chapters[j+1], dist[j], significance, direction)

// 4. 按 significance 降序排列
```

#### C++ 内部接口

| 方法 | 生命周期 | 职责 |
|------|----------|------|
| `getFunctionSet()` | 静态，扩展加载时 | 创建 `TableFunction`，4 参数 + `inferInputTypes` 回调 |
| `inferInputTypes()` | `bindFunc` 之前，1 次 | `ANY`→`LIST<DOUBLE>`, `ANY`→`LIST<INT64>` 类型解析 |
| `bindFunc()` | 每次 CALL，1 次 | `NestedVal` 解析 LIST 参数 → `DTPBD`；验证维度一致性 |
| `tableFunc()` | 执行期，1 次 | 重组向量 → cosine distance → 归一化 → 排序 → 写入 `DataChunk` |
| `initSharedStateFunc` | → `SimpleTableFunc::initSharedState` | 从 `maxRows = N−1` 创建 morsel |
| `initLocalStateFunc` | → `TableFunction::initEmptyLocalState` | 空本地状态 |

#### 嵌套结构

```
TableFuncBindData (DTPBD)
├── embeds:  vector<double>     // N×D 展平 (row-major)
├── chaps:   vector<int64_t>    // N 章节号
├── dims:    int64_t            // D (e.g. 768)
├── numEmb:  int64_t            // N
├── thr:     double             // 检测阈值
├── columns: expression_vector  // 4 outputs
└── numRows: row_idx_t          // max(N−1, 1)

LIST<DOUBLE> 参数 → NestedVal 解析
Value(0)
└── children[] (N×D 个 Value，每个 Value.getValue<double>())

LIST<INT64> 参数 → NestedVal 解析
Value(2)
└── children[] (N 个 Value，每个 Value.getValue<int64_t>())

DataChunk (≤N−1 行 × 4 列，按 significance 降序)
├── ValueVector[0]: INT64    chapter_number
├── ValueVector[1]: DOUBLE   embedding_shift
├── ValueVector[2]: DOUBLE   significance
└── ValueVector[3]: STRING   direction
```

#### 错误行为

| 场景 | 行为 |
|------|------|
| `num_embeddings < 2` | 返回 0 行 |
| `len(flat) ≠ N×D` | 返回 0 行（`numEmb=0` 静默） |
| `len(chapters) ≠ N` | 返回 0 行 |
| `embedding_dim ≤ 0` | 返回 0 行 |
| LIST 参数为 NULL | ⚠️ 未防御 → `NestedVal::getChildrenSize(NULL)` = SIGSEGV |
| 所有距离 ≤ threshold | 返回 0 行 |
| threshold 不传 | 默认 `0.3` |
| 零向量 (|v|=0) | cosine distance 返回 0.0（分母保护） |
| 浮点舍入导致 cosine>1 | clamp 到 [−1, 1] |

---

## 3. 加载与生命周期

### 3.1 加载

```sql
-- 每个会话必须加载一次
LOAD EXTENSION 'path/to/libbitemporal.lbug_extension';
```

```javascript
// Node.js
conn.querySync("LOAD EXTENSION 'C:/path/libbitemporal.lbug_extension'");
```

### 3.2 生命周期

| 阶段 | 事件 | C++ 钩子 |
|------|------|----------|
| 加载 | `LOAD EXTENSION` | `init(ClientContext*)` → `BitemporalExtension::load()` → `addTableFunc<>()` |
| 绑定 | `CALL f(...)` | `bindFunc()` → 解析参数 → 创建 `BindData` |
| 执行 | 返回结果 | `initSharedState()` → `getMorsel()` → `tableFunc()` |
| 卸载 | 会话关闭 | 自动析构（无 `deinit`） |

### 3.3 函数可见性

- `LOAD EXTENSION` 后，`character_similarity` 和 `detect_turning_points` 在当前会话中全局可见
- 关闭/重新 open 数据库后 → 必须重新 `LOAD EXTENSION`
- 不持久化到 catalog

---

## 4. Cypher 模板：bitemporal_query

`bitemporal_query` **不是 C++ 函数**。它是 **纯 Cypher 模板**，位于 `cypher_templates/bitemporal_query.cypher`。

### 4.1 依赖表

| 表 | 类型 | 列 |
|----|------|-----|
| `Character` | NODE | `id STRING PK`, `name STRING`, `work_id STRING`, … |
| `Chapter` | NODE | `id STRING PK`, `chapter_number INT16`, `title STRING` |
| `GRAMMAR_FACT` | REL | `FROM Character TO Chapter`, `fact_type STRING`, `value DOUBLE`, `analysis_version STRING`, `tx_created TIMESTAMP`, `tx_expired TIMESTAMP`, `valid_from INT64`, `valid_to INT64` |

### 4.2 模板列表

| 模板 | Cypher | 用途 |
|------|--------|------|
| 模板1 | `MATCH (c:Character)-[f:GRAMMAR_FACT]->(ch:Chapter) WHERE ch.chapter_number=$c AND f.valid_from<=$c AND (f.valid_to=-1 OR f.valid_to>=$c)` | 单角色+单回合双时态事实 |
| 模板2 | + `f1.fact_type = f2.fact_type` + delta 计算 | 跨回合对比 |
| 模板3 | 无 WHERE id 过滤，GROUP BY character | 角色群像快照 |

### 4.3 双时态逻辑

```
有效时间: valid_from ≤ chapter_number ≤ valid_to (valid_to=−1 表示"至今有效")
事务时间: tx_created ≤ tx_time < tx_expired (tx_expired=NULL 表示"当前有效")
```

---

## 5. 返回格式 (跨 API 差异)

### 5.1 CLI (lbug_shell.exe)

```
┌──────────────────┬─────────────────────┬─────┬──────────────────────┐
│ similarity_score │ ba_ratio_similarity │ ... │ compared_features    │
│ DOUBLE           │ DOUBLE              │     │ STRING               │
├──────────────────┼─────────────────────┼─────┼──────────────────────┤
│ 0.664583         │ 0.833333            │ ... │ ba_ratio,bei_ratio...│
└──────────────────┴─────────────────────┴─────┴──────────────────────┘
(1 tuple)
(6 columns)
```

### 5.2 Node.js (QueryResult API)

```javascript
const r = conn.querySync("CALL character_similarity(...) RETURN *;");
while (r.hasNext()) {
    const row = r.getNext();  // → {}
    // ⚠️ row 是空 object，按索引取值：
    // row[0] = similarity_score  (number)
    // row[1] = ba_ratio_similarity (number)
    // ...
    // row[5] = compared_features  (string)
}
```

| 属性 | 类型 | 说明 |
|------|------|------|
| `r.hasNext()` | `() => boolean` | 是否有下一行 |
| `r.getNext()` | `() => object` | 返回空 object `{}`；值按数字索引 `row[0]..row[n-1]` 取 |
| `getColumnNames()` | — | 0.18.3 Node.js 绑定未暴露此方法 |
| `getColumnTypes()` | — | 同上 |

> ⚠️ SIMPLE 函数 1 行输出时 `QueryResult.hasNext()` 可能返回 2 次 `true`（SimpleTableFunc morsel 包装器行为）。建议用 `while (r.hasNext())` 而非假设固定行数。

---

## 6. 错误码与诊断

### 6.1 LadybugDB 框架错误

| 错误 | 触发条件 |
|------|----------|
| `Binder exception: function character_similarity expects 8 parameters` | 参数数量错误 |
| `Binder exception: type mismatch` | 参数类型错误 |
| `Binder exception: function not found` | 未 LOAD EXTENSION |

### 6.2 扩展内静默处理

| 场景 | 行为 |
|------|------|
| 维度不匹配 | `detect_turning_points` 返回 0 行 |
| 少于 2 个 embedding | `detect_turning_points` 返回 0 行 |
| 所有值低于阈值 | `detect_turning_points` 返回 0 行 |
| 全零特征 | `character_similarity` 返回 1.0（分母保护） |
| NULL LIST 参数 | ⚠️ 未防御 → 可能 SIGSEGV |

---

## 7. 性能特性

| 函数 | 计算复杂度 | 空间复杂度 | 最大输入 |
|------|-----------|-----------|---------|
| `character_similarity` | O(1) | O(1) | 固定 8 个 DOUBLE |
| `detect_turning_points` | O(N×D) | O(N×D) | `N×D` 受 LIST literal 大小限制（无硬限制，实用 ~N=120, D=768 → ~368KB） |

- 纯计算函数 — 无磁盘 I/O
- 扩展内无 `NodeTable::scan` 调用 → 无 `MemoryManager` 生命周期冲突
- `SimpleTableFunc` 包装: 单 morsel (`maxMorselSize = DEFAULT_VECTOR_CAPACITY = 2048 行`)

---

## 8. 扩展注册表

```cpp
// bitemporal_extension.cpp
BitemporalExtension::load(context) {
    ExtensionUtils::addTableFunc<CharacterSimilarityFunction>(db);
    ExtensionUtils::addTableFunc<DetectTurningPointsFunction>(db);
}

// bitemporal_function.h
struct CharacterSimilarityFunction { static constexpr const char* name = "CHARACTER_SIMILARITY"; ... };
struct DetectTurningPointsFunction { static constexpr const char* name = "DETECT_TURNING_POINTS"; ... };

// CMakeLists.txt: build_extension_lib(${BUILD_STATIC_EXTENSION} "bitemporal")
// extension_config.cmake: EXTENSION_LIST += bitemporal
// extension/CMakeLists.txt: add_extension_if_enabled("bitemporal")
```

---

## 9. 版本兼容性

| 组件 | 版本 | 注解 |
|------|------|------|
| LadybugDB Core | 0.18.3 | Windows 11 x64, MSVC 19.50 |
| SimpleTableFunc | 0.18.3 | `initSharedState` / `initEmptyLocalState` |
| NestedVal API | 0.18.3 | LIST 参数解析 |
| inferInputTypes | 0.18.3 | `ANY → LIST` 类型推断 |
| 二进制格式 | v42 | 数据库格式 |

---

## 10. 文档索引

| 文档 | 内容 |
|------|------|
| [SPEC.md](SPEC.md) | 设计决策 + PR 模板 |
| [README.md](README.md) | 快速入门 |
| [bitemporal_query.cypher](cypher_templates/bitemporal_query.cypher) | 纯 Cypher 双时态模板（3 模板） |
| [test_data.cypher](test_data.cypher) | 测试数据库（红缕梦 5 角色 + 8 Snapshot + GRAMMAR_FACT） |
| [verify.js](test/verify.js) | Node.js 自动化测试 (19 PASS) |
| [verify_bitemporal.js](test/verify_bitemporal.js) | CLI 专项测试 (8 PASS) |
