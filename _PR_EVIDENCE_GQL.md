# GQL 扩展 PR 证据包(门禁四件实跑 + 可粘贴英文段)

> 用途:发 PR 前的实测证据攒档,写 PR description 时直接取第五节英文段。
> 数据全部来自 2026-10-02 本机实跑;原始 log 在仓根 `_gate_*.log`(未跟踪,不入库)。
> 环境:Windows 11 / MSVC 18 BuildTools (VS 2026) / `build_v0211t` Release /
> HEAD = `5f28079`(本地提交链,未 push)。
> 口径出处:`docs/gql_methodology.md` #9 执行闭环;0 静默错答、TCK 不降。

## 一、门禁结果(2026-10-02 实跑)

| # | 门禁 | 命令 | 结果 | 证据 |
|---|---|---|---|---|
| 1 | 构建 | `_build_gql.bat`(configure + target `e2e_test` `lbug_gql_extension`) | ✅ 零编译/链接错误(LNK4217/LNK4006 为本仓已知链接噪音) | `build_gql_build.log` |
| 2 | 自测双跑 | `E2E_TEST_FILES_DIRECTORY=extension ./build_v0211t/src/Release/e2e_test.exe --gtest_filter="gql~test~test_files~*"` | ✅ **161/161**(18 套件,14.7s,exit 0) | `_gate_selftest.log` |
| 3 | TCK | `python extension/gql/test/tck/run_tck.py` | ✅ 口径不降:**190 绿(70 过 + 120 note)/ 9 挂 / 7 跳**(206 场景);wrong-GQLSTATUS=**0** | `_gate_tck.log` + `extension/gql/test/tck/REPORT.md` |
| 4 | 全量引擎回归 | `E2E_TEST_FILES_DIRECTORY=test ./build_v0211t/src/Release/e2e_test.exe --gtest_filter="*"` | ✅ **1972/1975**(487 套件,746.8s);3 挂=已知 INSTALL 例(构建配置所致,见下),与基线一致 | `_gate_engine.log` |

门禁 4 的 3 挂(全数过 log + 源码 + configure log 核对,机制已钉死):
`test_files~extension~extension.LoadNotInstalledExtension` /
`ForceInstallExtension` / `UninstallExtensionError`,死在 `INSTALL HTTPFS;`/
`INSTALL neo4j;` → `'https' scheme is not supported.`(`third_party/httplib/httplib.h:8890`)。
**与网络无关**(机器 DNS/网络正常,有网照挂):本构建 configure 时
`find_package(OpenSSL 3 QUIET)` 未命中(`build_gql_configure.log`:
"OpenSSL not found; building without HTTPS extension download support"),
httplib 无 SSL 编译,https 下载在发请求前即被拒。装 OpenSSL 3 可让这 3 例
走真下载路径(可选,非门禁项)。旧口径「需外网必挂」是错归因,已更正。

构建产物(最后一次验证跑):`lbug.lib` 21:01:24 → `libgql.lbug_extension` 21:01:31 →
`e2e_test.exe` 21:01:48,`ARTIFACT_GATE_OK`。**首轮发现的 MSBuild 并行多目标漏链
已修**(`82e5107`):假 `proj -> exe` 完成消息、exe 旧于 lib——现在 `_build_gql.bat`
预删产物强制真链接 + 顺序构建(e2e_test 最后)+ 产物门(stale exe 响亮拒绝),
该失效模式无法再冒充新构建。(修复中另抓到一次产物门误报:librarian 重归档会抬
lbug.lib 的 mtime,故门只严查 exe、DLL 存在即可——坑记于 bat 注释。)

### 自测分套件计数(与 `_HANDOVER_GQL.md` 记录咬合)

basic 11 / comparebridge 11 / groupby 4 / jsonagg 9 / labels 3 / listguard 3 /
multihop 14 / orderability 5 / path 18 / qpibind 8 / relname 3 / routing 4 /
schema 6 / schemapath 12 / select 7 / smallmodes 8 / unsupported 28 / write 7
= **161**

### TCK 9 挂构成(全响亮;0 静默错答;三档码断言 wrong-GQLSTATUS=0)

- **parse-error 4**(语料自身问题,产品无罪;问题语料三件套留痕于 REPORT.md):
  Aggregation1 [1][2] — setup 用 openCypher `CREATE (`;
  Aggregation3 [2] — setup 用 openCypher `UNWIND`;
  create_graphs_Create2 [8] — `CREATE GRAPH ANY AS COPY OF` 文法歧义。
- **rejected-by-layer 5**(设计内拒绝,响亮报错):
  create_graph_types_Create1 [4][5] — 多标签节点类型(引擎单标签);
  create_graphs_Create2 [4] `LIKE` / [5][6] `AS COPY OF` — 图复制。
- 7 跳 = 1 记法不支持 + 6 能力标签(@MinNodeLabelsZero / @MaxNodeLabelsGTOne,
  单标签模型缺口,REPORT.md「Skipped scenarios」全列)。

## 二、引擎侧最小改动清单(vs 0.21.1 pristine `8e4b79d`)

| 文件 | 改动 | 性质 |
|---|---|---|
| `src/parser/visitor/standalone_call_rewriter.cpp` +4 | 多语句批互相污染**真 bug 修复**(也是「CALL GQL 必须单独成句」根因) | GQL 使能 |
| `src/include/catalog/catalog.h` +9 / `src/catalog/catalog.cpp` +11 / `src/main/database_manager.cpp` +6 | `Catalog::setFunctionFallback`——图 catalog 函数查找回退 main | GQL 使能 |
| `src/include/extension/extension_manager.h` +7 / `src/extension/extension_manager.cpp` +9 | `ExtensionManager::setData/getData` 每库扩展状态槽 | GQL 使能 |
| `src/binder/bind/bind_graph_pattern.cpp` +4 | 路径变量 `setAlias`(命名,不动语义) | GQL 使能 |
| `src/planner/plan/plan_subquery.cpp` +7/-2 | correlated-optional unnest by-value 修复(独立 commit `32cf881`) | **fork 侧独立**,勿捆进 GQL PR |
| `src/extension/CMakeLists.txt` | fork port(`bac3e0b`) | fork 侧,不进上游 PR |

GQL 使能 4 组合计 ≲54 行。全部以门禁 4(全量回归)护航。

## 三、PR 拆分建议(按 CONTRIBUTING「Avoid large pull requests」)

1. **PR-A 引擎侧**:上表 GQL 使能 4 组(4 个关注点可再拆:rewrite bugfix /
   setFunctionFallback / setData+getData / setAlias)。证据 = 全量回归数字。
2. **PR-B GQL extension**:`extension/gql/` 51 文件 +17,238 行(手写 transformer +
   vendored ANTLR 生成物 + vendored opengql/tck 语料 + 161 双跑用例)。
   description 注明 vendored 来源与生成方式(THIRD_PARTY_NOTICES.md 逐文件署名)。
3. 按 CONTRIBUTING 第一条,**先开 issue** 贴兼容矩阵 + 测试数字,与 core team 对齐范围。

**不进任何 PR**:中文工作文档(handover / 语义地图 / 方法学 / 评估 / 咨询)、
`_build_gql.bat`、`_gate_*.log`、untracked 杂物(`_tmp_*`、`follows.csv`、
`hyperalgo-core/`、`user.csv/parquet`)、`tools/wasm/package-lock.json` 改动。

## 四、PR 前待办(本次未能核验,如实列出——不声称测过没测的)

- **clang-format-18**:本机无 clang-format,无法本地核验(CI 有 Clang Format job 会卡)。
  提交前在有 clang-format-18 的环境跑
  `python3 scripts/run-clang-format.py --clang-format-executable clang-format-18 -r extension/gql src/`。
- **跨平台**:全部实测 = Windows 本机;CI 是 ubuntu-24.04 + 多平台 matrix(含 Windows)。
  建议 WSL/docker 过一遍 Linux 构建 + 自测;做不到则 PR description 如实写明测试平台。
- **CI sanity「generated grammar files up to date」**:对 vendored ANTLR 生成物
  (锁 4.13.1)是否放行未实测。
- ~~run_tck.py 输出非确定性~~ **已修**(`82e5107`):报告四个写出循环(values-only /
  passed-with-note / skipped / unchecked)补排序;两连跑验证 **REPORT 字节一致**,
  70/120/9/7 不变。重跑幂等,reviewer 复跑不再看到洗牌 diff。

## 五、英文可粘贴段(PR description「Testing」节)

```text
Testing (all on Windows 11 / MSVC 18 BuildTools, 2026-10-02; source tree at
5f28079 — later commits are test-harness/docs only):

- Build: Release build of `e2e_test` + `libgql.lbug_extension` — clean (no errors).
- GQL dual-run parity suite: 161/161 passed (18 suites; each case runs the GQL
  statement and an equivalent hand-written Cypher against the same expected
  result) — `E2E_TEST_FILES_DIRECTORY=extension e2e_test --gtest_filter="gql~test~test_files~*"`.
- opengql/tck conformance run: 190/206 green (70 passed + 120 passed-with-note),
  9 failed, 7 skipped. All 9 failures are loud rejections (0 silent wrong
  answers): 4 parse-error corpus issues (openCypher `CREATE`/`UNWIND` setup
  statements, `CREATE GRAPH ... AS COPY OF` grammar ambiguity) + 5 explicit
  design rejections (multi-label node types, `LIKE`/`AS COPY OF` graph
  copying). Wrong-GQLSTATUS count: 0. Scenario-level detail and corpus-integrity
  footnotes: `extension/gql/test/tck/REPORT.md`. Report generation is
  deterministic (listings sorted; verified byte-stable across consecutive
  reruns).
- Full engine regression (covers the engine-side changes below): 1972/1975
  passed. The 3 failures are pre-existing extension-registry INSTALL cases in
  this build configuration (LoadNotInstalledExtension / ForceInstallExtension /
  UninstallExtensionError): CMake's optional `find_package(OpenSSL 3)` is not
  satisfied on this build machine, so the bundled cpp-httplib is compiled
  without SSL and cannot download over https (rejected before any network I/O
  with `'https' scheme is not supported`). Deterministic in this build config
  and unrelated to this PR's changes.

Engine-side minimal changes (each a few lines, backed by the full regression):
- `standalone_call_rewriter.cpp`: fix multi-statement batch contamination
- `Catalog::setFunctionFallback` (catalog.h/.cpp, database_manager.cpp)
- `ExtensionManager::setData/getData` (extension_manager.h/.cpp)
- path variable `setAlias` in `bind_graph_pattern.cpp` (naming only)
```
