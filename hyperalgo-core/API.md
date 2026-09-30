# hyperalgo 工具 API 说明

> 版本:2026-09-10 · 适用 LadybugDB **0.20.2** · 契约版本 ABI_VERSION=2(FRZ SPEC **v1.1b** / P-1.5 **v1.2a** / 升级计划 **v1.3b**)
> 适用范围:hyperalgo 扩展使用者(后端/分析开发者)、对拍与验收工具使用者
> 语义权威:构造语义以《P-1 超图构造规范 SPEC》v1.1b 为准;接口以《P-1.5 后端服务接口契约》v1.2a 为准

---

## 0. 交付物总览

| 组件 | 位置 | 形态 | 用途 |
|---|---|---|---|
| hyperalgo 扩展 | `bin/win_amd64/libhyperalgo.lbug_extension` | 动态库 | `LOAD EXTENSION` 后以 `CALL` 使用(主接口) |
| hyperalgo-core | `hyperalgo-core/include/hyperalgo/` | header-only C++17 | 算法与数据结构(零依赖;扩展与 CLI 共用同一实现) |
| 核心 CLI | `bin/win_amd64/hyperalgo_cli.exe` | 命令行 | 对拍/批处理入口(TSV 输入) |
| 核心单测 | `bin/win_amd64/hyperalgo_tests.exe` | 命令行 | 自检(黄金图集 + 算法单测,无 DB 依赖) |
| 对拍/验收工具 | `hyperalgo-core/tools/*.py`、`ci_hyperalgo.sh` | Python / Bash | HNX 对拍、指纹核验、黄金图集核对、P0/P1a 验收 |
| 黄金图集 | `hyperalgo-core/golden/` | JSON + Cypher | FRZ-12 三件套(A/B/C/C1/C2) |

**数据模型(前置)**:属性图需具备
- 节点表 `Agent(id INT64 PK, stationType STRING, material_id STRING)`——超边成员/装配站;
- 关系表 `FLOWS(FROM Agent TO Agent, weight DOUBLE)`——`u→S` 物料流,`S` 为装配站(超边 id),`u` 为上游(超边成员);
- 状态表 `RoundState(round, agent_id, inventory DOUBLE)`——判定层库存(仅 `hyper_kitting_bool` 需要)。

**通用约定**
- 节点一律返回 **ext id** 字符串,格式 `<tableID>:<offset>`(如 `0:3`);业务 id 映射由 REST/绑定层完成。
- 分析层 CALL 消费 `hyper_project` 物化的图句柄:先 `CALL hyper_project('<图名>', 节点表, 关系表)`,后续 CALL 以图名引用;句柄按图名缓存于进程内,重投影覆盖。
- 错误以异常抛出,绑定层统一映射(契约 §1.1):如未投影时报
  `Binder exception: hyperalgo: projected hypergraph '<g>' not found; run CALL hyper_project('<g>', node_tables, rel_tables) first`。
- v1 现状(已知边界):所有分析 CALL 按**全活**计算;k_e 默认 `|e_S|`(站成员各自独立需求组);`alive/round/material_groups/k_e` 覆盖参数为 v1.1 扩展(CLI 已支持 GROUP/KE,扩展未接线)。

---

## 1. 构造层 API

### `CALL hyper_project(graph, node_tables, rel_tables)`

投影属性图为超图、构建双 CSR 并缓存句柄(FRZ-02/FRZ-10)。

| 参数 | 类型 | 说明 |
|---|---|---|
| graph | STRING | 图名(后续 CALL 的引用键) |
| node_tables | LIST\<STRING\> | 节点表名,如 `['Agent']` |
| rel_tables | LIST\<STRING\> | 关系表名,如 `['FLOWS']` |

返回(单行):

| 列 | 类型 | 说明 |
|---|---|---|
| graph | STRING | 图名回显 |
| n_nodes | UINT64 | CSR 节点数 = FLOWS 端点去重数(孤立站不入 CSR) |
| n_edges | UINT64 | 超边数 = 有入边的站数 |
| isolated_stations | UINT64 | 无入边节点计数(FRZ-01 条款 3;料号亦计入) |
| degenerate_edges | UINT64 | `\|e_S\|=1` 的超边数(单源直供) |
| fingerprint | STRING | graph_fingerprint(FRZ-07.1:canonical JSON+SHA256,键按 UTF-8 字节序、`%.3f` 定点) |

```sql
LOAD EXTENSION 'bin/win_amd64/libhyperalgo.lbug_extension';
CALL hyper_project('g', ['Agent'], ['FLOWS']) RETURN *;
-- g|4|3|1|3|264ec0ae...
```

> 注:`hyper_project` 为 GDS 表函数,必须带 `RETURN`(可用 `RETURN *`)。

---

## 2. 分析层 API(P0~P2)

以下全部消费 `hyper_project` 缓存的句柄;结果为 **bind 期**计算(小图交互友好,大图建议走批处理)。

### `CALL hyper_s_cc(graph, s) → node, component_id`

s-walk 连通(s-linegraph 退化对照;HNX 同语义,可对拍):两节点 s-邻接 ⟺ 共现于 ≥ s 条超边;不判超边完整性。`s ≥ 1`。

### `CALL hyper_kitting_cc(graph) → node, block_id`

齐套连通(P0-1b 主交付):全活状态下,存活超边子图上的连通块。仅成员侧节点出行;纯头节点/孤立站不出行。块数/大小分布 = 断供影响面。

### `CALL hyper_hit_set(graph) → node`

贪心击垮集(minimum hitting set,近似比 1+ln n):按选择序输出节点(ext id)。语义 = 移除后每条存活超边被击垮(活需求组数 < k_e)。

### `CALL hyper_ks_core(graph, k, s) → node, layer`

(k,s)-core 双参数剥离(Rust `get_core` 同口径):节点保留条件「活跃超边数 ≥ k」且所在超边「边大小 ≥ s」;同一轮内同时剥。`layer` 0 = 最外层;只输出被剥离节点,核心子图不输出。

### `CALL hyper_b_cycles(graph) → cycle_id, edge`

有向超图 B-回路(Ausiello 口径,FRZ-08):head(e)=S、tail(e)=成员,`head(eᵢ)∩tail(eᵢ₊₁)≠∅` 且首尾闭合,超边互异。每行一个 (回路号, 超边) 对;`edge` = S 的业务 ext id。

### `CALL hyper_pr_walk(graph, alpha, max_iter, tol) → node, score`

两步游走 PageRank(含 dangling 节点质量再分配)。`alpha` = 阻尼(如 0.15)、`max_iter` 上限、`tol` 收敛阈值。

### `CALL hyper_fiedler(graph) → node, score`

Zhou 2006 归一化拉普拉斯 Fiedler 向量(稠密 Jacobi)。**韧性加边候选评分**由绑定层对候选边两端取 `(f[u]-f[v])²` 得到(核心不单独输出边评分)。

---

## 3. 判定层 API(P3a-v0)

### `CALL hyper_kitting_bool(graph, node_tables, rel_tables, state_table, round)`

单站直接上游布尔齐套(FRZ-11 v1.1b 真值表),一次返回全部站。

| 参数 | 类型 | 说明 |
|---|---|---|
| graph | STRING | 图名(标识;本身不查注册表) |
| node_tables | LIST\<STRING\> | 节点表(如 `['Agent']`) |
| rel_tables | LIST\<STRING\> | 关系表(如 `['FLOWS']`) |
| state_table | STRING | 状态表名(需含 `round/agent_id/inventory` 列,附录 A) |
| round | INT64 | 目标轮次 |

返回(每站一行):

| 列 | 类型 | 说明 |
|---|---|---|
| station | INT64 | 站业务 id(`Agent.id`;表无 `id` 属性时回退 offset) |
| no_supplier | BOOL | 无上游(真值表第 1 行:孤立站) |
| kitting | BOOL | 全部直接上游 `inventory > 0`;无上游恒 true |
| missing | STRING | 缺料上游业务 id 列表,形如 `[0]` / `[0,3]` / `[]`(v0 绑定层表示,REST 转数组) |

**站集合** = 有入边节点(超边头)∪ 无入边且非任何超边成员的节点(孤立站);纯成员节点(料号/中间站)不出行。
**缺 RoundState 行** → 该上游视作 `inv=0`(缺料,与真值表第 4 行一致)。

```sql
CALL hyper_kitting_bool('g', ['Agent'], ['FLOWS'], 'RoundState', 1)
RETURN station, no_supplier, kitting, missing ORDER BY station;
-- 1|False|True|[]
-- 4|True|True|[]
```

---

## 4. 核心 CLI(`hyperalgo_cli`)

离线对拍/批处理入口;同一算法实现的另一出口。

```
hyperalgo_cli <cmd> <input.tsv> [args...]
```

**输入 TSV**(逐行,`#` 注释):

| 行 | 格式 | 说明 |
|---|---|---|
| NODE | `NODE <ext_id> <alive:0/1>` | 节点存活(缺省 0;须显式声明) |
| FLOW | `FLOW <u_ext> <S_ext> <weight>` | 供应流(并行边按 SUM 聚合) |
| GROUP | `GROUP <member_ext> <material_id>` | 料号成员分组(FRZ-00 条款 3;缺省站成员各自独立) |
| KE | `KE <S_ext> <k_e>` | 超边阈值覆盖(缺省 k_e = g(e)) |

**命令**:

| 命令 | 输出 |
|---|---|
| `scc <s>` | `node<TAB>component_id`(全部节点) |
| `kitting` | `node<TAB>block_id`(仅覆盖节点) |
| `hitset` | `node`(贪心选择序) |
| `kscore <k> <s>` | `node<TAB>layer` |
| `bcycles` | `cycle_id<TAB>edge` |
| `booleanize` | `S<TAB>groups<TAB>k_e<TAB>alive_groups<TAB>alive`(FRZ-05 阶段 B/C) |
| `pr <alpha> <maxiter> <tol>` | `node<TAB>score` |
| `fiedler` | `node<TAB>score` |
| `fp` | graph_fingerprint(hex) |

退出码:`0` 成功;`2` 参数/文件错误(未知命令、打不开输入)。

---

## 5. Python 工具

| 脚本 | 用法 | 退出码 |
|---|---|---|
| `tools/compare_scc.py` | `python compare_scc.py <tsv> <s> --hnx-path <hnx> --cli <exe>`;C++ vs HNX `s_connected_components` 分量划分对拍 | 0 一致 / 1 不一致 |
| `tools/fingerprint_check.py` | `python fingerprint_check.py <tsv> --cli <exe> [--expect <sha256>]`;按 FRZ-07.1 Python 独立复算 vs C++ 出口(可再比扩展端值) | 0 一致 / 1 不一致 |
| `tools/golden_check.py` | `python golden_check.py --cli <exe> [--write-fingerprint]`;FRZ-12 黄金图集全量核对(HIF→TSV→CLI vs expected.json) | 0 全过 / 1 有 FAIL |
| `tools/p0_acceptance.py` | `python p0_acceptance.py --cli <exe> [--random 200] [--require-exact]`;P0-2 回路/断环 + P1a 击垮集对拍 | 0 通过 / 1 失败 |
| `tools/ci_hyperalgo.sh` | `bash ci_hyperalgo.sh`;逐夜五段流水(单测→金本→HNX→P0/P1a→E2E) | 0 全绿 |

HNX oracle 环境(Python 3.14 实测):`python -m venv .venv-oracle` + 依赖 **pandas<3**(HNX 2.4.3 与 pandas 3 不兼容),详见 `tools/README.md`。

---

## 6. C++ 核心库(hyperalgo-core)

`namespace hyperalgo`,header-only C++17,零依赖;扩展与 CLI 共用(纪律 6:唯一算法实现)。接口冻结见 P-1.5 §1.2。

```cpp
#include <hyperalgo/hyperalgo.hpp>
using namespace hyperalgo;

// 构造
std::pair<HypergraphCSR, IdMap> build_csr_from_flows(const std::vector<FlowTriple>& flows);
void HypergraphCSR::validate();                       // I1~I6,违反抛异常

// 状态与布尔化(FRZ-05 阶段 B/C)
NodeState / EdgeState;  BooleanizeParams{ material_of_member, extra_groups, k_e_override, k_e_per_edge };
std::vector<EdgeGroupStat> edge_group_stats(csr, ns, params);   // g(e)/k_e/活组数/存活
EdgeState compute_edge_state(csr, ns, params);

// 指纹(FRZ-07.1)
std::string canonical_json(csr, idmap, node_state?, edge_state?);
std::string fingerprint(csr, idmap, node_state?, edge_state?);  // SHA256 hex

// 算法(签名与语义见 P-1.5 §1.2)
s_connected_components(csr, s)                         // P0-1a
kitting_components(csr, ns, es)                        // P0-1b
b_cycles(csr)                                          // P0-2
hitting_set_greedy(csr, ns, es)                        // P1a
ks_core_layers(csr, k, s)                              // P1b
fiedler_vector(csr); fiedler_candidate_scores(csr, candidates)   // P1c
pagerank_two_step(csr, alpha, max_iter, tol)           // P2
component_sizes(comp_id)                               // 辅助
```

约定:错误抛 `std::runtime_error`(绑定层 catch);`node_alive/edge_alive` 传入非空产出 snapshot_fingerprint,否则 graph_fingerprint。

---

## 7. 构建与测试

**Windows(MSVC,已验证)**

```bat
:: 扩展 + E2E runner
_build_hyperalgo.bat
:: 核心 CLI + 单测
hyperalgo-core\_build_test.bat
:: 全量校验(五段)
bash hyperalgo-core/tools/ci_hyperalgo.sh
```

E2E 单跑:

```bash
E2E_TEST_FILES_DIRECTORY=extension/hyperalgo/test/test_files \
  build_hyperalgo/src/Release/e2e_test.exe --gtest_filter="*Hyperalgo*"
```

**其他平台**:hyperalgo-core 用 `cmake -S hyperalgo-core -B build`(C++17,MSVC 下需 `/utf-8`);扩展构建见根 `CMakeLists.txt` 的 `build_extension_lib` 机制与 `extension/extension_config.cmake`(EXTENSION_LIST 含 hyperalgo)。

**测试覆盖**:核心单测(黄金图集 + 7 算法)、扩展 E2E 3 用例(链/环/判定层金本互证)、黄金图集核对(A/B/C/C1/C2、指纹)、HNX 对拍、P0/P1a 验收。

---

## 8. 版本与兼容性

| 项 | 值 |
|---|---|
| 宿主 | LadybugDB 0.20.2(`*.lbug_extension` 与宿主版本锁步) |
| 接口 ABI | ABI_VERSION = 2(含 `edge_business_ids`/`edge_thresholds`/`member_roles`/`EdgeState`) |
| 构造语义 | P-1 SPEC v1.1b(FRZ-00 ~ FRZ-12) |
| 接口契约 | P-1.5 v1.2a |
| 部署约束 | 计算后置(backend-first);扩展不参与 Wasm 构建(升级计划 v1.2 ⑦) |
| 已知未接线 | `alive/round/material_groups/k_e` 扩展参数、REST 外壳(`/v1/judge/kitting-bool` 等)属后续版本 |

---

## 9. 引用文档

| 文档 | 说明 |
|---|---|
| `docs/ladybug-api-reference.md` | LadybugDB 0.20.2 Cypher API 参考(**已含「超图算法 HYPERALGO」章**,面向 AI Agent/使用者) |
| `docs/P-1-超图构造规范-SPEC.md` | 构造语义权威(超边/布尔化/指纹/B-回路/真值表) |
| `docs/P-1.5-后端服务接口契约.md` | C++ 接口冻结 + REST 端点表 + CI 条款 |
| `docs/超图算法升级计划.md` | 路线图与验收标准 |
| `docs/评审报告-超图升级计划.md` | 问题清单与修订依据 |
| `hyperalgo-core/tools/README.md` | 对拍工具与 oracle 环境说明 |
| `hyperalgo-core/golden/README.md` | 黄金图集口径注记 |
