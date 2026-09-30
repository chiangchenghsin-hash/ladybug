# LadybugDB fork 升级踩坑手册

> 供下次升级（0.21.x → 0.22+ 或回移修复）使用。按升级阶段组织，每条 = 现象 → 原因 → 对策。
> 首版 2026-09-30，来自 0.20.2 → 0.21.1 升级实战（含 MERGE 规划死循环根因定位全程）。

## 0. 诊断方法论（先看这节）

### 死循环/挂死怎么定位（Windows，无 cdb/lldb 时）
- **心跳埋点法**：往嫌疑函数链的关键阶段插 `fprintf(stderr, "[HB] ..."); fflush(stderr);`，增量重编（MSVC 增量只需分钟级），跑复现用例看最后一个心跳在哪——挂死点就在最后一条心跳和下一条之间。定位后**逐处删掉埋点再提交**（保留修复注释）。
- **规划 vs 优化器二分**：内置开关 `LBUG_DUMP_LOGICAL=1` 会在优化器前后转储逻辑计划到 stderr。有"before"无"after"= 挂优化器；完全没有 = 挂规划器/绑定器。
- **e2e 挂死时先 `taskkill //F //IM e2e_test.exe`**：被杀任务的残留子进程会占住 `lbug_shared.dll`，后续重编报 `LNK1104 无法打开文件`。

### UB 怀疑清单（本次根因就是这类）
- `insert(f().begin(), f().end())`：`f()` **按值返回**且被调两次 → 迭代器区间跨两个临时对象 → 循环永不终止/走野内存（表现：单核挂死或 SIGSEGV，堆布局不同表现不同——"空表过、有数据挂"常是这类假象，别被"数据依赖"带偏）。
- 排查命令（单行模式全库扫）：`grep -rn "\w\+()\.\(begin\|end\)()" src/ extension/`；多行模式用 ripgrep `multiline` 查 `.begin(),\n.*.end()` 并人工确认两侧是同一左值。
- clang-tidy 的 `bugprone-infinite-loop` 可辅助，但**查不出这种 UB**（它看的是循环条件无副作用）。

## 1. 移植/构建

| 坑 | 现象 | 对策 |
|---|---|---|
| Python GBK 解码 | wasm 链最后一步 `collect-single-file-header.py` 报 `UnicodeDecodeError: 'gbk' codec...`（ninja 1345/1346） | 环境加 `PYTHONUTF8=1` 再跑 ninja |
| ninja 路径 | emsdk 目录下没有 ninja.exe；`cmd //c "..."` 复合引号经 bash 会碎 | 用 VS 路径 `...Microsoft\CMake\Ninja\ninja.exe`；**多步命令写 .bat 执行**，别在 bash 里嵌套引号 |
| wasm 打包三段链 | 别直接 `npm run build`（build.mjs 依赖 GNU make + 自删构建目录） | ninja → `cd tools/wasm && npm i && node bundle.mjs` → `package/`（worker 单文件）→ 拷入 `dist-*/wasm-deploy/lbug-wasm/` |
| frontend-aps/ 缺失 | 0.21.1 树没有桥接层文件 | 从上一版 `dist-ladybug-0.20.2/wasm-deploy/` 取 `lbug-bridge.js`/`serve.py`/`test-*.html` |
| python 依赖轮子 | Python 3.14 + 清华 pip 源装不上 pyarrow（`from versions: none`） | `pip install pyarrow -i https://pypi.org/simple`（官方源有 cp314 轮子） |
| bash cwd 漂移 | 长会话里 `cd` 过的 shell cwd 持续存在，相对路径悄悄失效 | 关键命令用绝对路径，或命令首行显式 `cd` 到树根 |

## 2. 测试

| 坑 | 现象 | 对策 |
|---|---|---|
| e2e 裸跑 0 tests | `e2e_test.exe` 无参数报 "does NOT link in any test case" | **必须带 `--gtest_filter="*"`**（runner 按 filter 注册用例） |
| e2e fixture 缺失 | `dictionary_bug/orb383_*` 报 `No file found ... A.parquet` | `python scripts/generate-dictionary-bug-fixture.py` 生成（需 pyarrow；parquet 产物不入 git） |
| extension~extension.* 失败 | 3 个用例报 httpfs/sqlitescanner 安装失败 | 需联网 `INSTALL` 官方扩展仓库，离线环境预期失败，非回归 |
| hyperalgo 动态扩展路径 | 扩展 e2e 报 `extension ... neither official ... nor ... exist` | `LOAD_DYNAMIC_EXTENSION` 在 `extension/hyperalgo/build/` 找；MSVC 产物在 `build/Release/`，**拷一份过去** |
| 改 core 源码后金测卡死 | `ci_hyperalgo.sh` 在 OPTIONAL MATCH 上无限挂（正是被修的 bug） | CI 用 `build_hyperalgo/src/Release/e2e_test.exe`——core 源码改动后**必须重编它**，否则旧 bug 还在测试二进制里 |

## 3. 上报上游（PR 流程）

- 标准流程：fork（账号下老 fork 先 Sync fork）→ `fix/<kebab>` 分支（沿用历史命名）→ 基于**上游 main**（不是 fork 的落后 main）→ push → PR。
- 大仓库用 `git fetch --depth 1 --filter=blob:none` + sparse-checkout 只取目标文件，免全量克隆。
- PR 用英文、语气礼貌：根因 / 复现（含"为什么容易误判"的细节）/ 修复 / 测试 四段；引用引入提交 sha。
- 判 bug 归属：`git show <baseline>:<file>` 看纯净基线是否带病——带病 = 上游 bug 走 PR；fork 私有 = 本地修。

## 4. 打包

- **windows zip** = `dist-ladybug-0.21.1/` 全量。体量对标上一版（0.20.2: 113.9MB → 0.21.1: 116.9MB）；抽验 zip 内文档是更新版（`zipfile` 读出来 grep 版本号）。
- **ubuntu zip** = 整树快照（含 untracked/node_modules，排 build*/dist/benchmark/log）。**dataset 只留 tinysnb**：0.21.1 上游快照自带全量测试数据（snap/ldbc-sf01/copy-test/large-array/lsqb-sf01，压缩后 ~113MB），全收进去包就从 ~72MB 膨胀到 ~182MB。`_pack_0211.py` 的 `slim_dataset=True` 已固化此约定。
- dist 是 gitignore 的发行产物树，改文档不用提交 git；但 `_pack_*.py` 的约定变更要提交。
- dist 里别留 `node_modules`（冒烟用的 npm 装到用户级 `~/node_modules` 即可，node 自会向上解析）。

## 5. 版本口径速查

- 存储版本 **v47** 全程不变（0.20.0→0.21.x 零迁移）；`canReadStorageVersion` 放行 40–46+当前（0.21.x 可读 v46，0.20.2 不可）。
- 扩展 ABI **0.21.0→0.21.1 有变更**，每次 minor 升级扩展全部重编；官方 core 同 minor 配套。
- 文档版本标注更新点：dist README / FIXES / api-reference / wasm-deploy README 四处 + 冒烟脚本名 `smoke-<ver>.js` + `@ladybugdb/core@<ver>`。
