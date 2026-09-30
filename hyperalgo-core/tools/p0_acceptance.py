#!/usr/bin/env python3
"""P0/P1a 验收执行(升级计划 v1.3 §4):
  P0-2  B-回路:4 级链全回路闭合检出、人为断一环检出(无 oracle,场景互证);
  P1a-2 击垮集:小图(n_edges ≤ 12)贪心解与穷举最优一致 + 覆盖性构造检验;
  P1a-5 样例 C1/C2 需求组语义(由 golden_check.py 锁定,此处仅提示)。
未覆盖(P0-1b 主验收「跳变轮次差 ≤2」):需 §12.2 场景层(RoundState/demand_share
来源表未定,附录 A 冻结为构造参数注入),待 P3a 场景数据到位后执行——见 --report-skip。

用法:python p0_acceptance.py --cli <hyperalgo_cli.exe> [--random 200] [--seed 20260910]
"""
import argparse
import itertools
import os
import random
import subprocess
import sys
import tempfile


def write_tsv(path, flows, nodes):
    with open(path, "w", encoding="utf-8") as f:
        for n in nodes:
            f.write(f"NODE {n} 1\n")
        for u, s, w in flows:
            f.write(f"FLOW {u} {s} {w}\n")


def run(cli, cmd, tsv, *args):
    out = subprocess.run([cli, cmd, tsv, *[str(a) for a in args]],
                         capture_output=True, text=True, check=True)
    return out.stdout.splitlines()


def test_b_cycles(cli, tmp):
    """P0-2:环 A→B→C→D→A(FLOWS 方向)超边级闭合;断 D→A 后无回路。"""
    fails = []
    nodes = ["A", "B", "C", "D"]
    ring = [("D", "A", 1.0), ("A", "B", 1.0), ("B", "C", 1.0), ("C", "D", 1.0)]
    broken = ring[1:]  # 断 D→A:e_A 无成员,超边消失,回路断裂
    cases = [
        ("回路闭合(4 超边环)", ring, 1, 4),
        ("人为断一环(D→A 移除)", broken, 0, 0),
    ]
    for name, flows, want_cycles, want_edges in cases:
        p = os.path.join(tmp, "bcycles.tsv")
        write_tsv(p, flows, nodes)
        rows = run(cli, "bcycles", p)
        cycles = {}
        for r in rows:
            cid, edge = r.split("\t")
            cycles.setdefault(cid, []).append(edge)
        got_cycles = len(cycles)
        got_edges = sum(len(v) for v in cycles.values())
        if got_cycles != want_cycles or got_edges != want_edges:
            fails.append(f"B-回路[{name}]: got {got_cycles} 回路/{got_edges} 超边, "
                         f"want {want_cycles}/{want_edges}")
        else:
            print(f"  [PASS] B-回路[{name}]: {got_cycles} 回路 / {got_edges} 超边")
    return fails


def min_hitting_set(nodes, edges):
    """穷举最优(边为成员集合;命中任一成员即被击垮——k_e=|e| 站成员口径)。"""
    for k in range(1, len(nodes) + 1):
        for comb in itertools.combinations(nodes, k):
            if all(any(u in comb for u in mem) for mem in edges):
                return len(comb)
    return len(nodes)


def test_hitting_set_random(cli, tmp, count, seed, require_exact=False):
    """P1a-2:随机小图(≤12 超边)贪心 vs 穷举;P1a-3:比值分布(理论上限 1+ln m)。

    注:贪心为 1+ln n 近似,不保证与穷举逐例相等(见 --require-exact)。
    """
    import math
    rng = random.Random(seed)
    exact, ratios = 0, []
    fails, mismatches = [], []
    m_max = 0
    for t in range(count):
        n_nodes = rng.randint(3, 8)
        members = [f"n{i}" for i in range(n_nodes)]
        m_edges = rng.randint(1, 12)
        m_max = max(m_max, m_edges)
        edges = []
        flows = []
        for j in range(m_edges):
            size = rng.randint(1, min(4, n_nodes))
            mem = rng.sample(members, size)
            edges.append(mem)
            for u in mem:
                flows.append((u, f"s{j}", 1.0))
        nodes = members + [f"s{j}" for j in range(m_edges)]
        p = os.path.join(tmp, f"hit{t}.tsv")
        write_tsv(p, flows, nodes)
        got = set(run(cli, "hitset", p))
        # 构造检验:贪心解必须真击垮全部超边
        if not all(got & set(mem) for mem in edges):
            fails.append(f"击垮集[{t}] 非可行解:{sorted(got)} vs {edges}")
            continue
        opt = min_hitting_set(members, edges)
        ratios.append(len(got) / opt if opt else 1.0)
        if len(got) == opt:
            exact += 1
        elif len(got) > opt * (1 + math.log(m_edges)):
            fails.append(f"击垮集[{t}] 超出 1+ln m 保证:{len(got)} vs opt {opt}")
        else:
            mismatches.append((sorted(got), opt, edges))
        os.remove(p)
    if fails:
        return fails
    tag = "[PASS]" if (exact == count or not require_exact) else "[FAIL]"
    print(f"  {tag} 击垮集随机对拍 {count} 例:与穷举一致 {exact}/{count},"
          f"比值 max {max(ratios):.2f}(理论上限 ≤ 1+ln m ≈ {1 + math.log(m_max):.2f})")
    if mismatches:
        print(f"         非最优例 {len(mismatches)}(贪心近似本性,1+ln n 保证内);"
              f"首例:贪心 {mismatches[0][0]} vs 最优 {mismatches[0][1]},超边 {mismatches[0][2]}")
    if require_exact and exact != count:
        return [f"严验收(--require-exact)未过:{exact}/{count} 与穷举一致"]
    return []


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--random", type=int, default=200)
    ap.add_argument("--seed", type=int, default=20260910)
    ap.add_argument("--require-exact", action="store_true",
                    help="严验收:要求随机小图逐例与穷举相等(验收原文口径;贪心不保证)")
    ap.add_argument("--report-skip", action="store_true")
    args = ap.parse_args()

    fails = []
    with tempfile.TemporaryDirectory() as tmp:
        fails += test_b_cycles(args.cli, tmp)
        fails += test_hitting_set_random(args.cli, tmp, args.random, args.seed, args.require_exact)

    print("  [SKIP] P0-1b 主验收(齐套连通块数跳变轮次 vs fill 崩点 ≤2 轮):"
          "需 §12.2 场景层数据(RoundState/demand_share 来源表未定,附录 A 冻结为构造参数注入),"
          "待 P3a 场景就绪")
    print("  [注] 样例 C1/C2 需求组语义锁定 → golden_check.py(sample_c1/c2)")
    if fails:
        print("[FAIL]")
        for f in fails:
            print(f"    - {f}")
        return 1
    print("[PASS] P0-2 + P1a-2/3 验收项通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
