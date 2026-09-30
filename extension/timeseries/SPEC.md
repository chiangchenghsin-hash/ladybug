# Bitemporal Extension — 完整规格

## 文件清单

```text
extension/bitemporal/
├── CMakeLists.txt                               # 树内构建集成
├── README.md                                    # 快速入门
├── SPEC.md                                      # 本文件
├── cypher_templates/
│   └── bitemporal_query.cypher                  # 纯 Cypher 双时态查询模板（3 种场景）
├── src/
│   ├── include/
│   │   ├── function/bitemporal_function.h        # 函数声明（2 个函数）
│   │   └── main/bitemporal_extension.h           # 扩展入口类
│   ├── function/
│   │   ├── CMakeLists.txt
│   │   ├── character_similarity.cpp              # 8 DOUBLE → 5 维相似度矩阵
│   │   └── detect_turning_points.cpp             # LIST<DOUBLE> 展平 embedding → 转折点
│   └── main/
│       ├── CMakeLists.txt
│       └── bitemporal_extension.cpp              # init() + name() C 导出
├── test/
│   ├── CMakeLists.txt
│   └── verify_bitemporal.js                      # 8/8 PASS CLI 验证套件
└── build/
    └── libbitemporal.lbug_extension              # 编译产物
```

## 核心参考摘要

### 1. character_similarity（不变）
- **输入：** 8 个 DOUBLE（4 特征 × 2 角色）
- **输出：** 1 行，6 列
- **算法：** 逐特征归一化差异 → 均值
- **C++ 文件：** `character_similarity.cpp`（~70 行）

### 2. detect_turning_points（v2，支持可变维度）
- **旧签名：** `(LIST<DOUBLE> N×4 features, LIST<INT64> chapters, threshold:=0.3)`
- **新签名：** `(flat_embeddings LIST<DOUBLE>, num_embeddings INT64, chapters LIST<INT64>, embedding_dim INT64, threshold:=0.3)`
- **变更原因：** 支持任意维度 embedding（推荐 768＝LadybugDB FLOAT[768]），不硬编码 4 维特征
- **算法：** 从展平 LIST 重组 N 个 D 维向量 → 连续对 cosine distance → min-max 归一化 → filter by threshold → sort by significance desc
- **输入验证：** `len(flat_embeddings) == N×D` 且 `len(chapters) == N`，不匹配 → 0 行
- **C++ 文件：** `detect_turning_points.cpp`（~150 行）

### 3. bitemporal_query（从 C++ 移除 → 纯 Cypher）
- **原因：** 双时态过滤的本质是 `WHERE f.valid_from <= $chapter AND (f.valid_to = -1 OR f.valid_to >= $chapter)` — 纯 Cypher 即可实现，无需 C++ 扩展
- **Cypher 模板：** `cypher_templates/bitemporal_query.cypher`（3 种场景模板）
- **配套数据：** `test_data.cypher` 新增 `GRAMMAR_FACT` 关系表 + 6 条测试事实

## 设计决策记录

| 决策 | 原因 |
|------|------|
| 扩展内无表扫描 | SimpleTableFunc + NodeTable::scan() 在 Windows 上导致 SIGSEGV。扫描在上层 Cypher 中完成 |
| 2 个 C++ 函数（非 3 个） | bitemporal_query 的纯 Cypher 实现足够，避免引入 C++ 复杂度和未测试的 RelTable 扫描路径 |
| `detect_turning_points` 展平 LIST + 显式维度 | LadybugDB 不支持 `LIST<FLOAT[768]>` 嵌套类型；展平 row-major + `num_embeddings` + `embedding_dim` 接管维度的控制权和验证 |
| `embedding_dim` 可配置 | 支持 4（ba_ratio/bei_ratio/sentiment/dominance）到 768+（完整 BERT embedding）的任何维度 |
| `direction` = cosine 距离加速度 | 'up' 表示 cosine 偏移在加速（波动增大）、'down' 表示在减速。不是情绪方向 — 已在文档说明并承认限制 |

## 构建命令

```bash
cmake -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_EXTENSIONS="vector;algo;fts;bitemporal" ..
ninja lbug_bitemporal_extension
# Output: extension/bitemporal/build/libbitemporal.lbug_extension
```

## 测试套件

- **IDE 验证：** `node dist-ladybug-0.18.3/verify.js`（19/19 PASS）
- **Bitemporal 专项测试：** `node extension/bitemporal/test/verify_bitemporal.js`（8/8 PASS）
- **CLI 冒烟测试：** `lbug_shell.exe test_honglou.lbug` → `CALL character_similarity(...)` + `CALL detect_turning_points(...)`

## 测试覆盖

| 测试 | 文件 | 验证内容 |
|------|------|----------|
| character_similarity 正常输入 | verify.js + verify_bitemporal.js | ≥1 row，不抛异常；CLI 验证 0.664583 |
| detect_turning_points 正常输入 | verify.js + verify_bitemporal.js | 3 章 4 维 → 返回转折点 |
| detect_turning_points 高阈值 | verify.js + verify_bitemporal.js | threshold=0.5 → 0 rows |
| detect_turning_points 维度不匹配 | verify.js + verify_bitemporal.js | 元素/章节数不匹配 → 0 rows |
| character_similarity 完全相同 | verify_bitemporal.js | similarity_score = 1.0 |
| character_similarity 全零 | verify_bitemporal.js | 表头可见（分母保护生效） |
| GRAMMAR_FACT 表 | verify_bitemporal.js | 纯 Cypher 查询返回 ba_ratio 事实 |

## PR 模板片段

```markdown
## Summary
Add `libbitemporal.lbug_extension` — a new extension providing two
pure-computation table functions for temporal character analysis:
`character_similarity` and `detect_turning_points`.

`bitemporal_query` is provided as pure Cypher templates (no C++ needed).

## Added functions
- `CALL character_similarity(baA,…,domA, baB,…,domB) RETURN *`
- `CALL detect_turning_points(flat_embeddings, N, chapters, D, threshold:=0.3) RETURN *`

## Design
- Pure math — no table scans in the extension; Cypher layer does all scanning
- ~290 lines of C++ (2 functions), ~100 lines of Cypher templates
- `detect_turning_points` supports arbitrary embedding dimensions (4…768+)
- Built and tested on Windows 11 x64 + MSVC 19.50
- 27/27 PASS verification suite (19 IDE + 8 CLI)

## Verification
```bash
node dist-ladybug-0.18.3/verify.js          # 19/19 PASS
node extension/bitemporal/test/verify_bitemporal.js   # 8/8 PASS
```
