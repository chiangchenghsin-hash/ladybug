# GQL extension — ISO GQL compatibility layer for LadybugDB

`CALL GQL("<ISO GQL statement>")` translates GQL to LadybugDB's native Cypher
dialect and executes it. GQL and the equivalent Cypher produce the same result
sets (see the dual-run parity tests in `test/test_files/`).

This is a **translation layer**, not a native GQL execution engine: LadybugDB's
query engine is Cypher-dialect; this extension maps GQL statements onto it.

## Usage

```sql
LOAD EXTENSION '.../libgql.lbug_extension';
CALL GQL("INSERT (n:Person {name: 'Alice', age: 30})");
CALL GQL("SELECT n.name AS name, count(*) AS c FROM GRAPH main MATCH (n:Person)");
```

Notes:

- `CALL GQL(...)` must be the only statement in the query batch (engine
  rewrite-path constraint).
- Unmapped GQL constructs fail fast with
  `GQL feature not supported: <construct>` — there is no silent pass-through
  to the Cypher parser.
- LadybugDB-specific DDL (`CREATE NODE TABLE`, `COPY ...`, ...) can be passed
  through `CALL GQL(...)` as a convenience; this is a private extension, not
  ISO GQL.

## Compatibility matrix (graded)

Format follows the GQL conformance model: mandatory features (by standard
subclause) + optional features (G-feature IDs). ✓ = implemented and covered by
parity tests; ✗ = explicitly rejected with `GQL feature not supported`.

### Mandatory features

| Subclause | Feature | Status | Mapping |
|---|---|---|---|
| 8 | Transaction management (START TRANSACTION / COMMIT / ROLLBACK) | ✓ | → `BEGIN TRANSACTION` / `COMMIT` / `ROLLBACK` |
| 14.4 | MATCH / OPTIONAL MATCH | ✓ | pass-through (pattern syntax shared) |
| 14.6 | FILTER | ✓ | → `WHERE` |
| 14.8 | FOR ... IN | ✓ (basic) | → `UNWIND ... AS` (WITH ORDINALITY/OFFSET ✗) |
| 14.9–14.10 | ORDER BY / SKIP / LIMIT | ✓ | OFFSET → `SKIP` |
| 14.11 | RETURN (incl. GROUP BY on RETURN) | ✓ | grouping → `WITH ... RETURN ...` |
| 14.12 | SELECT ... FROM GRAPH ... | ✓ | → `USE GRAPH` + `MATCH ... RETURN ...` |
| 16.15 | GROUP BY / HAVING (incl. implicit grouping) | ✓ | → `WITH keys, aggs WHERE ... RETURN ...` |
| 13.2 | INSERT | ✓ | → `CREATE` |
| 13.3 | SET | ✓ (approx.) | order-independent assignment emulated via `WITH` snapshot when needed; `SET n:Label` ✗ |
| 13.4 | REMOVE | △ approx. | property → `SET n.prop = NULL` (fixed-schema approximation of "property removed"); label removal ✗ |
| 13.5 | DELETE / DETACH DELETE | ✓ | → `DELETE` / `DETACH DELETE` |
| 12.4–12.5 | CREATE GRAPH / DROP GRAPH | ✓ | `ANY` → native `CREATE GRAPH g ANY` (open graph); typed (`CREATE GRAPH g t` / inline `{ ... }`) → `CREATE GRAPH` + `CREATE NODE/REL TABLE` DDL; `IF NOT EXISTS` emulated; `CREATE OR REPLACE` ✗; `LIKE <graph>` / `AS COPY OF <graph>` ✗ |
| 12.6–12.7 | CREATE GRAPH TYPE / DROP GRAPH TYPE | ✓ (basic) | graph type registered in extension memory (see notes); `AS { spec }` / `AS COPY OF <type>` ✓; `LIKE <graph>` ✗ |
| 7 | Session management | △ partial | `SESSION SET GRAPH` → `USE GRAPH`; SCHEMA / TIME ZONE / PARAMETER ✗ |

### Optional features (selected)

| G-feature | Feature | Status | Notes |
|---|---|---|---|
| G035/G036/G037 | Quantified path patterns (`{m,n}`, `*`, `+`, `?`) | ✓ (single-hop) | quantified single edges and single-hop QPPIs `( ()-[]->() ){m,n}` → Cypher var-length `[e*m..n]`; multi-hop / node-quantified QPPIs ✗ |
| G005/G015–G020 | Path search prefixes (ANY SHORTEST / ALL SHORTEST) | ✓ (single-hop) | → `[e*SHORTEST ...]` / `[e*ALL SHORTEST ...]`; counted `SHORTEST k`, `SHORTEST GROUP(S)`, `ALL/ANY PATHS` ✗ |
| G010–G013 | Path modes (WALK/TRAIL/ACYCLIC) | ✓ | single var-length slot → `[e*TRAIL ...]` / `[e*ACYCLIC ...]`; multi-hop patterns (and every ACYCLIC pattern) additionally bind a path variable and filter with `IS_TRAIL`/`IS_ACYCLIC` over the whole path — exact ISO semantics (see difference 7); WALK = engine default; SIMPLE ✗ (no engine counterpart) |
| G074 etc. | Label expressions (`&`, `!`, `|`, `%`) | ✓ (node patterns) | `:A&B`/`:A\|B`/`:!A`/parens → WHERE predicates over `labels(v)` (graph-kind-aware, see difference 13); simple `:Label` unchanged (table pruning); INSERT label sets `:A&B` → `CREATE (n:A:B ...)` on ANY graphs; `%` wildcard, edge label expressions, `IS LABELED` predicates ✗ |
| G100 | ELEMENT_ID | ✓ | → `internal_id()` |
| G115 | PROPERTY_EXISTS | ✓ | GQL native predicate (parsed as-is) |
| GA05 | Cast specification | ✓ | `CAST` shared syntax |
| GC03 | CREATE GRAPH TYPE | ✓ (basic) | graph type → node/rel table schema bridge; multi-label node types and `LIKE <graph>` ✗ |

Quantifier bounds follow the GQL/Neo4j semantics — note that GQL `*` is
**zero**-or-more (`[e*0..]` in Cypher; Cypher's bare `*` is one-or-more), `+`
is one-or-more, `?` is `{0,1}`. Lower bound 0 binds start = end with an empty
edge list. `DIFFERENT EDGES` match mode is rejected; `REPEATABLE ELEMENTS` is
dropped (LadybugDB MATCH already allows edge repetition).

### Function-name mapping

Most GQL function names match LadybugDB's catalog directly
(`UPPER`/`LOWER`/`CEILING`/`LN`/`COUNT`/`SUM`/...). Divergent spellings are
mapped (table adapted from Neo4j's `GQLAliasFunctionNameRewriter`, Apache-2.0 —
see `THIRD_PARTY_NOTICES.md`):

| GQL | LadybugDB |
|---|---|
| `COLLECT_LIST` | `COLLECT` |
| `PERCENTILE_DISC` / `PERCENTILE_CONT` | `PERCENTILEDISC` / `PERCENTILECONT` |
| `CHAR_LENGTH` / `CHARACTER_LENGTH` | `SIZE` |
| `LOCAL_DATETIME` / `ZONED_DATETIME` | `TIMESTAMP` |
| `PATH_LENGTH` | `LENGTH` |
| `ELEMENT_ID` | `internal_id` |
| `LOCAL_TIME` / `ZONED_TIME` | ✗ (no TIME type in LadybugDB) |
| `STDDEV_SAMP` / `STDDEV_POP` | ✗ (no such aggregate in LadybugDB) |

`||` (GQL string concatenation) maps to `+`.

### Known semantic differences

1. **REMOVE property** is approximated as `SET n.prop = NULL` — on LadybugDB's
   fixed schema the property column still exists (as NULL); ISO GQL "property
   removed" would make `PROPERTY_EXISTS`/`properties()` behave differently.
2. **SET assignment order** — GQL evaluates all RHS values before assigning
   (order-independent); Cypher `SET` is sequential. The translator snapshots
   RHS values via `WITH` when items interfere; otherwise direct mapping is
   equivalent.
3. **FROM GRAPH / SESSION SET GRAPH** route through `USE GRAPH`, which is
   session-sticky — the session's current graph stays switched after
   `CALL GQL` returns.
4. **Transaction-wrapped programs** (`START TRANSACTION <body> COMMIT` in one
   call) are rejected: the engine rewrite path would expose the COMMIT message
   instead of query rows. Issue BEGIN/COMMIT as separate statements.
5. **NEXT composition** across statements is not supported (Cypher
   multi-statements do not share binding scope).
6. `SELECT *` with GROUP BY/HAVING is not supported (use explicit items).
7. **Path modes are enforced exactly** (ISO GQL TRAIL = all edges distinct,
   ACYCLIC = all nodes distinct). A single var-length segment maps onto the
   engine's `*TRAIL`/`*ACYCLIC` recursive types — note the engine's `*ACYCLIC`
   alone only distincts intermediate nodes (start/end unconstrained) — and
   every ACYCLIC pattern, plus any TRAIL pattern with more than one edge, also
   binds a path variable and filters with `IS_TRAIL`/`IS_ACYCLIC` over the
   whole path (closed walks like `1 -> 2 -> 1` are correctly excluded).
8. **GQL has no `LENGTH()`** — ISO GQL spells it `PATH_LENGTH()` for paths
   (mapped to LadybugDB `LENGTH`). A bare `length(...)` call is a GQL syntax
   error at the parser level.
9. **Edge directions**: GQL's undirected / mixed-direction edge spellings
   (`~[e]~`, `<-[e]~`, `~[e]->`, `<->[e]`, ...) collapse to LadybugDB's
   any-direction `-[e]-` (a directed property graph has no undirected edges to
   distinguish).
10. **Graph types are per-database memory**: `CREATE GRAPH TYPE` registers the
    canonicalized type in extension state attached to the open database
    (LadybugDB has no graph-type catalog object). Types survive across
    `CALL GQL` calls and follow catalog semantics within one database (so
    independent sessions/tests do not leak types into each other), but they are
    not persisted to WAL — re-run `CREATE GRAPH TYPE` after a restart. Graph
    *names* and their schemas are durable; only the type registry is not.
11. **Synthetic primary key**: expanding a graph type adds
    `_gql_id SERIAL PRIMARY KEY` to every node table (GQL node types have no key
    property; LadybugDB node tables require one). The column is auto-filled on
    INSERT and is a regular hidden-ish property; `_gql_id` is rejected as a
    user property name.
12. **`NOT NULL` is dropped**: GQL property types may carry `NOT NULL`; LadybugDB
    DDL has no NOT NULL constraint, so it is accepted and ignored.
13. **Multi-label semantics depend on the graph kind.** Typed/tabled graphs
    give a node exactly one label (= table name), so a conjunction of distinct
    labels (`:A&B`) matches nothing and multi-label `INSERT` is rejected (the
    engine would silently create nothing there). Open ANY graphs store a label
    set (`STRING[]`) and carry real multi-label nodes: `INSERT (n:A&B ...)`,
    `MATCH (n:A&B)`, `:A|B`, `:!A` all work there. Compound label expressions
    translate to WHERE predicates over `labels(v)` chosen by graph kind (ANY:
    `list_contains`; typed: scalar equality) — the colon spelling is never
    reused for compound expressions because `:A:B` means AND on ANY graphs but
    OR (table union) on typed graphs. Multi-label node *types* in
    `CREATE GRAPH TYPE` remain unsupported. Undirected edge types are rejected
    too (LadybugDB rel tables are directed).
14. **`CREATE GRAPH g t` spelling**: ISO GQL references a graph type by bare
    name; the commonly generated `CREATE GRAPH g TYPE t` is accepted as a
    lenient pre-parse normalization (same idea as `FROM GRAPH`).
    `CREATE GRAPH TYPE t AS COPY OF u` is reinterpreted past a grammar
    ambiguity (unquoted `TYPE` can be a graph name).
15. **GQL has no bare `TIME` type** — only `LOCAL TIME`, `ZONED TIME`,
    `TIME [WITH|WITHOUT TIME ZONE]`. LadybugDB has no TIME type at all; all
    spellings are rejected.
16. **Unnamed result columns** are named after their GQL source text
    (`RETURN max(x)` → column `max(x)`), which is the GQL convention;
    LadybugDB's engine alone would uppercase function names.
17. **Heterogeneous list literals are rejected**: GQL list literals preserve
    per-element types (`[1, 2.0]` holds an INT64 and a DOUBLE); LadybugDB
    homogenizes mixed literals to one element type at bind time (STRING is the
    universal sink), which silently erases types and would make `max()`/`min()`
    compare the wrong values. Unyped literals whose element classes disagree
    (INT vs DOUBLE count as different classes; `null` ignored) are rejected with
    `GQL feature not supported: heterogeneous list literal`. Non-literal
    elements cannot be judged statically and may still homogenize at runtime.
    **Map/record values** (`{}`, `{k: v}`) in expressions are rejected too
    (`... map value`) — LadybugDB expressions have no map type.
18. **`FILTER` maps to `WITH * WHERE`** so it composes after `FOR`/`MATCH`;
    result semantics are unchanged.
19. **Boolean operators do type-check their operands**: `123 AND true` raises
    a BinderException in LadybugDB as GQL requires (earlier TCK reports claimed
    otherwise — that was a test-harness regex bug matching multi-line errors,
    now fixed).

### Graph type → schema mapping

| GQL graph type | LadybugDB DDL |
|---|---|
| `CREATE GRAPH g ANY` | `CREATE GRAPH g ANY` (open graph) |
| `NODE Person {name STRING, age INT64}` / `(:Person {name STRING, age INT64})` | `CREATE NODE TABLE Person(_gql_id SERIAL PRIMARY KEY, name STRING, age INT64)` |
| `EDGE KNOWS CONNECTING (Person TO Person) {since INT64}` / `(:Person)-[:KNOWS {since INT64}]->(:Person)` | `CREATE REL TABLE KNOWS(FROM Person TO Person, since INT64)` |
| `CREATE GRAPH TYPE t AS { ... }` | registered in the extension's type registry |
| `CREATE GRAPH g t` / `CREATE GRAPH g { ... }` | `CREATE GRAPH g; USE GRAPH g;` + the table DDL above |

Property value types map as: `BOOL`/`BOOLEAN`→BOOL, `STRING`/`CHAR`/`VARCHAR`→STRING,
`BYTES`/`BINARY`/`VARBINARY`→BLOB, `INT8..64`/`INTEGER8..64`/`SMALLINT`/`INT`/
`BIGINT`/`INTEGER`→INT8..64/INT16/INT32/INT64, `UINT*` likewise, `FLOAT32`/`FLOAT`/
`REAL`→FLOAT, `FLOAT64`/`DOUBLE`→DOUBLE, `DECIMAL(p,s)`→DECIMAL(p,s), `DATE`→DATE,
`TIMESTAMP`/`LOCAL DATETIME`→TIMESTAMP, `TIMESTAMP WITH TIME ZONE`/`ZONED DATETIME`
→TIMESTAMP_TZ, `DURATION(...)`→INTERVAL. Length/precision qualifiers on strings and
`FLOAT` are ignored. Everything else (TIME forms, lists, structs, ANY, ...) is
rejected explicitly.

## Architecture

```
GQL text → ANTLR GQL parser (vendored opengql grammar, ISO-derived)
        → GqlToCypherTransformer (explicit per-statement mapping)
        → Cypher text → engine's standalone-call rewrite → normal pipeline
```

See `src/gql_transformer.cpp` for the mapping logic and
`THIRD_PARTY_NOTICES.md` for adapted upstream work.

## Conformance (opengql/tck)

The vendored [opengql/tck](https://github.com/opengql/tck) suite (Apache-2.0,
see `test/tck/NOTICE.md`) is executed by `test/tck/run_tck.py`, which converts
the Gherkin scenarios to the same e2e harness the hand-written suite uses.

Measured on 2026-10-01 (untyped-graph mode; `python extension/gql/test/tck/run_tck.py`):

| Feature area | run | passed | failed | skipped |
|---|---|---|---|---|
| expressions / boolean | 149 | 149 | 0 | 1 |
| expressions / aggregation | 16 | 6 | 10 | 0 |
| catalog / create graph types | 7 | 5 | 2 | 8 |
| catalog / create graphs | 8 | 4 | 4 | 0 |
| catalog / create+drop schemas | 14 | 1 | 13 | 2 |
| debug | 1 | 0 | 1 | 0 |
| **total** | **195** | **165** | **30** | **11** of 206 |

Failure classes: rejected-by-layer 23 (schema namespaces, `LIKE`/`AS COPY OF`,
multi-label node types — unsupported by design — plus 4 heterogeneous list
literals, see difference 17), parse-error 4 (TCK setup uses openCypher
`CREATE (...)`/`UNWIND`, which is not GQL — GQL writes `INSERT`/`FOR`; plus one
`CREATE GRAPH ANY AS COPY OF` grammar ambiguity), other 3 (`MIN`/`MAX`/`SUM`
over list values — the engine has no list-typed aggregates; the binder rejects
them loudly). All failures are loud: the layer has **no silent wrong-answer
class**. See `test/tck/REPORT.md` for the per-scenario listing and methodology
(exception scenarios assert that *an* error is raised — GQLSTATUS codes are not
emitted yet; side effects are checked only where observable).

Skipped scenarios are either capability-tagged for features LadybugDB does not
have (`@MinNodeLabelsZero`, `@MaxNodeLabelsGTOne`, ...) or reference sample data
the TCK repo does not ship.

## Tests

Dual-run parity tests: every GQL case runs next to the equivalent Cypher and
asserts the same expected result.

```sh
# from repo root (e2e_test built with -DBUILD_EXTENSIONS="gql")
E2E_TEST_FILES_DIRECTORY=extension ./build/.../e2e_test --gtest_filter="gql~test~test_files~*"
# or
make extension-test

# TCK conformance run (writes test/tck/REPORT.md)
python extension/gql/test/tck/run_tck.py
```
