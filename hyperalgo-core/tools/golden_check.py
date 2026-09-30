#!/usr/bin/env python3
"""黄金图集核对(FRZ-12):HIF → TSV → hyperalgo_cli 算法/指纹 vs *.expected.json。

覆盖 FRZ-12 checklist:1(构造不变量由 core validate 承担)、3(指纹:Python 复算 +
C++ 出口 + 可选扩展端期望值)、4(退化/孤立计数非零样例)、5(C1/C2 k_e 语义锁定)。
用法:
  python golden_check.py [--golden-dir hyperalgo-core/golden] --cli <exe> [--write-fingerprint]
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fingerprint_check import canonical_json_and_hash  # noqa: E402


def default_k_e(edge):
    """FRZ-05 默认 k_e = g(e):料号成员按 material_id 分组(缺失即独立),站成员各自独立。"""
    keys = set()
    for m in edge["members"]:
        mat = m.get("material_id")
        keys.add(("mat", mat) if mat else ("stn", m["ext_id"]))
    return len(keys)


def hif_to_tsv(hif, alive_override=None):
    """alive_override: None = 按 HIF 快照;int = 全员覆盖;dict = ext_id → alive。"""
    lines = []
    for n in hif["nodes"]:
        if alive_override is None:
            alive = n.get("alive", 1)
        elif isinstance(alive_override, dict):
            alive = alive_override.get(n["ext_id"], 0)
        else:
            alive = alive_override
        lines.append(f"NODE {n['ext_id']} {alive}")
    for e in hif["edges"]:
        for m, h in zip(e["members"], e["h"]):
            lines.append(f"FLOW {m['ext_id']} {e['id']} {h}")
            if m.get("material_id"):
                lines.append(f"GROUP {m['ext_id']} {m['material_id']}")
        if e["k_e"] != default_k_e(e):
            lines.append(f"KE {e['id']} {e['k_e']}")
    return "\n".join(lines) + "\n"


def run(cli, cmd, tsv_path, *args):
    out = subprocess.run([cli, cmd, tsv_path, *[str(a) for a in args]],
                         capture_output=True, text=True, check=True)
    return out.stdout


def check_sample(cli, golden_dir, name, write_fp):
    hif = json.load(open(os.path.join(golden_dir, f"{name}.hif.json"), encoding="utf-8"))
    exp = json.load(open(os.path.join(golden_dir, f"{name}.expected.json"), encoding="utf-8"))
    tsv_all = os.path.join(golden_dir, f".{name}.all.tsv")
    tsv_as_is = os.path.join(golden_dir, f".{name}.asis.tsv")
    with open(tsv_all, "w", encoding="utf-8") as f:
        f.write(hif_to_tsv(hif, alive_override=1))
    with open(tsv_as_is, "w", encoding="utf-8") as f:
        f.write(hif_to_tsv(hif))

    fails = []

    def expect(label, got, want):
        if got != want:
            fails.append(f"{label}: got {got!r}, want {want!r}")

    # 构造统计(从 HIF 重建;扩展端实现另由 E2E 断言)
    stats = {
        "n_nodes": len(hif["nodes"]),
        "n_edges": len(hif["edges"]),
        "isolated_stations": len({n["ext_id"] for n in hif["nodes"]}
                                 - {e["id"] for e in hif["edges"]}),
        "degenerate_edges": sum(1 for e in hif["edges"] if len(e["members"]) == 1),
    }
    for k, v in exp.get("stats", {}).items():
        if k == "note":
            continue
        expect(f"stats.{k}", stats[k], v)

    # 指纹(FRZ-07.1):Python 复算 == C++ 出口;--write-fingerprint 时回填 expected
    nodes, flows, groups, ke = parse_tsv(tsv_as_is)
    _, py_fp = canonical_json_and_hash(nodes, flows, groups, ke)
    cxx_fp = run(cli, "fp", tsv_as_is).strip()
    expect("fingerprint(Python vs C++)", py_fp, cxx_fp)
    if write_fp:
        exp["fingerprint"] = cxx_fp
        with open(os.path.join(golden_dir, f"{name}.expected.json"), "w", encoding="utf-8") as f:
            json.dump(exp, f, ensure_ascii=False, indent=2)
            f.write("\n")
    elif "fingerprint" in exp:
        expect("fingerprint(expected vs C++)", cxx_fp, exp["fingerprint"])

    # 布尔化(FRZ-05;expected.booleanize 单超边样例)
    if "booleanize" in exp:
        rows = [l.split("\t") for l in run(cli, "booleanize", tsv_as_is).splitlines()]
        expect("booleanize.edges", len(rows), 1)
        groups_n, k_e, alive_g, alive = (int(rows[0][1]), int(rows[0][2]),
                                         int(rows[0][3]), int(rows[0][4]))
        b = exp["booleanize"]
        expect("booleanize.groups", groups_n, b["groups"])
        expect("booleanize.k_e", k_e, b["k_e"])
        expect("booleanize.alive_groups", alive_g, b["alive_groups"])
        expect("booleanize.edge_alive", alive, b["edge_alive"])
    # alive 模式扫描(FRZ-12 样例 C:齐 / 缺 2 员)
    for i, mode in enumerate(exp.get("booleanize_modes", [])):
        tsv_mode = os.path.join(golden_dir, f".{name}.mode{i}.tsv")
        with open(tsv_mode, "w", encoding="utf-8") as f:
            f.write(hif_to_tsv(hif, alive_override=mode["alive"]))
        cols = run(cli, "booleanize", tsv_mode).splitlines()[0].split("\t")
        tag = mode.get("name", f"mode{i}")
        expect(f"modes[{tag}].groups", int(cols[1]), mode["groups"])
        expect(f"modes[{tag}].k_e", int(cols[2]), mode["k_e"])
        expect(f"modes[{tag}].alive_groups", int(cols[3]), mode["alive_groups"])
        expect(f"modes[{tag}].edge_alive", int(cols[4]), mode["edge_alive"])
        os.remove(tsv_mode)

    # HIF 中 edge_alive 与实现一致(全样例)
    rows = {l.split("\t")[0]: l.split("\t")[4] for l in run(cli, "booleanize", tsv_as_is).splitlines()}
    for e in hif["edges"]:
        expect(f"edge_alive({e['id']})", rows[e["id"]], str(e["edge_alive"]))

    # s-cc(s=1;全活结构口径)
    if "s_cc_s1" in exp:
        comps = {l.split("\t")[1] for l in run(cli, "scc", tsv_all, 1).splitlines()}
        expect("s_cc_s1.components", len(comps), exp["s_cc_s1"]["components"])

    # 齐套连通(全活)
    if "kitting_cc_all_alive" in exp:
        blocks = {}
        for l in run(cli, "kitting", tsv_all).splitlines():
            u, b = l.split("\t")
            blocks.setdefault(b, []).append(u)
        got = sorted(sorted(v) for v in blocks.values())
        want = sorted(sorted(v) for v in exp["kitting_cc_all_alive"]["blocks"])
        expect("kitting_cc_all_alive.blocks", got, want)

    # 击垮集
    if "hitting_set" in exp:
        got = run(cli, "hitset", tsv_all).split()
        expect("hitting_set.size", len(got), exp["hitting_set"]["size"])
        if "nodes" in exp["hitting_set"]:
            expect("hitting_set.nodes", sorted(got), sorted(exp["hitting_set"]["nodes"]))

    # (k,s)-core(1,1)
    if "ks_core_1_1" in exp:
        got = {}
        for l in run(cli, "kscore", tsv_all, 1, 1).splitlines():
            u, layer = l.split("\t")
            got.setdefault(int(layer), []).append(u)
        got_layers = [sorted(got[k]) for k in sorted(got)]
        want = [sorted(x) for x in exp["ks_core_1_1"]["layers"]]
        expect("ks_core_1_1.layers", got_layers, want)

    # B-回路
    if "b_cycles" in exp:
        ids = {l.split("\t")[0] for l in run(cli, "bcycles", tsv_all).splitlines()}
        expect("b_cycles.cycles", len(ids), exp["b_cycles"]["cycles"])

    for f in (tsv_all, tsv_as_is):
        os.remove(f)
    return fails


def parse_tsv(path):
    """与 hyperalgo_cli.cpp 同格式的 TSV 解析。"""
    from fingerprint_check import parse_tsv as p
    return p(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--golden-dir",
                    default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "golden"))
    ap.add_argument("--cli", required=True)
    ap.add_argument("--write-fingerprint", action="store_true",
                    help="把 C++ 指纹回填 expected.json(CI 用核对模式,不带此开关)")
    args = ap.parse_args()

    names = sorted(f[:-len(".hif.json")] for f in os.listdir(args.golden_dir)
                   if f.endswith(".hif.json"))
    all_ok = True
    for name in names:
        fails = check_sample(args.cli, args.golden_dir, name, args.write_fingerprint)
        if fails:
            all_ok = False
            print(f"[FAIL] {name}")
            for f in fails:
                print(f"    - {f}")
        else:
            print(f"[PASS] {name}")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
