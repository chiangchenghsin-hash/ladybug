// ============================================================
// bitemporal_query — 纯 Cypher 双时态查询模板
// ============================================================
// 此功能不需要 C++ 扩展。Cypher WHERE 子句的过滤能力完全足够。
//
// 使用方法: 将 $character_id / $chapter / $tx_time 替换为实际值后执行。
// ============================================================

// ========== 模板1: 指定角色+回合的双时态查询 ==========
// 查找在某回合 (e.g. 第40回) 的"有效时间窗口"内的所有事实

MATCH (c:Character {id: $character_id})
      -[f:GRAMMAR_FACT]->(ch:Chapter)
WHERE ch.chapter_number = $chapter
  AND f.valid_from <= $chapter
  AND (f.valid_to = -1 OR f.valid_to >= $chapter)
  AND ($tx_time IS NULL OR
       (f.tx_created <= $tx_time AND
        (f.tx_expired IS NULL OR f.tx_expired > $tx_time)))
RETURN c.id         AS character_id,
       ch.chapter_number AS chapter,
       f.fact_type  AS fact_type,
       f.value      AS value,
       f.analysis_version AS analysis_version,
       f.tx_created AS tx_created,
       f.valid_from AS valid_from,
       f.valid_to   AS valid_to
ORDER BY f.tx_created DESC;


// ========== 模板2: 跨回合双时态对比 ==========
// 对比同一角色在 chapter_start vs chapter_end 的事实差异

MATCH (c:Character {id: $character_id})
      -[f1:GRAMMAR_FACT]->(ch1:Chapter)
WHERE ch1.chapter_number = $chapter_start
  AND f1.valid_from <= $chapter_start
  AND (f1.valid_to = -1 OR f1.valid_to >= $chapter_start)
MATCH (c)-[f2:GRAMMAR_FACT]->(ch2:Chapter)
WHERE ch2.chapter_number = $chapter_end
  AND f2.valid_from <= $chapter_end
  AND (f2.valid_to = -1 OR f2.valid_to >= $chapter_end)
  AND f1.fact_type = f2.fact_type
RETURN f1.fact_type                              AS fact_type,
       f1.value                                  AS value_before,
       f2.value                                  AS value_after,
       (f2.value - f1.value)                     AS delta,
       CASE WHEN f1.value != 0
            THEN (f2.value - f1.value) / abs(f1.value)
            ELSE 0.0 END                         AS pct_change
ORDER BY abs(delta) DESC;


// ========== 模板3: 角色群像 — 同一回合多个角色的双时态快照 ==========

MATCH (c:Character)-[f:GRAMMAR_FACT]->(ch:Chapter)
WHERE ch.chapter_number = $chapter
  AND f.valid_from <= $chapter
  AND (f.valid_to = -1 OR f.valid_to >= $chapter)
RETURN c.name       AS character_name,
       f.fact_type  AS fact_type,
       f.value      AS value,
       f.analysis_version AS analysis_version
ORDER BY c.name, f.fact_type;
