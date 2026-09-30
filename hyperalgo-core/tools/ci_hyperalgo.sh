#!/usr/bin/env bash
# hyperalgo 逐夜 CI(P-1.5 v1.2 §4 一致性条款的仓库内落地)
#   1) 核心单测(样例 A/B/C/C1/C2 + 环/谱/游走/校验)
#   2) 黄金图集核对(FRZ-12:指纹 Python 复算 vs C++ 出口、布尔化、s-cc/齐套/击垮集/ks-core/B-回路)
#   3) HNX s-cc 对拍(P0-1a;需 hyperalgo-core/.venv-oracle,缺失则 SKIP)
#   4) P0-2/P1a 验收(4 级链回路/断环;击垮集随机小图 vs 穷举)
#   5) 扩展 E2E(含判定层 Cypher 金本 + 指纹跨端互证)
# 前置:先构建(python hyperalgo-core 的 _build_test.bat / 根 _build_hyperalgo.bat),本脚本只验证。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CLI="$ROOT/hyperalgo-core/build_test/Release/hyperalgo_cli.exe"
TESTS="$ROOT/hyperalgo-core/build_test/Release/hyperalgo_tests.exe"
E2E="$ROOT/build_hyperalgo/src/Release/e2e_test.exe"
E2E_DIR="$ROOT/extension/hyperalgo/test/test_files"
ORACLE_PY="$ROOT/hyperalgo-core/.venv-oracle/Scripts/python.exe"
HNX_PATH="${HNX_PATH:-C:/Users/chian/Documents/kimi/workspace/hypergraph-reference/hnx}"
PY="${PYTHON:-python}"

step() { echo; echo "=== $* ==="; }
need() { [ -f "$1" ] || { echo "缺少 $1(请先构建)"; exit 2; }; }

need "$CLI"; need "$TESTS"

step "1/5 核心单测"
"$TESTS"

step "2/5 黄金图集核对(FRZ-12)"
PYTHONIOENCODING=utf-8 "$PY" "$ROOT/hyperalgo-core/tools/golden_check.py" --cli "$CLI"

step "3/5 HNX s-cc 对拍(P0-1a)"
if [ -x "$ORACLE_PY" ]; then
    "$ORACLE_PY" "$ROOT/hyperalgo-core/tools/compare_scc.py" /tmp/sample_b.tsv 1 \
        --hnx-path "$HNX_PATH" --cli "$CLI"
else
    echo "SKIP:未找到 $ORACLE_PY(python -m venv hyperalgo-core/.venv-oracle + pip install 依赖)"
fi

step "4/5 P0-2 / P1a 验收"
PYTHONIOENCODING=utf-8 "$PY" "$ROOT/hyperalgo-core/tools/p0_acceptance.py" --cli "$CLI"

step "5/5 扩展 E2E(判定层金本 + 指纹跨端)"
if [ -f "$E2E" ]; then
    E2E_TEST_FILES_DIRECTORY="$E2E_DIR" "$E2E" --gtest_filter="*Hyperalgo*"
else
    echo "SKIP:未找到 $E2E(请先运行 _build_hyperalgo.bat)"
fi

echo
echo "ALL CHECKS PASSED"
