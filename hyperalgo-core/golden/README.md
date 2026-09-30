# 黄金图集(FRZ-12 三件套)

依据:P-1 SPEC v1.1 FRZ-12(样例 A/B/C/C1/C2)。每样例交付三件套:

| 文件 | 内容 |
|---|---|
| `*.hif.json` | HIF 交换格式:nodes[{ext_id, role, alive}], edges[{id(=S ext_id), members[{ext_id, role, material_id?}], h[], k_e, edge_alive}] |
| `*.expected.json` | 构造统计 + 指纹 + 算法期望输出(布尔化 / s-cc / kitting-cc / hitting-set / ks-core / b-cycles;与 hyperalgo-core 单测同口径) |
| `*.cypher` | 判定层金本查询(FRZ-11 形态;方言适配注见文件内),CI 逐夜对拍(P-1.5 §4 条款 1) |

- 样例 A:四级链(教科书基准;全退化边)
- 样例 B:菱形供应 + 退化边 + 孤立站
- 样例 C:替代料组/阈值超边(4 料共享装配超边,显式 `k_e=2`;三 alive 模式扫描)
- 样例 C1:同料双供(需求组共享;P0-1 修订建议④反例)
- 样例 C2:异料单供 4 料(默认 `k_e=g(e)`;锁定 v1.0「默认 k_e=1」修复)

## 口径注记(2026-09-10 对拍执行时冻结)

1. **指纹**(FRZ-07.1):`expected.json.fingerprint` = graph_fingerprint(canonical JSON 键按
   UTF-8 字节序排序 + `%.3f` 定点 + SHA256)。权威值只由 C++ 构造层产出;Python `fingerprint_check.py`
   按规范独立复算用于交叉验证(勿手工改 expected 值,改代码后跑 `--write-fingerprint` 回填)。
2. **孤立站计数**(FRZ-01 条款 3):= 无入边节点(`e_S = ∅`)计数,料号成员同样计入
   (样例 B=5、C/C1/C2=4/3/4)。与 FRZ-00「站 = 存在入边的 Agent」的措辞张力已报评审。
3. **`k_e` 默认**:HIF→TSV 转换仅在 `k_e ≠ g(e)`(需求组数)时发 `KE` 行,保证默认语义也被检验。

## 对拍入口

- 黄金图集全量:`python tools/golden_check.py --cli <hyperalgo_cli.exe>`
- 指纹三端:`python tools/fingerprint_check.py <tsv> --cli <...> --expect <扩展端哈希>`
- HNX s-cc 对拍(P0-1a,oracle):`tools/compare_scc.py`
- P0-2/P1a 验收:`tools/p0_acceptance.py`
- 一键逐夜:`bash tools/ci_hyperalgo.sh`(见 `tools/README.md` 的 oracle 环境说明)
