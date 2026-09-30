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
| 12.4–12.5 | CREATE GRAPH / DROP GRAPH | ✓ | → native `CREATE GRAPH` / `DROP GRAPH`; `IF NOT EXISTS` emulated; `CREATE OR REPLACE` ✗ |
| 7 | Session management | △ partial | `SESSION SET GRAPH` → `USE GRAPH`; SCHEMA / TIME ZONE / PARAMETER ✗ |

### Optional features (selected)

| G-feature | Feature | Status | Notes |
|---|---|---|---|
| G035/G036/G037 | Quantified path patterns (`{m,n}`, `*`, `+`, `?`) | ✓ (single-hop) | quantified single edges and single-hop QPPIs `( ()-[]->() ){m,n}` → Cypher var-length `[e*m..n]`; multi-hop / node-quantified QPPIs ✗ |
| G005/G015–G020 | Path search prefixes (ANY SHORTEST / ALL SHORTEST) | ✓ (single-hop) | → `[e*SHORTEST ...]` / `[e*ALL SHORTEST ...]`; counted `SHORTEST k`, `SHORTEST GROUP(S)`, `ALL/ANY PATHS` ✗ |
| G010–G013 | Path modes (WALK/TRAIL/ACYCLIC) | ✓ (single-hop) | → `[e*TRAIL ...]` / `[e*ACYCLIC ...]`; WALK = engine default; SIMPLE ✗ (no engine counterpart); multi-hop path modes ✗ |
| G074 etc. | Label expressions (`&`, `!`, `|`, `%`) | ✗ | rejected explicitly; plain `:Label` works |
| G100 | ELEMENT_ID | ✓ | → `internal_id()` |
| G115 | PROPERTY_EXISTS | ✓ | GQL native predicate (parsed as-is) |
| GA05 | Cast specification | ✓ | `CAST` shared syntax |
| GC03 | CREATE GRAPH TYPE | ✗ | rejected explicitly (schema bridge planned) |

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
7. **ACYCLIC path mode** is approximate: the engine enforces
   intermediate-node distinctness only (start/end nodes are unconstrained, so
   closed walks like `1 -> 2 -> 1` are returned). ISO GQL ACYCLIC requires all
   nodes distinct. TRAIL matches GQL exactly (edges distinct).
8. **GQL has no `LENGTH()`** — ISO GQL spells it `PATH_LENGTH()` for paths
   (mapped to LadybugDB `LENGTH`). A bare `length(...)` call is a GQL syntax
   error at the parser level.
9. **Edge directions**: GQL's undirected / mixed-direction edge spellings
   (`~[e]~`, `<-[e]~`, `~[e]->`, `<->[e]`, ...) collapse to LadybugDB's
   any-direction `-[e]-` (a directed property graph has no undirected edges to
   distinguish).

## Architecture

```
GQL text → ANTLR GQL parser (vendored opengql grammar, ISO-derived)
        → GqlToCypherTransformer (explicit per-statement mapping)
        → Cypher text → engine's standalone-call rewrite → normal pipeline
```

See `src/gql_transformer.cpp` for the mapping logic and
`THIRD_PARTY_NOTICES.md` for adapted upstream work.

## Tests

Dual-run parity tests: every GQL case runs next to the equivalent Cypher and
asserts the same expected result.

```sh
# from repo root (e2e_test built with -DBUILD_EXTENSIONS="gql")
E2E_TEST_FILES_DIRECTORY=extension ./build/.../e2e_test --gtest_filter="gql~test~test_files~*"
# or
make extension-test
```
