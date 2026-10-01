#pragma once

#include "GQLParser.h"

// ANTLR exposes this implementation detail as a macro.
#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lbug {
namespace gql_extension {

// Canonical graph type model (aliases stripped — the form Neo4j's
// GraphTypeCanonicalizer normalizes to; see THIRD_PARTY_NOTICES.md).
struct GraphTypeProp {
    std::string name;
    std::string type; // LadybugDB column type text (INT64, STRING, ...)
};

struct GraphTypeNode {
    std::string name;
    std::vector<GraphTypeProp> props;
};

struct GraphTypeEdge {
    std::string name;
    std::string from; // endpoint node type name
    std::string to;
    std::vector<GraphTypeProp> props;
};

struct GraphTypeSpec {
    std::vector<GraphTypeNode> nodes;
    std::vector<GraphTypeEdge> edges;
};

// Graph-type registry keyed by upper-cased type name. Owned by the caller
// (per-database state — see GqlExtension::load / ExtensionManager::setData).
using GraphTypeRegistry = std::map<std::string, GraphTypeSpec>;

// GQL → Cypher translator (the dialect bridge for LadybugDB's Cypher engine).
//
// Design: explicit top-level dispatch over the GQL parse tree — every statement
// kind is either translated, routed to an equivalent native Cypher statement,
// or rejected with a clear "GQL feature not supported" error. There is no
// whole-text pass-through fallback: a GQL statement that we do not understand
// must never reach the Cypher parser and fail there with a misleading error.
//
// Composition model: one CALL GQL statement translates to one (occasionally
// multi-statement, ';'-separated) Cypher query string, re-parsed by the
// standalone-call rewrite path in ClientContext. The last Cypher statement is
// the visible one.
//
// Function-name mappings are adapted from Neo4j's Cypher front-end
// (GQLAliasFunctionNameRewriter, Apache-2.0) and re-targeted to LadybugDB's
// own function catalog. See THIRD_PARTY_NOTICES.md.
class GqlToCypherTransformer {
public:
    // Resolves whether a named graph ("" = the session's current graph) is an
    // open ANY graph (labels live in a STRING[] column) vs a typed/tabled graph
    // (one label = table name). Returns nullopt when the graph cannot be
    // resolved. Used for label-expression translation, whose predicate form
    // differs per graph kind.
    using AnyGraphResolver = std::function<std::optional<bool>(const std::string &)>;

    explicit GqlToCypherTransformer(const std::string &query_p, GraphTypeRegistry *registry_p,
                                    AnyGraphResolver anyGraphResolver_p = nullptr)
        : query(query_p), registry(registry_p), anyGraphResolver(std::move(anyGraphResolver_p)) {}

    // Absolute source span in `query`, with the replacement text to use when
    // the span is rewritten (aggregate → alias etc.).
    struct Span {
        size_t start = 0;
        size_t stop = 0; // inclusive
        std::string replacement;
    };

    // Walk the GQL parse tree and return the equivalent Cypher query string.
    // Throws common::RuntimeException on unsupported GQL constructs.
    std::string Transform(GQLParser::GqlProgramContext &root);

    // Set when the statement is "CREATE [PROPERTY] GRAPH IF NOT EXISTS <name>".
    // LadybugDB Cypher has no IF NOT EXISTS for CREATE GRAPH, so the extension
    // enforces the GQL existence semantics at bind/rewrite time.
    bool sawIfNotExistsCreateGraph = false;
    std::string createGraphName;

    // Counter for auto-generated path variable names (`_gql_pp0`, ...) used by
    // whole-pattern path-mode filters. Reset per Transform call.
    int autoPathIdx = 0;
    // Counter for auto-generated node variable names (`_gql_nl0`, ...) used by
    // compound label expressions on anonymous nodes.
    int autoLabelIdx = 0;
    // Graph kind for label-expression translation (true = ANY graph, false =
    // typed/tabled graph), resolved at Transform entry. Nullopt = unresolvable
    // (compound label expressions are then rejected).
    std::optional<bool> labelGraphIsAny;
    AnyGraphResolver anyGraphResolver;

    [[noreturn]] static void unsupported(const std::string &feature);

    // (De)serialization of the graph-type registry for per-database storage.
    static std::string serializeGraphTypes(const GraphTypeRegistry &registry);
    static GraphTypeRegistry deserializeGraphTypes(const std::string &data);

    // GQL catalog statements produce empty results; LadybugDB DDL returns a
    // message row, so catalog translations end with this zero-row tail (TCK:
    // "Then the result should be empty").
    static constexpr const char *EMPTY_RESULT_CYPHER =
        "WITH 0 AS _gql_r WHERE false RETURN _gql_r";

private:
    struct SelectItemInfo {
        antlr4::ParserRuleContext *exprCtx = nullptr;
        std::string exprText; // expression source text (unmodified)
        std::string alias;    // user alias, empty when absent
        size_t exprStart = 0;
        size_t exprStop = 0;
        bool hasAggregate = false;
    };

    // ---------- top-level dispatch ----------
    std::string translateSessionActivity(GQLParser::SessionActivityContext *ctx);
    std::string translateTransactionActivity(GQLParser::TransactionActivityContext *ctx);
    std::string translateProcedureSpecification(GQLParser::ProcedureSpecificationContext *ctx);
    std::string translateStatementBlock(GQLParser::StatementBlockContext *ctx);
    std::string translateStatement(GQLParser::StatementContext *ctx);
    std::string translateCompositeQuery(GQLParser::CompositeQueryStatementContext *ctx);
    std::string translateLinearCatalog(GQLParser::LinearCatalogModifyingStatementContext *ctx);
    std::string translateLinearQuery(GQLParser::LinearQueryStatementContext *ctx);
    std::string translateLinearData(GQLParser::LinearDataModifyingStatementContext *ctx);

    // ---------- statement primitives ----------
    std::string translateQueryPrimitive(GQLParser::PrimitiveQueryStatementContext *ctx);
    std::string translateDataPrimitive(GQLParser::PrimitiveDataModifyingStatementContext *ctx);
    std::string translatePrimitiveResult(GQLParser::PrimitiveResultStatementContext *ctx);

    // ---------- query primitives ----------
    std::string translateSelectStatement(GQLParser::SelectStatementContext *ctx);
    std::string translateMatchStatement(GQLParser::MatchStatementContext *ctx);
    std::string translateMatchStatement(GQLParser::MatchStatementContext *ctx,
                                        std::vector<std::string> &wheres);
    std::string translateReturnStatement(GQLParser::ReturnStatementContext *ctx,
                                         GQLParser::OrderByAndPageStatementContext *page);
    std::string translateFilterStatement(GQLParser::FilterStatementContext *ctx);
    std::string translateForStatement(GQLParser::ForStatementContext *ctx);
    std::string translateOrderByAndPage(GQLParser::OrderByAndPageStatementContext *ctx);

    // ---------- graph patterns (QPPI / path modes / search prefixes) ----------
    // `wheres` collects predicates hoisted from inline element WHERE fillers and
    // any pattern-level WHERE; the caller merges them into the clause's WHERE.
    std::string translateGraphPattern(GQLParser::GraphPatternContext *ctx,
                                      std::vector<std::string> &wheres);
    std::string translatePathPattern(GQLParser::PathPatternContext *ctx,
                                     std::vector<std::string> &wheres);
    // `recType` is injected into each var-length slot ("", "TRAIL", "ACYCLIC",
    // "SHORTEST", "ALL SHORTEST"). Whole-pattern mode semantics are applied by
    // translatePathPattern via a path-variable wrap (IS_TRAIL / IS_ACYCLIC).
    std::string translatePathTerm(GQLParser::PathTermContext *ctx,
                                  std::vector<std::string> &wheres,
                                  const std::string &recType,
                                  int *edgeCountOut = nullptr);
    std::string translateEdgePattern(GQLParser::EdgePatternContext *ctx,
                                     const std::string &recDetail);
    std::string translateNodePattern(GQLParser::NodePatternContext *ctx,
                                     std::vector<std::string> &wheres);
    // Splits a filler into the bracket head (variable + :labels) and the
    // trailing property map; an inline WHERE is pushed onto `wheres`.
    // `isNodePattern` enables compound label-expression translation (edge
    // type expressions stay unsupported).
    void translateFiller(GQLParser::ElementPatternFillerContext *ctx,
                         std::vector<std::string> &wheres, std::string &head,
                         std::string &props, bool isNodePattern = true);
    // GQL label expression (ISO GQL feature G074) → Cypher boolean over the
    // bound variable's labels. Simple names stay as pattern labels; compound
    // expressions become WHERE predicates whose form depends on the graph
    // kind (ANY: list_contains(labels(v), ...); typed: labels(v) = ...).
    std::string translateLabelExpression(GQLParser::LabelExpressionContext *ctx,
                                         const std::string &var);
    // Resolve `labelGraphIsAny` from the statement's graph references
    // (FROM GRAPH / USE GRAPH / SESSION SET GRAPH; empty name = current).
    void resolveLabelGraphKind(GQLParser::GqlProgramContext *root);
    // Path mode / search prefix -> iC_RecursiveType text ("", "TRAIL",
    // "ACYCLIC", "SHORTEST", "ALL SHORTEST"). Throws on unsearchable forms.
    std::string translatePathPatternPrefix(GQLParser::PathPatternPrefixContext *ctx);

    // ---------- write primitives ----------
    std::string translateInsertStatement(GQLParser::InsertStatementContext *ctx);
    std::string translateSetStatement(GQLParser::SetStatementContext *ctx);
    std::string translateRemoveStatement(GQLParser::RemoveStatementContext *ctx);
    std::string translateDeleteStatement(GQLParser::DeleteStatementContext *ctx);

    // ---------- catalog / session ----------
    std::string translateCreateGraphStatement(GQLParser::CreateGraphStatementContext *ctx);
    std::string translateDropGraphStatement(GQLParser::DropGraphStatementContext *ctx);
    std::string translateCreateGraphTypeStatement(
        GQLParser::CreateGraphTypeStatementContext *ctx);
    std::string translateDropGraphTypeStatement(GQLParser::DropGraphTypeStatementContext *ctx);
    std::string translateSessionSetGraphClause(GQLParser::SessionSetGraphClauseContext *ctx);

    // ---------- helpers ----------
    std::string sourceText(antlr4::ParserRuleContext *ctx) const;
    std::string renderSelectItems(const std::vector<SelectItemInfo> &items,
                                  const std::vector<Span> &replacements) const;

    // ---------- Q2 comparison bridge (ANY graphs) ----------
    // On an open ANY graph a property comparison is a text-order comparison at
    // the engine — a silent wrong answer for JSON values. `emitValueExpression`
    // rewrites GQL `<` `<=` `>` `>=` to the extension's total-order predicates
    // _gql_lt/_gql_le/_gql_gt/_gql_ge (ANY x ANY -> BOOL), recursing through
    // every nested expression position. Typed graphs and unresolvable graph
    // kinds return the source text byte-for-byte (no behavior change).
    std::string emitValueExpression(GQLParser::ValueExpressionContext *ctx) const;
    // A single <comparisonExprAlt>: emit both operands recursively, then the
    // total-order call for the operator (equality stays textual until B2).
    std::string emitComparison(GQLParser::ComparisonExprAltContext *ctx) const;
    // Source text of `node` with every top-level comparison replaced by its
    // emitted form (right-to-left splices over absolute source offsets).
    std::string spliceComparisons(antlr4::tree::ParseTree *node) const;
    // The same bridge for any expression-bearing subtree (search conditions,
    // projection items, HAVING): every top-level comparison inside `node` is
    // spliced. Descending stops at aggregate boundaries so span-based aggregate
    // alias rewrites (replaceExprs over raw source text) keep matching.
    std::string emitExpr(antlr4::tree::ParseTree *node) const;
    // Renders an ORDER BY clause with `pairs` (projected expression -> output
    // alias) applied. On ANY graphs each mapped sort key is wrapped in
    // _gql_sortkey so JSON values sort under GQL's total order, not text order.
    std::string renderOrderBy(
        GQLParser::OrderByClauseContext *ctx,
        const std::vector<std::pair<std::string, std::string>> &pairs) const;

    // Reject GQL-only pattern features (quantified paths, path modes/search
    // prefixes, exotic label expressions) with a named error.
    // `allowLabelExpr` = label-expression operators are translated elsewhere
    // (filler label expressions) rather than rejected.
    void checkPatternSupported(antlr4::ParserRuleContext *ctx, bool allowLabelExpr = false);

    // Collect absolute spans of every aggregateFunction subtree under ctx.
    static void collectAggregates(antlr4::tree::ParseTree *node,
                                  std::vector<Span> &out, int &counter);
    static bool containsAggregate(antlr4::tree::ParseTree *node);

    // Literal-aware identifier rewrite (GQL function spellings → LadybugDB).
    static std::string mapIdentifiers(const std::string &text);
    // Literal-aware operator rewrite (`||` → `+`).
    static std::string mapOperators(const std::string &text);
    // Apply expression-level mappings to a source-text fragment.
    std::string finishExpr(const std::string &text) const;

    static std::string snippet(const std::string &text);
    static std::string replaceWord(const std::string &str, const std::string &from,
                                   const std::string &to);

    const std::string &query;
    GraphTypeRegistry *registry;
};

} // namespace gql_extension
} // namespace lbug
