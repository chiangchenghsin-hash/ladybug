// 样例 A 金本(FRZ-11 形态;P-1 SPEC v1.1)——单站直接上游布尔齐套(非递归)
// 口径:判定层 v0 = 单站直接上游;整链递归移交 P3a(FRZ-11 v1.1)
// 场景:4 级链 R→A→B→C→D;全部 alive → 每站齐套;断 R → A 缺料(B/C/D 单层口径各自判定)
//
// ⚠ 方言适配(2026-09-10 实测,LadybugDB 0.20.2 ANTLR 语法):
//   FRZ-11 草案文本中的 `size([x IN sup WHERE ...])` 列表推导式在 0.20.2 语法中**不可解析**
//   (Cypher.g4 无 oC_ListComprehension 规则;ALL/ANY/NONE/SINGLE 量词与列表函数可用)。
//   下方为等价可执行写法:`list_filter` + lambda(已由 extension/hyperalgo E2E
//   HyperalgoGoldenJudge 逐句执行验证)。FRZ-11 文本待下版 errata 同步。
//
// 单站金本模板(任意 $stationId, $round;表名按 NASH 附录 A:Agent / RoundState / FLOWS)
// MATCH (s:Agent) WHERE s.id = $stationId
// OPTIONAL MATCH (u:Agent)-[f:FLOWS]->(s)
// OPTIONAL MATCH (rs:RoundState) WHERE rs.round = $round AND rs.agent_id = u.id
// WITH s, collect({uid: u.id, inv: coalesce(rs.inventory, 0.0)}) AS sup
// RETURN size(list_filter(sup, x -> x.uid IS NOT NULL)) = 0                 AS no_supplier,
//        all(x IN sup WHERE x.uid IS NULL OR x.inv > 0)                     AS kitting,
//        list_transform(list_filter(sup, x -> x.uid IS NOT NULL AND x.inv <= 0),
//                       y -> y.uid)                                        AS missing;
//
// 期望(4 级链,全库存 >0):no_supplier=False,kitting=True,missing=[]
// 期望(站 A,上游 R 库存 =0):no_supplier=False,kitting=False,missing=[R.id]
//
// CI 对拍条目(P-1.5 §4 条款 1):hyper_kitting_cc('g') 与上述 Cypher 对同轮数据互证;
// 指纹一致(FRZ-07.1)为前提。可执行对拍 = extension/hyperalgo/test/test_files/hyperalgo.test
// 的 HyperalgoGoldenJudge 用例(判定层 Cypher 与分析层 C++ 同图断言)。
//
// v1.1b 决策(2026-09-10):判定层 v0 主实现 = 扩展 CALL
//   CALL hyper_kitting_bool('g', ['Agent'], ['FLOWS'], 'RoundState', $round)
//   RETURN station, no_supplier, kitting, missing;
// 本文件的 Cypher 保留为对照 oracle,二者在 HyperalgoGoldenJudge 同图互证(含孤立站与断供两态)。
