#pragma once

#include "GQLParser.h"

// ANTLR exposes this implementation detail as a macro.
#ifdef INVALID_INDEX
#undef INVALID_INDEX
#endif

#include <map>
#include <string>
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
    explicit GqlToCypherTransformer(const std::string &query_p, GraphTypeRegistry *registry_p)
        : query(query_p), registry(registry_p) {}

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
    std::string translatePathTerm(GQLParser::PathTermContext *ctx,
                                  std::vector<std::string> &wheres,
                                  const std::string &recType);
    std::string translateEdgePattern(GQLParser::EdgePatternContext *ctx,
                                     const std::string &recDetail);
    std::string translateNodePattern(GQLParser::NodePatternContext *ctx,
                                     std::vector<std::string> &wheres);
    // Splits a filler into the bracket head (variable + :labels) and the
    // trailing property map; an inline WHERE is pushed onto `wheres`.
    void translateFiller(GQLParser::ElementPatternFillerContext *ctx,
                         std::vector<std::string> &wheres, std::string &head,
                         std::string &props);
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

    // Reject GQL-only pattern features (quantified paths, path modes/search
    // prefixes, exotic label expressions) with a named error.
    void checkPatternSupported(antlr4::ParserRuleContext *ctx);

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
