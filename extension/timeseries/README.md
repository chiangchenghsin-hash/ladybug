# LadybugDB Bitemporal Extension — 完整指南

## 快速验证（3 条命令）

```bash
# 1. 建测试库
lbug_shell.exe test_honglou.lbug < test_data.cypher

# 2. 加载扩展
echo "LOAD EXTENSION 'path/to/libbitemporal.lbug_extension';" | lbug_shell.exe test_honglou.lbug

# 3. 运行
CALL character_similarity(0.12,0.08,0.8,0.6, 0.1,0.15,0.5,0.4) RETURN *;
CALL detect_turning_points(
    [0.33,0.07,0.6,0.7, 0.12,0.08,0.8,0.6, 0.06,0.23,-0.3,0.3],
    3, [1,40,80], 4, threshold:=0.01
) RETURN *;
```

---

## 函数 1: character_similarity

**签名：**
```sql
CALL character_similarity(
    ba_ratio_a   DOUBLE, ba_ratio_b   DOUBLE,
    bei_ratio_a  DOUBLE, bei_ratio_b  DOUBLE,
    sentiment_a  DOUBLE, sentiment_b  DOUBLE,
    dominance_a  DOUBLE, dominance_b  DOUBLE
) RETURN similarity_score DOUBLE, ba_ratio_similarity DOUBLE,
         bei_ratio_similarity DOUBLE, sentiment_similarity DOUBLE,
         dominance_similarity DOUBLE, compared_features STRING;
```

**算法：** 逐特征归一化差异相似度 (1 - |a-b| / max(|a|,|b|, 0.001))，4 个特征取平均。

**应用层 Cypher 准备数据：**
```sql
-- 角色A 第40回特征
MATCH (s:Snapshot {character_id: 'honglou-jia-bao-yu', chapter_number: 40})
RETURN s.ba_count*1.0/s.speech_count AS baR,
       s.bei_count*1.0/s.speech_count AS beiR,
       s.sentiment AS sent,
       s.dominance AS dom;

-- 角色B 第40回特征（同上查出后拼接）
-- 结果：(0.12, 0.08, 0.8, 0.6)  vs  (0.1, 0.15, 0.5, 0.4)
-- 调用 CALL character_similarity(0.12,0.08,0.8,0.6, 0.1,0.15,0.5,0.4) → 0.665
```

---

## 函数 2: detect_turning_points

**签名：**
```sql
CALL detect_turning_points(
    flat_embeddings LIST<DOUBLE>,   -- N×D 展平 (row-major)
    num_embeddings  INT64,           -- N
    chapters        LIST<INT64>,     -- N 章节号（同序）
    embedding_dim   INT64,           -- D (e.g. 768)
    threshold := 0.3                 -- [可选] DOUBLE 突变阈值（默认 0.3）
) RETURN chapter_number INT64, embedding_shift DOUBLE,
         significance DOUBLE, direction STRING;
```

**算法：**
1. 从展平 LIST 重建 N 个 D 维向量
2. 连续向量间 cosine distance
3. Min-max 归一化 → significance [0,1]
4. Filter by threshold, sort by significance desc

**应用层 Cypher 准备数据：**
```sql
-- Step 1: 按章节收集 embedding
MATCH (s:Snapshot {character_id: 'honglou-jia-bao-yu'})
WITH s ORDER BY s.chapter_number
WITH COLLECT(s.embedding) AS embeddings,
     COLLECT(s.chapter_number) AS chapters

-- Step 2: 应用层展平 (JS/TS)
--   const flat = embeddings.flat();   // N×768 → 1D array
--   const N = embeddings.length;
--   const D = 768;

-- Step 3: 调用扩展
CALL detect_turning_points(flat, N, chapters, D, threshold:=0.3) RETURN *;
```

**C++ 实现在扩展内完成所有向量运算 — LIST 参数在 bindFunc 中解析，cosine distance + 显著性归一化 + 排序全部在 tableFunc 中完成。**

---

## 双时态查询（纯 Cypher，无需扩展）

`bitemporal_query` 不需要 C++ 扩展。使用纯 Cypher WHERE 子句的过滤能力即可。

**模板1 — 指定角色+回合的事实查询：**
```sql
MATCH (c:Character {id: 'honglou-jia-bao-yu'})
      -[f:GRAMMAR_FACT]->(ch:Chapter)
WHERE ch.chapter_number = 40
  AND f.valid_from <= 40
  AND (f.valid_to = -1 OR f.valid_to >= 40)
RETURN c.id, ch.chapter_number, f.fact_type, f.value,
       f.analysis_version, f.tx_created, f.valid_from, f.valid_to
ORDER BY f.tx_created DESC;
```

完整模板集：[`cypher_templates/bitemporal_query.cypher`](cypher_templates/bitemporal_query.cypher)

---

## 测试数据库

保存为 `test_data.cypher` 并运行 `lbug_shell.exe test_honglou.lbug < test_data.cypher` 以创建具有以下内容的数据库：

- 1 个 Work（红楼梦）
- 5 个 Characters（贾宝玉、林黛玉、薛宝钗、王熙凤、贾母）
- 3 个 Chapters（第 1/40/80 回）
- 8 个 Snapshots（第 1/40/80 回各 2-3 个角色）
- 关系：HAS_SNAPSHOT、CHAPTER_HAS、NEXT_SNAPSHOT、INTERACTS
- 🆕 GRAMMAR_FACT 关系表（6 条双时态事实：ba_ratio、bei_ratio × 角色/章节）
