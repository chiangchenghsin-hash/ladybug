# hyperalgo-core 对拍与验收工具

| 工具 | 用途 |
|---|---|
| `hyperalgo_cli.cpp` | 核心算法 CLI(TSV 输入:`NODE/FLOW/GROUP/KE`;命令 `scc/kitting/hitset/kscore/bcycles/booleanize/pr/fiedler/fp`) |
| `fingerprint_check.py` | FRZ-07.1 指纹:Python 独立复算 vs C++ 出口(`--expect` 可再比扩展端/金本值) |
| `golden_check.py` | FRZ-12 黄金图集全量核对(HIF→TSV→CLI vs `*.expected.json`) |
| `compare_scc.py` | P0-1a:HNX `s_connected_components` 对拍(节点分量,成员域) |
| `p0_acceptance.py` | P0-2(4 级链回路/断环)+ P1a(击垮集随机小图 vs 穷举) |
| `ci_hyperalgo.sh` | 逐夜流水:单测 → 黄金图集 → HNX 对拍 → P0/P1a → 扩展 E2E |

## 构建

```bash
# 核心 CLI + 单测(MSVC)
cmd //c hyperalgo-core/_build_test.bat
# 扩展 + E2E
cmd //c _build_hyperalgo.bat
# 全量校验
bash hyperalgo-core/tools/ci_hyperalgo.sh
```

## HNX oracle 环境(仅对拍需要,可选)

`compare_scc.py` 需要原始解压的 HyperNetX 2.4.3(`hypergraph-reference/hnx`,BSD-3,
本地运行不抄码)。依赖较重,建议隔离 venv:

```bash
python -m venv hyperalgo-core/.venv-oracle
hyperalgo-core/.venv-oracle/Scripts/python.exe -m pip install \
  --index-url https://pypi.org/simple --only-binary=:all: \
  networkx numpy "pandas<3" scipy matplotlib scikit-learn heapdict requests \
  fastjsonschema decorator bitsets
```

注意:
- **`pandas<3` 必须固定**——HNX 2.4.3 与 pandas 3.x 不兼容(dict factory 的
  `dfp.misc_properties` 属性访问路径在 pandas 3 变更后报 AttributeError);
  Python 3.14 下 pandas 2.3.3 有 cp314 wheel,已验证可用。
- 公司/校园镜像(如 tsinghua)在本机沙箱内不可达时用 `--index-url https://pypi.org/simple`。
- 对拍域说明:HNX 节点域 = 超边成员;C++ CSR 另含纯头节点(FRZ-01 条款 3),
  脚本自动校验域差并只在成员域比对。
