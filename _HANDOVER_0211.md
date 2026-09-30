# 交接：LadybugDB fork 升级到 0.21.1（2026-09-30）

> 给下一个会话：工作目录建议切到 `C:\Users\chian\Documents\trae_projects\ladybug-0.21.1`。
> 本文是唯一交接入口；先读完再动手。

## 一、任务与结论（TL;DR）

**任务**：把 0.20.2 fork（`ladybug-0.20.2`，含 43+ 算法 / timeseries / fts MeCab+jieba / hyperalgo 超图扩展）升级到上游 0.21.1，方法照搬 0.20.2 的构建链。

**已完成**：定制全部移植到 0.21.1 并提交（git：`8e4b79d` 基线 → `bac3e0b` 移植）；核心 e2e、4 扩展+shell、hyperalgo **全部构建通过**（零代码适配——0.21.1 的 API 变更没伤到我们的定制）；dist 骨架已组装。

**卡点已解（2026-09-30）**：MERGE 规划死循环根因已定位并修复——`planOptionalMatch` 内 `constFilteredVars.insert(collector.getVarNames().begin(), collector.getVarNames().end())`，`getVarNames()` 按值返回被调两次，迭代器区间跨两个临时对象（UB）→ 无限循环/SIGSEGV。引入提交 `58c10a424`（09-24，Arun Sharma）。触发面 = **MERGE/OPTIONAL MATCH 模式带属性谓词 + 非空左计划**（如 `(a)-[r:knows {date: ...}]->(b)` 或 `(b:person {ID: 5})`）；无属性 map 或纯 MERGE 不触发（与 variants 探针完全吻合）。修复已提交本地 `32cf881`，上游 PR：**LadybugDB/ladybug#1083**（fork `chiangchenghsin-hash/ladybug`，分支 `fix/merge-pattern-predicate-ub`，上游提交 `eae998d`）。merge e2e 26/26 全绿。

## 二、目录与 git 状态

| 路径 | 用途 | 状态 |
|---|---|---|
| `ladybug-0.20.2/` | 0.20.2 fork 工作树（来源） | 只读参考，勿动 |
| `ladybug-0.21.0/` | 0.21.0 尝试（已弃：用户确认 0.21.0 有 bug） | 保留作对照（其 e2e 构建可用） |
| `ladybug-0.21.1/` | **当前工作树** | git 2 commits，工作区干净 |
| `_upstream_ext_9884564/` | 上游 extensions@9884564 稀疏检出（ftype/vector/algo 对比基准） | 参考 |
| `_hang_repro/` | 死循环复现脚本 + 日志 | 关键证据 |
| `_official_0211/` | 官方 0.21.1 Windows CLI | 对照 |

`ladybug-0.21.1` git：`8e4b79d` Baseline: pristine 0.21.1 upstream snapshot → `bac3e0b` Port fork customizations onto 0.21.1。回滚锚点就是 baseline。

## 三、已移植的定制清单（0.20.2 → 0.21.1）

1. **hyperalgo-core/**（34 文件，header-only，工作区当前状态非 commit 态）+ **extension/hyperalgo/**（15）+ **extension/timeseries/**（14）——整目录拷贝。
2. **extension/algo/**：用户的 57 算法 fork 整层替换上游 14 文件 GDS 桥接架构；删掉了上游残留的 11 个 gds_* 文件（gds_csr_bridge/leiden/louvain/node2vec/ppr + 2 头 + 4 测试）。**保留**用户版 `download_icebug.cmake`（icebug 12.8，与用户 algo CMakeLists 自洽；升 13.4 另做）。
3. **extension/fts/**：`dict_dir_resolver.{h,cpp}` 新增；`tokenize.cpp`/`CMakeLists.txt`/`src/utils/CMakeLists.txt`/`error.test`/`fts_japanese.test` 直接覆盖；`create_fts_index.cpp`/`fts_config.cpp`/`query_fts_bind_data.cpp`/`fts_utils.h`/`fts_utils.cpp` 三方合并（git merge-file；上游已收编用户的 wildcard 保护/ignore_pattern/jieba 空白过滤，合并后只余 mecab 块+include 差异）。
4. **extension/vector/**：`hnsw_index.cpp`（NULL-embedding shrink 防护）+`update.test` 三方合并。
5. **核心补丁 5 处**：`CMakeLists.txt` OpenSSL QUIET 化、`src/extension/CMakeLists.txt` httplib OpenSSL_FOUND 门控、`cmake/BundleStaticLibrary.cmake` emar 例外、`Makefile` EXTENSION_LIST +hyperalgo、`.gitignore`。
6. **扩展注册 2 处**：`extension/CMakeLists.txt` + `extension/extension_config.cmake` 加 hyperalgo/timeseries。
7. **third_party/mecab/**（68 文件，56MB）——**必须有**，否则 fts 配不过（`extension/fts/CMakeLists.txt` 引用 `${PROJECT_SOURCE_DIR}/third_party/mecab`）。0.21.1 快照不含它。
8. **tools/wasm/**（54 文件，含用户 bundle.mjs）——gitlink 与上游相同（`aa72699`），从 0.20.2 直接拷。
9. **构建/打包脚本**：`_build_v0211t.bat`（核心 e2e → build_v0211t）、`_build_fts_vector.bat`（algo;timeseries;fts;vector + shell → build_v0211）、`_build_hyperalgo.bat`（→ build_hyperalgo）、`_build_wasm.bat`（→ build\wasm，emsdk 路径同 0.20.2）、`_pack_0211.py`、`dist-ladybug-0.21.1/smoke-0.21.1.js`。
10. **dataset/**：0.21.1 快照自带（645 文件，含 tinysnb）——不用拷（0.21.0 树当时是空的才需要）。

**兼容性结论**：存储版本 v47 不变（0.20.0→0.21.1 全是 v47），老库零迁移；扩展 ABI 版本 0.21.0→0.21.1，扩展全部重编（已做）。

## 四、构建状态

| 链 | 结果 | 产物位置 |
|---|---|---|
| 核心 e2e（`_build_v0211t.bat`） | ✅ BUILD_EXIT=0 | `build_v0211t/src/Release/e2e_test.exe` |
| 扩展+shell（`_build_fts_vector.bat`） | ✅ BUILD_OK | `build_v0211/src/Release/lbug_shell.exe`；`extension/{algo,fts,vector,timeseries}/build/Release/lib*.lbug_extension` |
| hyperalgo（`_build_hyperalgo.bat`） | ✅ BUILD_EXIT=0 | `build_hyperalgo/`（libhyperalgo 待拷入 dist） |
| wasm（`_build_wasm.bat`） | ✅ NINJA_EXIT=0（需 PYTHONUTF8=1） | `tools/wasm/package/`（bundle.mjs 产物）→ `dist-ladybug-0.21.1/wasm-deploy/` |

**dist-ladybug-0.21.1/** 已装齐：lbug_shell.exe + libalgo/libfts/libtimeseries/libvector/libhyperalgo.lbug_extension + fts_dict/ + fts_dict-ipadic/ + wasm-deploy/ + README.md/FIXES.md/ladybug-api-reference.md（**版本标注已更新至 0.21.1**）+ smoke-0.21.1.js（12/12）。已打包 releases/0.21.1/。

## 五、卡点：MERGE 规划死循环（✅ 已解决 2026-09-30）

### 根因（已确证，心跳埋点定位）
`src/planner/plan/plan_subquery.cpp` `planOptionalMatch` 的 inner-selectivity 门控里：
```cpp
constFilteredVars.insert(collector.getVarNames().begin(),
    collector.getVarNames().end());
```
`DependentVarNameCollector::getVarNames()` **按值返回**却被调用两次 → `begin()`/`end()` 来自两个不同临时 `unordered_set` → insert 迭代器区间跨容器（UB）→ **单核死循环或 SIGSEGV**（堆布局决定表现，故有"空表过/有数据挂"的假象）。

- 引入提交：`58c10a424`（2026-09-24 18:57 PDT，Arun Sharma，"selective correlated-optional unnest..."）。二分窗口曾误判 09-27~28，实际因 dev.20260927 撞上 #1059（COPY 崩溃）无法测 MERGE，把窗口推后了。
- 触发面：**MERGE/OPTIONAL MATCH 的模式带属性 map/谓词**（产生 legPredicates）且**左计划非空**。Qa（纯 MERGE，空左）不触发；Qd/Qb 无模式属性不触发；`{ID: 5}`/`{date: ...}` 均触发。variants 探针与此完全吻合。
- 官方二进制同挂 → 纯上游 bug（基线 blob `9b269c4` 同款代码）。

### 修复
本地 `32cf881`；上游 PR **https://github.com/LadybugDB/ladybug/pull/1083**（fork `chiangchenghsin-hash/ladybug`，分支 `fix/merge-pattern-predicate-ub`，提交 `eae998d`，英文礼貌措辞，含复现/根因/测试说明）。修法：`auto predVarNames = collector.getVarNames();` 绑定一次再取迭代器区间，附注释防回归。
回归：merge 全家（dml_rel/dml_node/transaction）26/26 绿；全量 e2e 见第六节。

### 复现（保留备查）
```cypher
MATCH (a:person), (b:person) WHERE a.ID = 0 AND b.ID = 5
MERGE (a)-[r:knows {date: date('2022-02-02')}]->(b);
```
tinysnb 数据集即可。`_hang_repro/variants.js|repro_official.js` 是当时的探针脚本（注意 Qb 挂真因是 `(b:person {ID: 5})` 属性 map，不是 rel 形状本身）。

## 六、下一步（按序）

1. ~~根因~~ ✅ 已定位：`getVarNames()` 双调用 UB（见第五节）。
2. ~~修复~~ ✅ 本地 `32cf881`。
3. ~~上报上游~~ ✅ PR LadybugDB/ladybug#1083（英文礼貌，含复现/根因/测试）。跟进 review 即可。相关 landscape：#1059（COPY 未 checkpoint CSR 扫描越界，open）、#1082（参数化 QUERY_FTS_INDEX 同连接第 2 次 SIGSEGV，0.21.0/0.21.1 都中——会影响 fts 冒烟！）、#1062（#1046 的 Q6 性能回归修复，open）。
4. **恢复测试**：merge e2e 26/26 ✅；**全量 e2e** ≈1500+ 通过、仅 4 失败——3 个 `extension~extension.*`（需联网 `INSTALL HTTPFS/sqlitescanner`，环境问题）+ 1 个 `AnonymousParquetDeleteReload`（**fixture 缺失**：`python scripts/generate-dictionary-bug-fixture.py` 生成 A/B/A_TO_B.parquet 后转绿；需 pyarrow——Python 3.14 记得 `pip install pyarrow -i https://pypi.org/simple`，清华源无轮子）。注意：`e2e_test.exe` 必须带 `--gtest_filter="*"` 才会注册用例，裸跑是 0 tests。dist 冒烟 **12/12**（core@0.21.1，含 hyperalgo 链图金测）；hyperalgo-core 单测 BUILD_OK/ALL TESTS PASSED。
5. ~~补完 dist~~ ✅：libhyperalgo 已入 dist；wasm 已构建（注意两点：`collect-single-file-header.py` 需 `PYTHONUTF8=1` 否则 GBK 解码炸；ninja 用 VS CMake Ninja）→ `node tools/wasm/bundle.mjs`（先 `npm i`）→ `package/`（worker 25.1MB）→ `wasm-deploy/`（结构同 0.20.2 dist，桥接层文件取自 `ladybug-0.20.2/dist-ladybug-0.20.2/wasm-deploy/`）；README/FIXES/api-reference/wasm-deploy 文档版本均已更新。
6. ~~打包~~ ✅：`python _pack_0211.py` → releases/0.21.1/ladybug-0.21.1-windows.zip(116.9MB) + -ubuntu.zip(181.8MB)。已抽验 windows zip 含全部更新文件（README/FIXES/api-reference 0.21.1 口径、libhyperalgo、smoke-0.21.1.js、wasm-deploy）。

**终态补充**：hyperalgo 金测 CI **ALL CHECKS PASSED**（3/3；注意 CI 用 `build_hyperalgo/src/Release/e2e_test.exe`，改 core 源后必须重编它；`LOAD_DYNAMIC_EXTENSION hyperalgo` 在 `extension/hyperalgo/build/` 找扩展，MSVC 产物在 `build/Release/`，需拷一份过去）。hyperalgo-core 单测 ALL TESTS PASSED。api-reference.md 已由 subagent 更新（26 处编辑：版本口径/算法数 57/FTS 上游收编说明/存储兼容表等）。

## 七、环境备忘

- 构建：VS 2026 BuildTools（`C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\...`），googletest 本地源 `C:/Users/chian/Documents/trae_projects/ladybug-0.19.0/build_algo/_deps/googletest-src`。
- emsdk：`C:\emsdk`（wasm 链）；node 24 在 PATH。
- npm：`@ladybugdb/core` 有 0.21.1 + 0.21.0-dev.202609XX 每日构建（二分用）+ 0.21.2 尚未发布；上游 main 只比 v0.21.1 多 1 个 rust_api 提交（**无修复**）。
- e2e 跑法：`E2E_TEST_FILES_DIRECTORY=test/test_files ./build_v0211t/src/Release/e2e_test.exe --gtest_filter="..."`（cwd=树根；dataset 走编译期 LBUG_ROOT_DIRECTORY 自动解析）。
- 行尾噪声：对比 extension 树时用 `diff --strip-trailing-cr`；`extension/` 在 0.20.2 仓库里是 submodule（gitlink 9884564），用户的定制全部是 working-tree 未跟踪状态。
- `_pack_0211.py` 约定：二进制 STORED/文本 DEFLATE、mtime 归一 1980-01-01、SKIP build*/releases/.venv-oracle/vendor/*.log。
