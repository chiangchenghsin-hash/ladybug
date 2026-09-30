# hyperalgo 交付包(LadybugDB 0.20.2)

超图算法扩展与工具集:把「pairwise 图看连通」升级为「超图看齐套」。
本包含**预编译扩展**(win_amd64)、**完整源码**、**算法工具链**、**冻结文档**与**黄金图集**。

> 版本:2026-09-10 · 宿主 LadybugDB **0.20.2** · 接口 ABI_VERSION=2
> 语义:P-1 SPEC **v1.1b** · 接口:P-1.5 **v1.2a** · 路线图:升级计划 **v1.3b**

## 包内容

```
README.md                 本文件(快速开始)
API.md                    工具 API 说明(扩展 CALL / CLI / Python 工具 / C++ 核心)
docs/                     冻结文档(P-1 SPEC、P-1.5 契约、升级计划、评审报告)
docs/ladybug-api-reference.md   LadybugDB 0.20.2 API 参考(已含「超图算法 HYPERALGO」章)
bin/win_amd64/            libhyperalgo.lbug_extension + hyperalgo_cli.exe + hyperalgo_tests.exe
hyperalgo-core/           header-only C++17 核心(include/ + test/ + tools/ + golden/)
extension/hyperalgo/      扩展源码(src/ + test/,构建需 ladybug-0.20.2 源码树)
```

## 快速开始(5 分钟)

```sql
-- 1) 加载扩展(按实际路径)
LOAD EXTENSION 'bin/win_amd64/libhyperalgo.lbug_extension';

-- 2) 建数据(最小示例):Agent 节点 + FLOWS 供应关系
CREATE NODE TABLE Agent(id INT64 PRIMARY KEY, stationType STRING, material_id STRING);
CREATE NODE TABLE RoundState(id INT64 PRIMARY KEY, round INT32, agent_id INT64, inventory DOUBLE);
CREATE REL TABLE FLOWS(FROM Agent TO Agent, weight DOUBLE);

-- 3) 投影超图(构建双 CSR + 指纹;后续算法引用图名)
CALL hyper_project('g', ['Agent'], ['FLOWS']) RETURN *;

-- 4) 分析层:齐套连通 / 击垮集 / (k,s)-core / B-回路 / 游走 / 谱
CALL hyper_kitting_cc('g') RETURN node, block_id ORDER BY node;
CALL hyper_hit_set('g') RETURN node;
CALL hyper_b_cycles('g') RETURN cycle_id, edge;

-- 5) 判定层:单站直接上游布尔齐套(含缺料归因)
CALL hyper_kitting_bool('g', ['Agent'], ['FLOWS'], 'RoundState', 1)
RETURN station, no_supplier, kitting, missing ORDER BY station;
```

自检(无需数据库):

```bat
bin\win_amd64\hyperalgo_tests.exe
```

## 构建(可选:从源码重建扩展)

扩展构建需要 **ladybug-0.20.2 源码树**(`extension/hyperalgo` 放入其 `extension/` 下,
并在 `extension/extension_config.cmake` 的 EXTENSION_LIST 加入 hyperalgo),再执行:

```bat
_build_hyperalgo.bat          :: Windows/MSVC:扩展 + E2E runner
hyperalgo-core\_build_test.bat :: 核心 CLI + 单测(独立,无需 ladybug)
bash hyperalgo-core/tools/ci_hyperalgo.sh   :: 全量校验(五段流水)
```

hyperalgo-core 单独构建:`cmake -S hyperalgo-core -B build -DCMAKE_BUILD_TYPE=Release`(C++17)。

## 验证与对拍

- 核心单测与黄金图集:`hyperalgo_tests.exe`
- 黄金图集核对(A/B/C/C1/C2、指纹、布尔化):`hyperalgo-core/tools/golden_check.py`
- HNX `s_connected_components` 对拍:P0-1a,`compare_scc.py`(需 oracle venv,见 `tools/README.md`)
- P0-2/P1a 验收:`p0_acceptance.py`
- 扩展 E2E(含判定层 Cypher 金本互证):在 ladybug 源码树内运行(见 API.md §7)

## 版本与约束

- `*.lbug_extension` 与宿主 LadybugDB 版本**锁步**:本包为 0.20.2 / win_amd64;其他平台需从源码重建。
- 计算后置(backend-first):扩展不参与 Wasm;判定层与算法均在后端原生进程执行。
- 已知未接线:扩展的 `alive/round/material_groups/k_e` 参数、REST 外壳(后端服务范围)。
