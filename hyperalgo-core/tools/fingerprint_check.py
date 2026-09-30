#!/usr/bin/env python3
"""指纹跨端核验(FRZ-12 checklist 3 / FRZ-07.1):Python 独立复算 vs C++ 唯一出口。

规范(P-1 SPEC v1.1 FRZ-07.1):
  - canonical JSON:键按 UTF-8 字节序排序、无空白;UTF-8 编码;
  - 内容:n_nodes/n_edges/edges(按 edge_business_ids 升序;id=S_int_id,members=ext_id
    升序,h 与 members 对齐 "%.3f" 定点,k_e,edge_alive)+ node_alive(结构快照省略);
  - double 先 round(x*1000)/1000 再 %.3f;
  - 哈希:SHA256(canonical JSON 字节)。
本脚本按规范独立复算(Python 侧只比对/复算,不产出权威值——生产值以 C++ 构造层为唯一出口)。

用法:
  python fingerprint_check.py <input.tsv> --cli <hyperalgo_cli.exe> [--expect <sha256hex>]
TSV 行格式见 hyperalgo_cli.cpp(NODE/FLOW/GROUP/KE)。
"""
import argparse
import hashlib
import json
import math
import subprocess
import sys


def parse_tsv(path):
    nodes, flows, groups, ke = {}, [], {}, {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            p = line.split()
            if p[0] == "NODE":
                nodes[p[1]] = int(p[2]) if len(p) > 2 else 0
            elif p[0] == "FLOW":
                flows.append((p[1], p[2], float(p[3]) if len(p) > 3 else 1.0))
            elif p[0] == "GROUP":
                groups[p[1]] = p[2]
            elif p[0] == "KE":
                ke[p[1]] = int(p[2])
    return nodes, flows, groups, ke


def fixed3(x):
    """FRZ-07.1 条款 3:C++ std::round(半远离零)→ %.3f(正数域,权重 > 0)。"""
    return "%.3f" % (math.floor(x * 1000.0 + 0.5) / 1000.0)


def canonical_json_and_hash(nodes, flows, groups, ke):
    """按 FRZ-07.1 构建 canonical JSON 并返回 (json_str, sha256hex)。"""
    # 1) 聚合并行边(FRZ-01 条款 4:同 (u,S) 取 SUM)
    edge_members = {}  # S → {u: weight}
    endpoints = set()
    for u, s, w in flows:
        endpoints.add(u)
        endpoints.add(s)
        edge_members.setdefault(s, {})
        edge_members[s][u] = edge_members[s].get(u, 0.0) + w
    # 2) IdMap:内部 id = ext_id 按 UTF-8 字节序排序后的下标(Python str 序 = 码点序 = UTF-8 字节序)
    ordered = sorted(endpoints)
    int_of = {ext: i for i, ext in enumerate(ordered)}
    # 3) 超边按 edge_business_ids(S 的内部 id)升序;成员按 ext 升序,h 对齐
    edges = []
    for s in sorted(edge_members, key=lambda x: int_of[x]):
        mem = edge_members[s]
        members_sorted = sorted(mem.keys())
        # k_e:显式 KE 覆盖 > 默认 g(e)(料号按 material_id 分组 + 站成员各自一组)
        if s in ke:
            k_e = ke[s]
        else:
            group_keys = {
                groups.get(u, "\x00station\x00" + u) for u in members_sorted
            }
            k_e = len(group_keys)
        edges.append({
            "edge_alive": 1,  # 结构快照(无状态):edge_alive 恒 1(FRZ-07.1 条款 5)
            "h": [fixed3(mem[u]) for u in members_sorted],
            "id": int_of[s],
            "k_e": k_e,
            "members": members_sorted,
        })
    obj = {
        "edges": edges,
        "n_edges": len(edges),
        "n_nodes": len(ordered),
    }
    js = json.dumps(obj, ensure_ascii=False, separators=(",", ":"), sort_keys=True)
    return js, hashlib.sha256(js.encode("utf-8")).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("tsv")
    ap.add_argument("--cli", required=True, help="hyperalgo_cli 可执行文件")
    ap.add_argument("--expect", default=None, help="期望 SHA256(如扩展端/金本值)")
    ap.add_argument("--print-json", action="store_true")
    args = ap.parse_args()

    nodes, flows, groups, ke = parse_tsv(args.tsv)
    js, ref = canonical_json_and_hash(nodes, flows, groups, ke)
    out = subprocess.run([args.cli, "fp", args.tsv], capture_output=True, text=True, check=True)
    cxx = out.stdout.strip()

    if args.print_json:
        print(js)
    print(f"Python 复算: {ref}")
    print(f"C++ 出口  : {cxx}")
    ok = ref == cxx
    if not ok:
        print("FAIL: Python 复算与 C++ 指纹不一致(规范实现偏差)")
    if args.expect:
        print(f"期望值    : {args.expect}")
        if cxx != args.expect:
            print("FAIL: C++ 指纹与期望值不一致")
            ok = False
    if ok:
        print("PASS: 指纹一致")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
