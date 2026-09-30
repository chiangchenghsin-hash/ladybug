#!/usr/bin/env python3
"""s-walk 连通分量对拍:P-1 SPEC v1.1 FRZ-12 checklist + 升级计划 v1.3 P0-1a 验收。

C++ (hyperalgo_cli scc) vs HNX s_connected_components(参考 oracle #1)。
用法:
  python compare_scc.py <input.tsv> <s> [--hnx-path <dir>] [--cli <exe>]
输出:分量划分(按节点分组的规范形)逐位比对;一致 → PASS。

语义对齐(双方同一口径):
  - 节点 s-邻接 ⟺ 两节点共现于 ≥ s 条超边(HNX adjacency_matrix = B·Bᵀ ≥ s;
    C++ connectivity.hpp 同款共现计数);
  - 取节点分量(HNX edges=False),保留单点分量(return_singletons=True)。
对拍域:HNX 的节点 = 超边成员;C++ CSR 还含「纯头节点」(作为 S 出现、不作成员的站,
FRZ-01 条款 3)。故对拍域 = 成员域(交集),纯头节点差异单独核验允许。
注:分量编号本身无意义,比对「节点→分量」等价关系(划分规范形)。

依赖:HNX 2.4.3 原始解压目录(hypergraph-reference/hnx),需 networkx/numpy/pandas<3/
scipy/matplotlib/scikit-learn/heapdict/requests/fastjsonschema/decorator/bitsets;
本仓库建议用 hyperalgo-core/.venv-oracle(Python 3.14 + pandas 2.3.3,见 README)。
"""
import argparse
import collections
import subprocess
import sys


def parse_tsv(path):
    """返回 (nodes: [(ext, alive)], flows: [(u, s, w)])"""
    nodes, flows = [], []
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if parts[0] == "NODE":
                nodes.append((parts[1], int(parts[2]) if len(parts) > 2 else 1))
            elif parts[0] == "FLOW":
                flows.append((parts[1], parts[2], float(parts[3]) if len(parts) > 3 else 1.0))
    return nodes, flows


def partition_of(label_to_cid):
    """划分规范形:每个分量 → 排序后的节点标签集合"""
    by = collections.defaultdict(list)
    for label, cid in label_to_cid.items():
        by[cid].append(label)
    return sorted(tuple(sorted(v)) for v in by.values())


def hnx_scc(hnx_path, flows, s):
    """HNX 参考实现:Hypergraph.s_connected_components(s, edges=False)
    (hypergraph.py:2082;get_linegraph→adjacency_matrix B·Bᵀ ≥ s)。"""
    sys.path.insert(0, hnx_path)
    import hypernetx as hnx

    edges = {}
    for i, (u, v, w) in enumerate(flows):
        edges.setdefault(v, []).append(u)
    H = hnx.Hypergraph(edges)  # 节点域 = 超边成员(HNX 中 S 仅为超边 id)
    label_of = {}
    for cid, members in enumerate(
        H.s_connected_components(s=s, edges=False, return_singletons=True)
    ):
        for m in members:
            label_of[str(m)] = cid
    return label_of


def cli_scc(cli, tsv, s):
    out = subprocess.run([cli, "scc", tsv, str(s)], capture_output=True, text=True, check=True)
    label_to_cid = {}
    for line in out.stdout.splitlines():
        label, cid = line.split("\t")
        label_to_cid[label] = int(cid)
    return label_to_cid


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("s", type=int)
    ap.add_argument(
        "--hnx-path",
        default=r"C:\Users\chian\Documents\kimi\workspace\hypergraph-reference\hnx",
    )
    ap.add_argument("--cli", default=r"build_test\Release\hyperalgo_cli.exe")
    args = ap.parse_args()

    nodes, flows = parse_tsv(args.tsv)
    members = {u for u, _, _ in flows}
    heads = {v for _, v, _ in flows}

    # C++ 侧(CSR 全域,含纯头节点)
    cxx_all = cli_scc(args.cli, args.tsv, args.s)
    cxx_part_all = partition_of(cxx_all)

    # HNX 侧(成员域)
    hnx_all = hnx_scc(args.hnx_path, flows, args.s)

    cxx_set, hnx_set = set(cxx_all), set(hnx_all)
    hnx_only = hnx_set - cxx_set
    cxx_only = cxx_set - hnx_set
    # 域差仅允许:C++ 独有的纯头节点(出现为 S、从不作成员)
    illegal = cxx_only - (heads - members)
    ok_domain = not hnx_only and not illegal

    common = cxx_set & hnx_set
    cxx_part = partition_of({k: v for k, v in cxx_all.items() if k in common})
    hnx_part = partition_of({k: v for k, v in hnx_all.items() if k in common})

    print(f"s={args.s}")
    print(f"  C++  节点域 {len(cxx_set)} 节点, 全域分量数: {len(cxx_part_all)}"
          f"(成员域分量数: {len(cxx_part)})")
    print(f"  HNX  节点域 {len(hnx_set)} 节点, 分量数: {len(hnx_part)}")
    if cxx_only:
        print(f"  域差:C++ 独有纯头节点 {sorted(cxx_only)}(FRZ-01 条款 3,不参与成员对拍)")
    if hnx_only:
        print(f"  域差异常:HNX 独有节点 {sorted(hnx_only)} → C++ CSR 缺失成员")
    if illegal:
        print(f"  域差异常:C++ 独有非纯头节点 {sorted(illegal)}")
    if not ok_domain:
        print("  FAIL: 节点域不一致")
        return 1
    if cxx_part == hnx_part:
        print("  PASS: 成员域分量划分一致")
        return 0
    print("  FAIL: 划分不一致")
    print(f"  C++ : {cxx_part}")
    print(f"  HNX : {hnx_part}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
