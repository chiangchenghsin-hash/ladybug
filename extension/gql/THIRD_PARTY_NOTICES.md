# Third-Party Notices — GQL extension

This extension's GQL→Cypher translation layer adapts mapping rules and
conformance knowledge from the following open-source projects. We gratefully
acknowledge their authors.

## Neo4j Cypher front-end (neo4j/neo4j)

- Source: https://github.com/neo4j/neo4j
- Files consulted / adapted:
  - `community/cypher/front-end/rewriting/src/main/scala/org/neo4j/cypher/internal/rewriting/rewriters/astRewriters/GQLAliasFunctionNameRewriter.scala`
    — GQL function-alias → Cypher function mapping table (re-targeted to
    LadybugDB's own function catalog in `gql_transformer.cpp`
    `GQL_FUNCTION_MAP`).
  - `PropertyExistsToIsNotNull.scala` — PROPERTY_EXISTS → IS NOT NULL rewrite
    semantics (GQL feature G115).
  - `LabelExpressionPredicateNormalizer.scala`, `NormalizeHasLabelsAndHasType.scala`
    — label-expression normalization approach.
  - `community/neo4j-gql-status/` — GQL-status error envelope conventions.
- License: Apache License 2.0
- Copyright (c) "Neo4j" Neo4j Sweden AB

Adapted portions are rewritten in C++ for LadybugDB's parse-tree model and
re-targeted to LadybugDB's Cypher dialect; the Apache-2.0 license terms apply
to the adapted mapping tables/rules. License text:
https://www.apache.org/licenses/LICENSE-2.0

## opengql / LDBC GQL Implementation Working Group (gql-antlr)

- Source: https://github.com/opengql/grammar
- Usage: `extension/third_party/opengql/GQL.g4` (vendored ANTLR grammar for
  ISO GQL, generated from the ISO BNF artifacts via gramgen and hand-tuned).
- License: Apache License 2.0

## ISO/IEC 39075:2024 GQL standard

- The GQL language itself is defined by ISO/IEC 39075:2024. The machine-readable
  grammar artifact (`ISO_IEC_39075(en).bnf.txt` / `.bnf.xml` published at
  standards.iso.org) is used as the reference for language-scope decisions.
- The standard document itself is copyrighted by ISO; only its published
  grammar artifact is used as a language definition reference.
