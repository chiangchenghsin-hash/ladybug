# GQL reference sources (`docs/gql_ref/`)

Upstream reference sources consulted — and in part adapted — while building the
GQL→Cypher translation layer in `extension/gql/`; the adapted logic lives in C++
in `extension/gql/src/gql_transformer.cpp`. This file indexes the copies kept
here and records what each one informed;
`extension/gql/THIRD_PARTY_NOTICES.md` is the authoritative provenance and
licensing register — see it for license terms and copyright lines.

| File | Upstream source (as recorded in THIRD_PARTY_NOTICES.md) | License | What it informed |
| --- | --- | --- | --- |
| `ISO_IEC_39075.bnf.txt` | ISO/IEC 39075 grammar artifact `ISO_IEC_39075(en).bnf.txt` / `.bnf.xml`, published at standards.iso.org (local copy renamed) | ISO copyright — see note below | Language-scope decisions for the translation layer (notices: reference for language-scope decisions) |
| `Cypher25Parser.g4` | Neo4j Cypher 25 grammar — `https://github.com/neo4j/neo4j` (grouped with the Neo4j sources in notices' "Reference copies"; exact upstream path not recorded) | Apache-2.0, © Neo4j Sweden AB (file header) | Consulting reference for the target Cypher dialect surface (`USE`, transaction commands, path modes, `PROPERTY_EXISTS` predicate); no code cites it — the engine parses the rewritten Cypher with its own grammar (`src/antlr4/Cypher.g4`) |
| `astRewriters/GQLAliasFunctionNameRewriter.scala` | `community/cypher/front-end/rewriting/src/main/scala/org/neo4j/cypher/internal/rewriting/rewriters/astRewriters/GQLAliasFunctionNameRewriter.scala` — `https://github.com/neo4j/neo4j` | Apache-2.0, © Neo4j Sweden AB | `GQL_FUNCTION_MAP` function-name mapping table in `gql_transformer.cpp` |
| `astRewriters/LabelExpressionPredicateNormalizer.scala` | `astRewriters/LabelExpressionPredicateNormalizer.scala` — `https://github.com/neo4j/neo4j` (filename per notices; directory from the file's package declaration) | Apache-2.0, © Neo4j Sweden AB | Label-expression normalization approach → G074 compound label expressions become `WHERE` predicates over `labels(v)` (`translateLabelExpression`) |
| `astRewriters/NormalizeHasLabelsAndHasType.scala` | `astRewriters/NormalizeHasLabelsAndHasType.scala` — `https://github.com/neo4j/neo4j` (as above) | Apache-2.0, © Neo4j Sweden AB | Label-expression normalization approach (grouped with the file above in notices) → simple `:Label` stays in the pattern spelling, compound forms become predicates (`translateFiller` in `gql_transformer.cpp`) |
| `astRewriters/PropertyExistsToIsNotNull.scala` | `astRewriters/PropertyExistsToIsNotNull.scala` — `https://github.com/neo4j/neo4j` (as above) | Apache-2.0, © Neo4j Sweden AB | G115 `PROPERTY_EXISTS(v, prop)` → `(v.prop IS NOT NULL)` rewrite, adapted in `mapIdentifiersInto` (`gql_transformer.cpp`) — "property present" modeled as "column not null" on the fixed schema (`extension/gql/README.md`, G115 + difference 1) |
| `graphType/GraphTypeCanonicalizer.scala` | `community/cypher/front-end/rewriting/src/main/scala/org/neo4j/cypher/internal/rewriting/rewriters/astRewriters/GraphTypeCanonicalizer.scala` @ neo4j/neo4j `54a7dcf7c2501b31866199143364c5332da8936f` (source comment in the file; filename also in notices) | Apache-2.0, © Neo4j Sweden AB | `GraphTypeSpec` canonical form for `CREATE GRAPH TYPE` → node/rel table DDL schema bridge in `gql_transformer.cpp` |

## ISO/IEC 39075 grammar artifact

`ISO_IEC_39075.bnf.txt` is the ISO "digital artifact" grammar of ISO/IEC 39075;
its header states it "may be used by implementers of GQL-implementations when
generating parsers for the GQL language". It is ISO-copyrighted and kept as an
**internal reference only**: excluded from git via `docs/gql_ref/.gitignore`,
do not redistribute it outside this repository, and do not present it as our own
work. Per THIRD_PARTY_NOTICES.md only the published grammar artifact — not the
standard text — is used as a language definition reference.

## Consulted but not kept

Consulted during development but not copied into this directory; re-fetch from:

- `AddElementUniquenessPredicates.scala`, `AddPathPredicates.scala` —
  neo4j/neo4j front-end `rewriting/.../astRewriters/`; quantifier bounds
  (`*`→0..∞, `+`→1..∞, `{m,n}`→m..n) and path-mode uniqueness, adapted for the
  single-hop QPPI → var-length mapping in `gql_transformer.cpp` `quantifierRange`.
- `AddVarLengthBoundPredicates.scala` — neo4j/neo4j `astRewriters/`; consulted
  per `_HANDOVER_GQL.md` (local copy `_tmp_gql_ref/astRewriters/...`; not listed
  in THIRD_PARTY_NOTICES.md).
- `PathMode.scala` — `front-end/expressions/.../PathMode.scala` (notices);
  GQL path-mode semantics reference (`PathMode.effectivePathMode`).
- `QuantifiedPathPatternConverters.scala` — neo4j/neo4j `ir/`; consulted per
  `_HANDOVER_GQL.md` (local copy `_tmp_gql_ref/ir/...`; not listed in notices).
- `community/neo4j-gql-status/` — neo4j/neo4j; GQL-status error envelope
  conventions.
- The vendored opengql GQL grammar lives at
  `extension/third_party/opengql/GQL.g4` (`https://github.com/opengql/grammar`)
  and is not duplicated here.
