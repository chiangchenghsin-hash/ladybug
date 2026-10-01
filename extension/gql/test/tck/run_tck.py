#!/usr/bin/env python3
"""opengql/tck conformance runner for the LadybugDB GQL translation layer.

Converts the vendored TCK Gherkin scenarios (extension/gql/test/tck/features)
into LadybugDB e2e .test cases, runs them through the same e2e_test harness the
hand-written conformance suite uses, and writes a pass-rate report.

The TCK is executed in "untyped graph" mode (see the TCK README): every working
graph is created as an open `CREATE GRAPH ... ANY` graph and populated from the
populator file. Sample data comes from tck/data.

Methodology notes (kept honest on purpose):
- Expected results are compared after converting TCK values to the engine's
  Value::toString form (True/False, %.6f floats, empty cell for NULL).
- "an exception condition should be raised: <code>" asserts that SOME error is
  raised; the GQLSTATUS code itself is not checked (the layer does not emit
  GQLSTATUS codes yet).
- Side effects are verified only for scenarios whose working graph starts
  empty (observable metrics: +nodes / +edges / no side effects). Metrics the
  engine cannot observe (+properties, +labels, +schemas, ...) and preloaded
  graphs are recorded as unchecked.
- Scenarios needing sample data that the TCK repo does not ship, and scenarios
  using runtime template substitutions ($(randomLabelSet(...))), are skipped
  and counted separately.

Usage (from the repository root):
  python extension/gql/test/tck/run_tck.py [--filter SUBSTR] [--report PATH]
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import unicodedata

TCK_ROOT = pathlib.Path(__file__).resolve().parent
REPO_ROOT = TCK_ROOT.parents[3]
E2E_BIN = REPO_ROOT / "build_v0211t" / "src" / "Release" / "e2e_test.exe"
GEN_DIR = REPO_ROOT / "_tck_gen"
EXT_PATH = "${LBUG_ROOT_DIRECTORY}/extension/gql/build/libgql.lbug_extension"
DEFAULT_REPORT = TCK_ROOT / "REPORT.md"

# Non-query statements: a line starting one of these begins a new statement.
NON_QUERY_STARTERS = {
    "CREATE", "INSERT", "SET", "DELETE", "REMOVE", "USE", "DROP", "SESSION",
    "START", "COMMIT", "ROLLBACK", "CALL",
}
# Query clauses: they chain into ONE statement until the result part completes it
# ("FOR a IN ... FOR b IN ... RETURN ..." is a single GQL statement).
QUERY_CLAUSES = {"MATCH", "OPTIONAL", "WITH", "FOR", "FILTER", "LET"}
# Result-part words: RETURN completes a query; ORDER/SKIP/LIMIT/FINISH continue it.
RESULT_WORDS = {"RETURN", "ORDER", "SKIP", "LIMIT", "FINISH"}


# -----------------------------------------------------------------------------
# Gherkin parsing
# -----------------------------------------------------------------------------

class Step:
    def __init__(self, keyword: str, text: str, doc: str | None):
        self.keyword = keyword
        self.text = text.strip()
        self.doc = doc


class Scenario:
    def __init__(self, feature: str, tags: list[str], name: str, steps: list[Step]):
        self.feature = feature
        self.tags = tags
        self.name = name
        self.steps = steps


def _dedent_docstring(lines: list[str]) -> str:
    indents = [len(l) - len(l.lstrip()) for l in lines if l.strip()]
    cut = min(indents) if indents else 0
    return "\n".join(l[cut:] if len(l) >= cut else l for l in lines)


def parse_feature(path: pathlib.Path) -> tuple[str, list[str], list[Scenario]]:
    """Return (feature_name, feature_tags, scenarios). Expands Scenario Outlines."""
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    feature_name = path.stem
    feature_tags: list[str] = []
    scenarios: list[Scenario] = []
    pending_tags: list[str] = []

    i = 0
    n = len(lines)
    while i < n:
        raw = lines[i]
        line = raw.strip()
        if not line or line.startswith("#"):
            i += 1
            continue
        if line.startswith("@"):
            for tok in re.split(r"[\s,]+", line):
                if tok.startswith("@"):
                    pending_tags.append(tok[1:])
            i += 1
            continue
        if line.startswith("Feature:"):
            feature_name = line[len("Feature:"):].strip()
            feature_tags = pending_tags
            pending_tags = []
            i += 1
            continue
        if line.startswith("Scenario Outline:"):
            scen_name = line[len("Scenario Outline:"):].strip()
            i += 1
            steps, examples, i = _parse_scenario_body(lines, i)
            for row in examples:
                sub = [Step(s.keyword, _subst(s.text, row),
                            _subst(s.doc, row) if s.doc is not None else None)
                       for s in steps]
                scenarios.append(Scenario(path.stem, feature_tags + pending_tags,
                                          scen_name, sub))
            pending_tags = []
            continue
        if line.startswith("Scenario:"):
            scen_name = line[len("Scenario:"):].strip()
            i += 1
            steps, _examples, i = _parse_scenario_body(lines, i)
            scenarios.append(Scenario(path.stem, feature_tags + pending_tags,
                                      scen_name, steps))
            pending_tags = []
            continue
        i += 1
    return feature_name, feature_tags, scenarios


def _parse_scenario_body(lines: list[str], i: int):
    steps: list[Step] = []
    examples: list[dict[str, str]] = []
    n = len(lines)
    step_re = re.compile(r"^(Given|When|Then|And|But)\s+(.*)$")
    while i < n:
        raw = lines[i]
        line = raw.strip()
        if not line or line.startswith("#"):
            i += 1
            continue
        if line.startswith("@") or line.startswith("Scenario"):
            break
        if line.startswith("Examples:"):
            i += 1
            table_rows: list[list[str]] = []
            while i < n and lines[i].strip().startswith("|"):
                cells = [c.strip() for c in lines[i].strip().strip("|").split("|")]
                table_rows.append(cells)
                i += 1
            if table_rows:
                header = table_rows[0]
                for row in table_rows[1:]:
                    examples.append(dict(zip(header, row)))
            continue
        m = step_re.match(line)
        if not m:
            i += 1
            continue
        keyword, text = m.group(1), m.group(2)
        i += 1
        doc = None
        if i < n and lines[i].strip().startswith('"""'):
            delim_indent = len(lines[i]) - len(lines[i].lstrip())
            i += 1
            body: list[str] = []
            while i < n and not lines[i].strip().startswith('"""'):
                body.append(lines[i][delim_indent:] if len(lines[i]) > delim_indent
                            else lines[i].strip())
                i += 1
            i += 1  # closing """
            doc = _dedent_docstring(body)
        # Gherkin data table attached to the step (result tables, side effects)
        table_lines: list[str] = []
        while i < n and lines[i].strip().startswith("|"):
            table_lines.append(lines[i].strip())
            i += 1
        if table_lines:
            table_text = "\n".join(table_lines)
            doc = table_text if not doc else doc + "\n" + table_text
        steps.append(Step(keyword, text, doc))
    return steps, examples, i


def _subst(text: str | None, row: dict[str, str]) -> str | None:
    if text is None:
        return None
    out = text
    for k, v in row.items():
        out = out.replace(f"<{k}>", v)
    return out


# -----------------------------------------------------------------------------
# Program splitting (TCK "programs" are newline-separated GQL statements)
# -----------------------------------------------------------------------------

def split_program(text: str) -> list[str]:
    statements: list[str] = []
    cur: list[str] = []
    depth = 0
    cur_has_return = False
    cur_is_query = False

    def flush():
        nonlocal cur, cur_has_return, cur_is_query
        joined = "\n".join(cur).strip()
        if joined:
            statements.append(joined)
        cur = []
        cur_has_return = False
        cur_is_query = False

    for raw in text.splitlines():
        if not raw.strip():
            if cur:
                cur.append(raw)
            continue
        leading = len(raw) - len(raw.lstrip())
        first = raw.strip().split()[0].upper().rstrip(":")

        starts_new = False
        if cur and leading == 0 and depth == 0:
            if first in NON_QUERY_STARTERS:
                starts_new = True
            elif first == "RETURN":
                # RETURN continues a query that has no result part yet.
                starts_new = not (cur_is_query and not cur_has_return)
            elif first in QUERY_CLAUSES:
                # MATCH/FOR/FILTER/... continue an open query; begin a new
                # statement only after a completed one.
                starts_new = not (cur_is_query and not cur_has_return)
            # ORDER/SKIP/LIMIT/FINISH and pattern prefixes continue the statement.

        if starts_new:
            flush()
        if not cur:
            cur_is_query = first in QUERY_CLAUSES or first == "RETURN"
        if first == "RETURN":
            cur_has_return = True
        cur.append(raw)

        # track bracket depth outside string literals
        in_str = None
        for ch in raw:
            if in_str:
                if ch == in_str:
                    in_str = None
                continue
            if ch in ("'", '"', "`"):
                in_str = ch
            elif ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth = max(0, depth - 1)
    flush()
    return statements


# -----------------------------------------------------------------------------
# Value conversion: TCK cells -> engine Value::toString form
# -----------------------------------------------------------------------------

def _unescape_gql_string(s: str) -> str:
    return s.replace("\\'", "'").replace('\\"', '"').replace("\\\\", "\\")


def tck_cell_to_expected(cell: str) -> str | None:
    c = cell.strip()
    if c == "null":
        return ""
    if c == "true":
        return "True"
    if c == "false":
        return "False"
    if len(c) >= 2 and c[0] == "'" and c[-1] == "'":
        return _unescape_gql_string(c[1:-1])
    if re.fullmatch(r"-?\d+", c):
        return c
    if re.fullmatch(r"-?(\d+\.\d*|\.\d+|\d+)([eE][+-]?\d+)?", c) and ("." in c or "e" in c.lower()):
        return f"{float(c):.6f}"
    if c.startswith("[") and c.endswith("]"):
        inner = c[1:-1].strip()
        if not inner:
            return "[]"
        parts = _split_commas(inner)
        rendered = []
        for p in parts:
            v = tck_cell_to_expected(p)
            if v is None:
                return None
            if v == "" and p.strip() != "null":
                return None
            rendered.append(v)
        return "[" + ",".join(rendered) + "]"
    return None


def _split_commas(s: str) -> list[str]:
    parts, depth, cur, in_str = [], 0, "", None
    for ch in s:
        if in_str:
            cur += ch
            if ch == in_str:
                in_str = None
            continue
        if ch in ("'", '"'):
            in_str = ch
            cur += ch
            continue
        if ch in "[{":
            depth += 1
        elif ch in "]}":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return parts


# -----------------------------------------------------------------------------
# Statement emission
# -----------------------------------------------------------------------------

def collapse_ws_outside_strings(gql: str) -> str:
    out: list[str] = []
    in_str = None
    pending_space = False
    for ch in gql:
        if in_str:
            out.append(ch)
            if ch == in_str:
                in_str = None
            continue
        if ch in ("'", '"', "`"):
            if pending_space and out:
                out.append(" ")
            pending_space = False
            in_str = ch
            out.append(ch)
            continue
        if ch.isspace():
            pending_space = bool(out)
            continue
        if pending_space:
            out.append(" ")
            pending_space = False
        out.append(ch)
    return "".join(out)


def wrap_call_gql(gql: str) -> str:
    s = collapse_ws_outside_strings(gql)
    s = s.replace("\\", "\\\\").replace('"', '\\"')
    return f'CALL GQL("{s}")'


def sanitize_case_name(name: str) -> str:
    norm = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    norm = re.sub(r"[^A-Za-z0-9]+", "_", norm).strip("_")
    return norm[:100] or "scenario"


class Expectation:
    OK = "ok"
    EMPTY = "empty"
    ROWS = "rows"
    ERROR = "error"


class Emitted:
    """One .test statement with its expectation."""

    def __init__(self, cypher: str, kind: str, rows: list[list[str]] | None = None,
                 ordered: bool = False, headers: bool = False):
        self.cypher = cypher
        self.kind = kind
        self.rows = rows or []
        self.ordered = ordered
        self.headers = headers


def convert_scenario(sc: Scenario) -> tuple[str | None, list[Emitted], list[str]]:
    """Convert one scenario to emitted statements.

    Returns (skip_reason, statements, notes).
    """
    notes: list[str] = []
    emitted: list[Emitted] = []

    def add_gql(gql: str, kind: str = Expectation.OK, **kw):
        for stmt in split_program(gql) if "\n" in gql.strip() else [gql.strip()]:
            pass  # replaced below

    # graph-kind tracking for side-effect observability
    has_working_graph = False
    setup_has_writes = False
    setup_stmts: list[str] = []  # deferred: emitted before when-statements

    def note_write(stmts: list[str]):
        nonlocal setup_has_writes
        for s in stmts:
            if re.match(r"(?is)^\s*(CREATE|INSERT|SET|DELETE|REMOVE)\b", s):
                setup_has_writes = True

    when_result: tuple[str, list, bool] | None = None  # (kind, stmts, ordered)
    when_rows: list[list[str]] = []
    when_exception = False
    side_effect_table: list[tuple[str, str]] | None = None
    no_side_effects = False

    for step in sc.steps:
        t = step.text
        low = t.lower()

        # ---- Given / setup ----
        if low.startswith("an empty catalog"):
            continue
        if low in ("the omitted graph",):
            notes.append("home graph omitted")
            continue
        if low in ("an empty graph", "any graph"):
            has_working_graph = True
            setup_stmts.append(wrap_call_gql("CREATE GRAPH tckwk ANY"))
            setup_stmts.append(wrap_call_gql("SESSION SET GRAPH tckwk"))
            continue
        m = re.fullmatch(r"the (.+) graph", low)
        if m:
            name = m.group(1)
            if "/" in name:
                pop = TCK_ROOT / "data" / f"{name}.gql"
            else:
                pop = TCK_ROOT / "data" / "graphs" / f"{name}.gql"
            if not pop.exists():
                return (f"sample data missing: {pop.relative_to(TCK_ROOT)}", [], notes)
            has_working_graph = True
            setup_stmts.append(wrap_call_gql("CREATE GRAPH tckwk ANY"))
            setup_stmts.append(wrap_call_gql("SESSION SET GRAPH tckwk"))
            pop_text = pop.read_text(encoding="utf-8")
            pop_stmts = split_program(pop_text)
            note_write(pop_stmts)
            for s in pop_stmts:
                setup_stmts.append(wrap_call_gql(s))
            continue
        if low.startswith("the graph created by executing"):
            if not step.doc:
                return ("malformed step: no program", [], notes)
            stmts = split_program(step.doc)
            note_write(stmts)
            for s in stmts:
                setup_stmts.append(wrap_call_gql(s))
            created = None
            for s in stmts:
                mm = re.search(r"(?is)CREATE\s+(?:PROPERTY\s+)?GRAPH\s+(?:IF\s+NOT\s+EXISTS\s+)?(\S+)", s)
                if mm:
                    created = mm.group(1)
            if created:
                setup_stmts.append(wrap_call_gql(f"SESSION SET GRAPH {created}"))
                has_working_graph = True
            continue
        m = re.fullmatch(r"(.+) catalog", low)
        if m:
            name = m.group(1)
            cat = TCK_ROOT / "data" / "catalogs" / f"{name}.gql"
            if not cat.exists():
                return (f"sample data missing: {cat.relative_to(TCK_ROOT)}", [], notes)
            stmts = split_program(cat.read_text(encoding="utf-8"))
            note_write(stmts)
            for s in stmts:
                setup_stmts.append(wrap_call_gql(s))
            continue
        if low.startswith("having executed"):
            if not step.doc:
                return ("malformed step: no program", [], notes)
            stmts = split_program(step.doc)
            note_write(stmts)
            for s in stmts:
                setup_stmts.append(wrap_call_gql(s))
            continue
        if "randomly generated label set" in low:
            return ("requires runtime label-set generation", [], notes)

        # ---- When ----
        if low.startswith("executing query") or low.startswith("executing the program") \
                or low.startswith("executing program"):
            if not step.doc:
                return ("malformed step: no program", [], notes)
            if "$(" in step.doc:
                return ("requires runtime template substitution", [], notes)
            when_result = ("pending", split_program(step.doc), False)
            continue

        # ---- Then / And (assertions) ----
        if "result should be empty" in low:
            when_result = (Expectation.EMPTY, when_result[1], False) if when_result else None
            continue
        if re.match(r"the result should be,\s*in (any )?order", low) or \
                re.match(r"result should be,\s*in (any )?order", low):
            ordered = "any order" not in low
            rows = _parse_result_table(step.doc)
            if rows is None:
                return ("unsupported expected-result notation", [], notes)
            when_result = (Expectation.ROWS, when_result[1], ordered)
            when_rows = rows
            continue
        if "exception condition should be raised" in low:
            when_exception = True
            continue
        if low == "no side effects":
            no_side_effects = True
            continue
        if low.startswith("the side effects should be"):
            side_effect_table = _parse_side_effects(step.doc)
            continue
        if "should have equivalent" in low or "should be equivalent" in low:
            notes.append(f"unchecked step: {t}")
            continue
        return (f"unsupported step: {t}", [], notes)

    if when_result is None and not when_exception:
        return ("scenario has no When query", [], notes)

    emitted = []
    for s in setup_stmts:
        emitted.append(Emitted(s, Expectation.OK))

    when_stmts = [wrap_call_gql(s) for s in (when_result[1] if when_result else [])]
    if when_exception:
        for s in when_stmts[:-1]:
            emitted.append(Emitted(s, Expectation.OK))
        emitted.append(Emitted(when_stmts[-1], Expectation.ERROR))
    elif when_result[0] == Expectation.EMPTY:
        for s in when_stmts:
            emitted.append(Emitted(s, Expectation.EMPTY))
    elif when_result[0] == Expectation.ROWS:
        for s in when_stmts[:-1]:
            emitted.append(Emitted(s, Expectation.OK))
        rows = when_rows
        emitted.append(Emitted(when_stmts[-1], Expectation.ROWS, rows,
                               ordered=when_result[2], headers=True))
    else:
        return ("scenario has no Then result assertion", [], notes)

    # ---- side-effect checks (only observable on empty-start working graphs) ----
    if has_working_graph and not setup_has_writes:
        if no_side_effects:
            emitted.append(Emitted("MATCH (n) RETURN count(*)", Expectation.ROWS,
                                   [["0"]], False, False))
            emitted.append(Emitted("MATCH ()-[e]->() RETURN count(*)", Expectation.ROWS,
                                   [["0"]], False, False))
        elif side_effect_table:
            checked = False
            for metric, val in side_effect_table:
                if metric == "+nodes":
                    emitted.append(Emitted("MATCH (n) RETURN count(*)", Expectation.ROWS,
                                           [[val]], False, False))
                    checked = True
                elif metric == "+edges":
                    emitted.append(Emitted("MATCH ()-[e]->() RETURN count(*)",
                                           Expectation.ROWS, [[val]], False, False))
                    checked = True
                else:
                    notes.append(f"unchecked side effect: {metric} {val}")
            if not checked and side_effect_table:
                pass  # all metrics unobservable; noted above
    else:
        if no_side_effects:
            notes.append("no side effects (unchecked: preloaded graph)")
        elif side_effect_table:
            notes.append("side effects unchecked (preloaded graph or unobservable)")

    return (None, emitted, notes)


def _parse_result_table(doc: str | None):
    if not doc:
        return None
    rows = []
    for line in doc.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        rows.append(cells)
    if not rows:
        return None
    header = rows[0]
    body = rows[1:]
    converted = []
    for r in body:
        conv = []
        for c in r:
            v = tck_cell_to_expected(c)
            if v is None:
                return None
            conv.append(v)
        converted.append(conv)
    return [header] + converted  # header first; CHECK_COLUMN_NAMES


def _parse_side_effects(doc: str | None):
    if not doc:
        return []
    out = []
    for line in doc.splitlines():
        line = line.strip()
        if not line.startswith("|"):
            continue
        cells = [c.strip() for c in line.strip("|").split("|")]
        if len(cells) >= 2:
            out.append((cells[0], cells[1]))
    return out


# -----------------------------------------------------------------------------
# .test rendering
# -----------------------------------------------------------------------------

def render_case(case_name: str, emitted: list[Emitted], comment: str) -> str:
    out = [f"# {comment}", f"-CASE {case_name}", "",
           f'-STATEMENT load extension "{EXT_PATH}"', "---- ok", ""]
    for e in emitted:
        directives = []
        if e.kind == Expectation.ROWS:
            if e.headers:
                directives.append("-CHECK_COLUMN_NAMES")
            if e.ordered:
                directives.append("-CHECK_ORDER")
        out.extend(directives)
        out.append(f"-STATEMENT {e.cypher}")
        if e.kind == Expectation.OK:
            out.append("---- ok")
        elif e.kind == Expectation.EMPTY:
            out.append("---- 0")
        elif e.kind == Expectation.ERROR:
            out.append("---- error(regex)")
            # [\s\S]+ matches multi-line errors too (ANTLR caret messages);
            # plain ".+" does not cross newlines under std::regex_match.
            out.append(r"[\s\S]+")
        elif e.kind == Expectation.ROWS:
            rows = e.rows
            out.append(f"---- {len(rows)}")
            for r in rows:
                out.append("|".join(r))
        out.append("")
    return "\n".join(out) + "\n"


# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------

# Implementation-capability tags: scenarios that only apply to implementations
# with capabilities LadybugDB's schema bridge does not have. Per the TCK README
# consumers construct tag expressions matching their implementation; we skip
# these instead of counting them as failures.
CAPABILITY_SKIP_TAGS = {
    "MinNodeLabelsZero": "node types need at least one label (LadybugDB table name)",
    "MinNodeTypeKeyLabelsZero": "key label sets need at least one identifying label",
    "MaxNodeLabelsGTOne": "LadybugDB nodes have a single label",
    "MaxNodeTypeKeyLabelsGTOne": "LadybugDB nodes have a single label",
}


def collect_scenarios(filter_: str | None):
    features = sorted((TCK_ROOT / "features").rglob("*.feature"))
    all_sc = []
    for f in features:
        _, _, scens = parse_feature(f)
        for s in scens:
            if filter_ and filter_.lower() not in f.stem.lower() and \
                    filter_.lower() not in s.name.lower():
                continue
            all_sc.append((f, s))
    return all_sc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--filter", default=None, help="substring filter on feature/scenario")
    ap.add_argument("--report", default=str(DEFAULT_REPORT))
    ap.add_argument("--keep", action="store_true", help="keep generated .test files")
    args = ap.parse_args()

    if not E2E_BIN.exists():
        print(f"e2e_test binary not found: {E2E_BIN} (build with _build_gql.bat)", file=sys.stderr)
        return 2

    all_sc = collect_scenarios(args.filter)
    GEN_DIR.mkdir(exist_ok=True)
    for old in GEN_DIR.glob("*.test"):
        old.unlink()

    index: dict[str, tuple[str, str, list[str]]] = {}  # case -> (feature, scenario, notes)
    skipped: list[tuple[str, str, str]] = []
    by_file: dict[str, list[str]] = {}

    file_cases: dict[pathlib.Path, list[str]] = {}
    case_counter: dict[str, int] = {}
    def feat_key(path: pathlib.Path) -> str:
        rel = path.relative_to(TCK_ROOT / "features").with_suffix("")
        return sanitize_case_name(str(rel).replace("\\", "/").replace("/", "_"))

    for fpath, sc in all_sc:
        fk = feat_key(fpath)
        cap = next((t for t in sc.tags if t in CAPABILITY_SKIP_TAGS), None)
        if cap is not None:
            skipped.append((fk, sc.name,
                            f"capability tag @{cap}: {CAPABILITY_SKIP_TAGS[cap]}"))
            continue
        skip, emitted, notes = convert_scenario(sc)
        if skip is not None:
            skipped.append((fk, sc.name, skip))
            continue
        base = sanitize_case_name(f"{fk}_{sc.name}")
        k = case_counter.get(base, 0)
        case_counter[base] = k + 1
        case = base if k == 0 else f"{base}_{k}"
        comment = f"{fpath.relative_to(TCK_ROOT)} :: {sc.name} [tags: {', '.join(sc.tags) or '-'}]"
        out_path = GEN_DIR / f"{fk}.test"
        file_cases.setdefault(out_path, []).append(render_case(case, emitted, comment))
        index[case] = (fk, sc.name, notes)
        by_file.setdefault(fk, []).append(case)

    for out_path, blocks in file_cases.items():
        header = "-DATASET CSV empty\n\n--\n\n"
        out_path.write_text(header + "\n".join(blocks), encoding="utf-8")

    print(f"generated {len(index)} cases in {GEN_DIR} "
          f"(skipped {len(skipped)} of {len(all_sc)} scenarios)")

    env = {"E2E_TEST_FILES_DIRECTORY": "_tck_gen", "LBUG_ROOT_DIRECTORY": str(REPO_ROOT)}
    proc = subprocess.run(
        [str(E2E_BIN), "--gtest_filter=*"],
        cwd=str(REPO_ROOT), env={**dict(**__import__("os").environ), **env},
        capture_output=True, text=True)
    log = proc.stdout + proc.stderr
    (GEN_DIR / "run.log").write_text(log, encoding="utf-8")

    failed = re.findall(r"^\[  FAILED  \] ([A-Za-z0-9_.]+) \(\d+ ms\)", log, re.M)
    passed_m = re.search(r"^\[  PASSED  \] (\d+) tests?", log, re.M)
    n_pass = int(passed_m.group(1)) if passed_m else 0
    n_fail = len(set(failed))
    # gtest "N tests from M suites" totals
    tot_m = re.search(r"\[==========\] (\d+) tests? from", log)
    n_total = int(tot_m.group(1)) if tot_m else n_pass + n_fail

    # classify failure messages (first error line per failed case)
    fail_msgs: dict[str, str] = {}
    for name in set(failed):
        grp, case = name.split(".", 1) if "." in name else ("", name)
        m = re.search(
            rf"\[ RUN      \] {re.escape(name)}\n(.*?)(?:\[       OK \]|\[  FAILED  \])",
            log, re.S)
        if m:
            block = m.group(1)
            err = re.search(
                r"(?:EXPECT OK BUT GOT ERROR|Unexpected error for query|"
                r"Result tuple at index|Which is|error: Expected|"
                r"error: Value of: std::regex_match)[^\n]*", block)
            fail_msgs[name] = (err.group(0)[:160] if err else "unknown").strip()

    # ---- report ----
    lines = ["# opengql/tck conformance report — LadybugDB GQL translation layer", ""]
    lines.append(f"- TCK vendored at `extension/gql/test/tck/` "
                 f"(opengql/tck, Apache-2.0 — see NOTICE.md; openCypher-derived "
                 f"features retain their Neo4j attribution headers).")
    lines.append(f"- Mode: untyped graphs (`CREATE GRAPH ... ANY` + populator).")
    lines.append(f"- Scenarios run: **{n_pass + n_fail}** executed, "
                 f"**{n_pass} passed**, **{n_fail} failed**, "
                 f"**{len(skipped)} skipped**.")
    if n_total and n_total != n_pass + n_fail:
        lines.append(f"- gtest total: {n_total}.")
    lines.append("")
    lines.append("Methodology: expected results are compared in the engine's "
                 "Value::toString form; exception scenarios assert that *an* error "
                 "is raised (GQLSTATUS codes are not emitted by the layer yet); "
                 "side effects are checked only for empty-start working graphs "
                 "and observable metrics (+nodes/+edges).")
    lines.append("")

    lines.append("## Per feature")
    lines.append("")
    lines.append("| Feature | run | passed | failed | skipped |")
    lines.append("|---|---|---|---|---|")
    all_feats = sorted(set([feat_key(f) for f, _ in all_sc] + list(by_file.keys())))
    for feat in all_feats:
        cases = by_file.get(feat, [])
        failed_cases = {x.split(".", 1)[-1] for x in failed}
        f_fail = sum(1 for c in cases if c in failed_cases)
        f_pass = len(cases) - f_fail
        f_skip = sum(1 for s in skipped if s[0] == feat)
        lines.append(f"| {feat} | {len(cases)} | {f_pass} | {f_fail} | {f_skip} |")
    lines.append("")

    if skipped:
        lines.append("## Skipped scenarios")
        lines.append("")
        for feat, name, why in skipped:
            lines.append(f"- `{feat}` :: {name} — {why}")
        lines.append("")

    def classify(msg: str) -> str:
        low = msg.lower()
        if "not supported" in low or "unsupported" in low:
            return "rejected-by-layer"
        if "failed to parse gql" in low or "parser exception" in low:
            return "parse-error"
        if "regex_match" in low:
            if "actual error:" in low:
                # an error WAS raised; the regex just failed to match it
                return "error-regex-mismatch"
            return "expected-exception-not-raised"
        if "did not match" in low or "expected equality" in low or "which is" in low:
            return "result-mismatch"
        if "isSuccess" in low:
            return "unexpected-error"
        return "other"

    if failed:
        lines.append("## Failures (classified)")
        lines.append("")
        classes: dict[str, int] = {}
        for name in sorted(set(failed)):
            case = name.split(".", 1)[-1]
            feat, scname, _ = index.get(case, ("?", "?", []))
            msg = fail_msgs.get(name, "")
            cls = classify(msg)
            classes[cls] = classes.get(cls, 0) + 1
            lines.append(f"- `{feat}` :: {scname} — [{cls}] {msg}")
        lines.append("")
        lines.append("Failure classes: " +
                     ", ".join(f"{k}={v}" for k, v in sorted(classes.items())))
        lines.append("")

    unchecked_notes = [(c, n) for c, (_f, _s, n) in index.items() if n]
    if unchecked_notes:
        lines.append("## Unchecked assertions (best-effort)")
        lines.append("")
        for case, notes in unchecked_notes:
            for n in notes:
                lines.append(f"- `{case}`: {n}")
        lines.append("")

    report_path = pathlib.Path(args.report)
    report_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"report written to {report_path}")
    print(f"RESULT: {n_pass} passed, {n_fail} failed, {len(skipped)} skipped")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
