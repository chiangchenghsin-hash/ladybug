#include "gql_transformer.hpp"

#include "common/exception/runtime.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <utility>

namespace lbug {
namespace gql_extension {

// =============================================================================
// Errors
// =============================================================================

void GqlToCypherTransformer::unsupported(const std::string &feature) {
    throw common::RuntimeException{"GQL feature not supported: " + feature};
}

std::string GqlToCypherTransformer::snippet(const std::string &text) {
    std::string t;
    for (char c : text) {
        if (t.size() >= 48) {
            t += "...";
            break;
        }
        t += (c == '\n' || c == '\t') ? ' ' : c;
    }
    return t;
}

static bool iequals(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::toupper(static_cast<unsigned char>(a[i])) !=
            std::toupper(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// =============================================================================
// Graph-type registry (de)serialization for per-database storage
// =============================================================================

std::string GqlToCypherTransformer::serializeGraphTypes(const GraphTypeRegistry &registry) {
    constexpr char TAB = '\t';
    constexpr char NL = '\n';
    std::string out;
    for (const auto &[key, spec] : registry) {
        out += "T";
        out += TAB;
        out += key;
        out += NL;
        for (const auto &n : spec.nodes) {
            out += "N";
            out += TAB;
            out += n.name;
            for (const auto &p : n.props) {
                out += TAB;
                out += p.name + ":" + p.type;
            }
            out += NL;
        }
        for (const auto &e : spec.edges) {
            out += "E";
            out += TAB;
            out += e.name;
            out += TAB;
            out += e.from;
            out += TAB;
            out += e.to;
            for (const auto &p : e.props) {
                out += TAB;
                out += p.name + ":" + p.type;
            }
            out += NL;
        }
    }
    return out;
}

GraphTypeRegistry GqlToCypherTransformer::deserializeGraphTypes(const std::string &data) {
    GraphTypeRegistry registry;
    GraphTypeSpec *current = nullptr;
    std::istringstream in(data);
    std::string line;
    auto splitTabs = [](const std::string &text) {
        std::vector<std::string> parts;
        std::string cur;
        for (char c : text) {
            if (c == '	') {
                parts.push_back(cur);
                cur.clear();
            } else {
                cur += c;
            }
        }
        parts.push_back(cur);
        return parts;
    };
    while (std::getline(in, line)) {
        auto parts = splitTabs(line);
        if (parts.empty() || parts[0].empty()) continue;
        if (parts[0] == "T" && parts.size() >= 2) {
            current = &registry[parts[1]];
            *current = GraphTypeSpec{};
        } else if (parts[0] == "N" && current && parts.size() >= 2) {
            GraphTypeNode node;
            node.name = parts[1];
            for (size_t i = 2; i < parts.size(); i++) {
                auto colon = parts[i].find(':');
                if (colon == std::string::npos) continue;
                node.props.push_back({parts[i].substr(0, colon), parts[i].substr(colon + 1)});
            }
            current->nodes.push_back(std::move(node));
        } else if (parts[0] == "E" && current && parts.size() >= 4) {
            GraphTypeEdge edge;
            edge.name = parts[1];
            edge.from = parts[2];
            edge.to = parts[3];
            for (size_t i = 4; i < parts.size(); i++) {
                auto colon = parts[i].find(':');
                if (colon == std::string::npos) continue;
                edge.props.push_back({parts[i].substr(0, colon), parts[i].substr(colon + 1)});
            }
            current->edges.push_back(std::move(edge));
        }
    }
    return registry;
}

// =============================================================================
// Public entry point
// =============================================================================

std::string GqlToCypherTransformer::Transform(GQLParser::GqlProgramContext &root) {
    sawIfNotExistsCreateGraph = false;
    createGraphName.clear();

    if (root.sessionCloseCommand()) {
        unsupported("SESSION CLOSE");
    }
    auto *activity = root.programActivity();
    if (!activity) {
        unsupported("empty program");
    }
    if (auto *session = activity->sessionActivity()) {
        return translateSessionActivity(session);
    }
    if (auto *txn = activity->transactionActivity()) {
        return translateTransactionActivity(txn);
    }
    unsupported("program activity");
}

// =============================================================================
// Session / transaction
// =============================================================================

std::string GqlToCypherTransformer::translateSessionActivity(
    GQLParser::SessionActivityContext *ctx) {
    // GQL sessions allow repeated SESSION SET commands; LadybugDB has one
    // session graph switch (USE GRAPH) so only the single-command form maps.
    if (!ctx->sessionResetCommand().empty()) {
        unsupported("SESSION RESET");
    }
    if (ctx->sessionSetCommand().size() != 1) {
        unsupported("multiple SESSION SET commands");
    }
    auto *cmd = ctx->sessionSetCommand(0);
    if (auto *graph = cmd->sessionSetGraphClause()) {
        return translateSessionSetGraphClause(graph);
    }
    if (cmd->sessionSetSchemaClause()) {
        unsupported("SESSION SET SCHEMA");
    }
    if (cmd->sessionSetTimeZoneClause()) {
        unsupported("SESSION SET TIME ZONE");
    }
    unsupported("SESSION SET PARAMETER");
}

std::string GqlToCypherTransformer::translateTransactionActivity(
    GQLParser::TransactionActivityContext *ctx) {
    auto *start = ctx->startTransactionCommand();
    auto *body = ctx->procedureSpecification();
    auto *end = ctx->endTransactionCommand();

    if (start && body) {
        // A transaction-wrapped program would translate to
        // "BEGIN TRANSACTION; <body>; COMMIT" where only the COMMIT message is
        // visible to the caller (rewritten statements: last one wins). Reject
        // instead of silently returning the wrong result shape.
        unsupported("transaction-wrapped program in a single CALL GQL "
                    "(issue BEGIN/COMMIT as separate CALL GQL statements)");
    }
    std::string result;
    if (start) {
        // GQL: START TRANSACTION [READ ONLY | READ WRITE]
        // Cypher: BEGIN TRANSACTION [READ ONLY]
        std::string text = sourceText(start);
        text = replaceWord(text, "START TRANSACTION", "BEGIN TRANSACTION");
        text = replaceWord(text, "READ WRITE", "");
        text = std::regex_replace(text, std::regex("\\s+"), " ");
        while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
            text.pop_back();
        }
        if (text != "BEGIN TRANSACTION" && text != "BEGIN TRANSACTION READ ONLY") {
            unsupported("transaction characteristics (" + snippet(text) + ")");
        }
        result = text;
    }
    if (body) {
        result = translateProcedureSpecification(body);
    }
    if (end) {
        std::string text = sourceText(end); // COMMIT / ROLLBACK
        result = result.empty() ? text : result + "; " + text;
    }
    return result;
}

// =============================================================================
// Procedure body / statements
// =============================================================================

std::string GqlToCypherTransformer::translateProcedureSpecification(
    GQLParser::ProcedureSpecificationContext *ctx) {
    auto *body = ctx->procedureBody();
    if (!body) {
        unsupported("procedure specification");
    }
    if (body->atSchemaClause()) {
        unsupported("AT SCHEMA clause");
    }
    if (body->bindingVariableDefinitionBlock()) {
        unsupported("binding variable definitions (GRAPH/TABLE/VALUE ... = ...)");
    }
    return translateStatementBlock(body->statementBlock());
}

// statementBlock: statement nextStatement*  — NEXT composition is a pipeline
// over binding tables; Cypher multi-statements don't share bindings, so refuse
// rather than silently dropping scope.
std::string GqlToCypherTransformer::translateStatementBlock(
    GQLParser::StatementBlockContext *ctx) {
    if (!ctx) {
        unsupported("statement block");
    }
    if (!ctx->nextStatement().empty()) {
        unsupported("NEXT statement composition");
    }
    return translateStatement(ctx->statement());
}

std::string GqlToCypherTransformer::translateStatement(GQLParser::StatementContext *ctx) {
    if (!ctx) {
        unsupported("statement");
    }
    if (auto *composite = ctx->compositeQueryStatement()) {
        return translateCompositeQuery(composite);
    }
    if (auto *catalog = ctx->linearCatalogModifyingStatement()) {
        return translateLinearCatalog(catalog);
    }
    if (auto *data = ctx->linearDataModifyingStatement()) {
        return translateLinearData(data);
    }
    unsupported("statement");
}

std::string GqlToCypherTransformer::translateCompositeQuery(
    GQLParser::CompositeQueryStatementContext *ctx) {
    auto *expr = ctx->compositeQueryExpression();
    // Only the simple form (no UNION/EXCEPT/INTERSECT/OTHERWISE) is mapped.
    if (expr->compositeQueryExpression()) {
        unsupported("composite query (UNION/EXCEPT/INTERSECT)");
    }
    auto *primary = expr->compositeQueryPrimary();
    if (!primary) {
        unsupported("composite query");
    }
    return translateLinearQuery(primary->linearQueryStatement());
}

std::string GqlToCypherTransformer::translateLinearCatalog(
    GQLParser::LinearCatalogModifyingStatementContext *ctx) {
    auto simple = ctx->simpleCatalogModifyingStatement();
    if (simple.size() != 1) {
        unsupported("multiple catalog statements");
    }
    auto *prim = simple[0]->primitiveCatalogModifyingStatement();
    if (!prim) {
        unsupported("CALL catalog-modifying procedure");
    }
    if (auto *createGraph = prim->createGraphStatement()) {
        return translateCreateGraphStatement(createGraph);
    }
    if (auto *dropGraph = prim->dropGraphStatement()) {
        return translateDropGraphStatement(dropGraph);
    }
    if (prim->createSchemaStatement()) {
        unsupported("CREATE SCHEMA");
    }
    if (prim->dropSchemaStatement()) {
        unsupported("DROP SCHEMA");
    }
    if (auto *createGraphType = prim->createGraphTypeStatement()) {
        return translateCreateGraphTypeStatement(createGraphType);
    }
    if (auto *dropGraphType = prim->dropGraphTypeStatement()) {
        return translateDropGraphTypeStatement(dropGraphType);
    }
    unsupported("catalog statement");
}

// =============================================================================
// Linear query / data statements
// =============================================================================

namespace {

std::string joinClauses(const std::vector<std::string> &parts) {
    std::ostringstream out;
    bool first = true;
    for (auto &p : parts) {
        if (p.empty()) continue;
        if (!first) out << ' ';
        out << p;
        first = false;
    }
    return out.str();
}

// Comma-separated list rendering (RETURN items, WITH items, SET items, ...).
std::string joinCommas(const std::vector<std::string> &parts) {
    std::ostringstream out;
    bool first = true;
    for (auto &p : parts) {
        if (p.empty()) continue;
        if (!first) out << ", ";
        out << p;
        first = false;
    }
    return out.str();
}

// Replaces whole-expression occurrences of pair.first with pair.second in
// source text. Literal-aware (string/quoted-identifier contents untouched),
// boundary-aware (`n.age` does not match inside `n.aged` or `m.n.age`), and
// longest-pattern-first. Used for rewriting ORDER BY / HAVING / RETURN
// fragments that reference *new* occurrences of select-item expressions, where
// absolute span math does not apply.
std::string replaceExprs(const std::string &text,
                         std::vector<std::pair<std::string, std::string>> pairs) {
    std::sort(pairs.begin(), pairs.end(),
              [](const auto &a, const auto &b) { return a.first.size() > b.first.size(); });
    std::string out;
    out.reserve(text.size());
    auto isWordChar = [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.';
    };
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '\'' || c == '"' || c == '`') {
            char quote = c;
            out += c;
            ++i;
            while (i < text.size()) {
                out += text[i];
                if (text[i] == quote) {
                    if (i + 1 < text.size() && text[i + 1] == quote) {
                        out += text[i + 1];
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        bool matched = false;
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            for (auto &p : pairs) {
                const std::string &from = p.first;
                if (from.empty() || i + from.size() > text.size()) continue;
                if (text.compare(i, from.size(), from) != 0) continue;
                bool beforeOk = (i == 0) || !isWordChar(text[i - 1]);
                size_t after = i + from.size();
                bool afterOk = (after >= text.size()) || !isWordChar(text[after]);
                if (beforeOk && afterOk) {
                    out += p.second;
                    i = after;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += c;
            ++i;
        }
    }
    return out;
}

} // namespace

std::string GqlToCypherTransformer::translateLinearQuery(
    GQLParser::LinearQueryStatementContext *ctx) {
    if (auto *ambient = ctx->ambientLinearQueryStatement()) {
        if (ambient->nestedQuerySpecification()) {
            unsupported("nested query specification");
        }
        std::vector<std::string> parts;
        if (auto *simple = ambient->simpleLinearQueryStatement()) {
            for (auto *qs : simple->simpleQueryStatement()) {
                auto *prim = qs->primitiveQueryStatement();
                if (!prim) {
                    unsupported("CALL query procedure");
                }
                parts.push_back(translateQueryPrimitive(prim));
            }
        }
        parts.push_back(translatePrimitiveResult(ambient->primitiveResultStatement()));
        return joinClauses(parts);
    }
    if (auto *focused = ctx->focusedLinearQueryStatement()) {
        if (focused->selectStatement()) {
            return translateSelectStatement(focused->selectStatement());
        }
        if (focused->focusedNestedQuerySpecification()) {
            unsupported("nested query specification");
        }
        std::vector<std::string> parts;
        for (auto *part : focused->focusedLinearQueryStatementPart()) {
            parts.push_back("USE GRAPH " + sourceText(part->useGraphClause()->graphExpression()) +
                            ";");
            for (auto *qs : part->simpleLinearQueryStatement()->simpleQueryStatement()) {
                auto *prim = qs->primitiveQueryStatement();
                if (!prim) {
                    unsupported("CALL query procedure");
                }
                parts.push_back(translateQueryPrimitive(prim));
            }
        }
        if (auto *tail = focused->focusedLinearQueryAndPrimitiveResultStatementPart()) {
            parts.push_back("USE GRAPH " +
                            sourceText(tail->useGraphClause()->graphExpression()) + ";");
            for (auto *qs : tail->simpleLinearQueryStatement()->simpleQueryStatement()) {
                auto *prim = qs->primitiveQueryStatement();
                if (!prim) {
                    unsupported("CALL query procedure");
                }
                parts.push_back(translateQueryPrimitive(prim));
            }
            parts.push_back(translatePrimitiveResult(tail->primitiveResultStatement()));
        } else if (auto *resultOnly = focused->focusedPrimitiveResultStatement()) {
            parts.push_back("USE GRAPH " +
                            sourceText(resultOnly->useGraphClause()->graphExpression()) + ";");
            parts.push_back(translatePrimitiveResult(resultOnly->primitiveResultStatement()));
        } else {
            unsupported("focused linear query");
        }
        return joinClauses(parts);
    }
    unsupported("linear query");
}

std::string GqlToCypherTransformer::translateLinearData(
    GQLParser::LinearDataModifyingStatementContext *ctx) {
    std::string prefix;
    GQLParser::SimpleLinearDataAccessingStatementContext *accessing = nullptr;
    GQLParser::PrimitiveResultStatementContext *result = nullptr;

    if (auto *ambient = ctx->ambientLinearDataModifyingStatement()) {
        if (ambient->nestedDataModifyingProcedureSpecification()) {
            unsupported("nested data-modifying procedure specification");
        }
        auto *body = ambient->ambientLinearDataModifyingStatementBody();
        accessing = body->simpleLinearDataAccessingStatement();
        result = body->primitiveResultStatement();
    } else if (auto *focused = ctx->focusedLinearDataModifyingStatement()) {
        if (focused->focusedNestedDataModifyingProcedureSpecification()) {
            unsupported("nested data-modifying procedure specification");
        }
        auto *body = focused->focusedLinearDataModifyingStatementBody();
        prefix = "USE GRAPH " + sourceText(body->useGraphClause()->graphExpression()) + ";";
        accessing = body->simpleLinearDataAccessingStatement();
        result = body->primitiveResultStatement();
    } else {
        unsupported("linear data-modifying statement");
    }

    std::vector<std::string> parts;
    for (auto *s : accessing->simpleDataAccessingStatement()) {
        if (auto *qs = s->simpleQueryStatement()) {
            auto *prim = qs->primitiveQueryStatement();
            if (!prim) {
                unsupported("CALL query procedure");
            }
            parts.push_back(translateQueryPrimitive(prim));
        } else if (auto *ms = s->simpleDataModifyingStatement()) {
            auto *prim = ms->primitiveDataModifyingStatement();
            if (!prim) {
                unsupported("CALL data-modifying procedure");
            }
            parts.push_back(translateDataPrimitive(prim));
        } else {
            unsupported("data-accessing statement");
        }
    }
    if (result) {
        parts.push_back(translatePrimitiveResult(result));
    }
    std::string body = joinClauses(parts);
    return prefix.empty() ? body : prefix + " " + body;
}

std::string GqlToCypherTransformer::translateQueryPrimitive(
    GQLParser::PrimitiveQueryStatementContext *ctx) {
    if (auto *match = ctx->matchStatement()) {
        return translateMatchStatement(match);
    }
    if (auto *filter = ctx->filterStatement()) {
        return translateFilterStatement(filter);
    }
    if (auto *page = ctx->orderByAndPageStatement()) {
        return translateOrderByAndPage(page);
    }
    if (auto *forStmt = ctx->forStatement()) {
        return translateForStatement(forStmt);
    }
    if (ctx->letStatement()) {
        unsupported("LET statement");
    }
    unsupported("query primitive");
}

std::string GqlToCypherTransformer::translateDataPrimitive(
    GQLParser::PrimitiveDataModifyingStatementContext *ctx) {
    if (auto *insert = ctx->insertStatement()) {
        return translateInsertStatement(insert);
    }
    if (auto *set = ctx->setStatement()) {
        return translateSetStatement(set);
    }
    if (auto *remove = ctx->removeStatement()) {
        return translateRemoveStatement(remove);
    }
    if (auto *del = ctx->deleteStatement()) {
        return translateDeleteStatement(del);
    }
    unsupported("data-modifying primitive");
}

std::string GqlToCypherTransformer::translatePrimitiveResult(
    GQLParser::PrimitiveResultStatementContext *ctx) {
    if (!ctx) {
        unsupported("missing result statement");
    }
    if (ctx->FINISH()) {
        unsupported("FINISH");
    }
    return translateReturnStatement(ctx->returnStatement(), ctx->orderByAndPageStatement());
}

// =============================================================================
// Query primitives
// =============================================================================

std::string GqlToCypherTransformer::translateMatchStatement(
    GQLParser::MatchStatementContext *ctx) {
    std::vector<std::string> wheres;
    std::string out = translateMatchStatement(ctx, wheres);
    for (size_t i = 0; i < wheres.size(); ++i) {
        out += (i ? " AND " : " WHERE ") + wheres[i];
    }
    return out;
}

std::string GqlToCypherTransformer::translateMatchStatement(
    GQLParser::MatchStatementContext *ctx, std::vector<std::string> &wheres) {
    std::string keyword = "MATCH ";
    GQLParser::SimpleMatchStatementContext *simple = ctx->simpleMatchStatement();
    if (!simple) {
        auto *operand = ctx->optionalMatchStatement()->optionalOperand();
        if (!operand->simpleMatchStatement()) {
            unsupported("OPTIONAL MATCH block form");
        }
        keyword = "OPTIONAL MATCH ";
        simple = operand->simpleMatchStatement();
    }
    auto *bindingTable = simple->graphPatternBindingTable();
    if (bindingTable->graphPatternYieldClause()) {
        unsupported("MATCH ... YIELD");
    }
    return keyword + translateGraphPattern(bindingTable->graphPattern(), wheres);
}

std::string GqlToCypherTransformer::translateFilterStatement(
    GQLParser::FilterStatementContext *ctx) {
    GQLParser::SearchConditionContext *cond = ctx->searchCondition();
    if (!cond && ctx->whereClause()) {
        cond = ctx->whereClause()->searchCondition();
    }
    // GQL FILTER == Cypher post-filter: "WITH * WHERE ..." keeps every binding
    // (a bare WHERE only parses as part of MATCH/WITH and broke FILTER after
    // FOR/UNWIND).
    return "WITH * WHERE " + finishExpr(sourceText(cond));
}

std::string GqlToCypherTransformer::translateForStatement(
    GQLParser::ForStatementContext *ctx) {
    if (ctx->forOrdinalityOrOffset()) {
        unsupported("FOR ... WITH ORDINALITY/OFFSET");
    }
    auto *item = ctx->forItem();
    std::string var = sourceText(item->forItemAlias()->bindingVariable());
    std::string src = finishExpr(sourceText(item->forItemSource()->valueExpression()));
    // GQL FOR x IN expr == Cypher UNWIND expr AS x
    return "UNWIND " + src + " AS " + var;
}

std::string GqlToCypherTransformer::translateOrderByAndPage(
    GQLParser::OrderByAndPageStatementContext *ctx) {
    std::string text = finishExpr(sourceText(ctx));
    // LadybugDB Cypher spells paging SKIP/LIMIT; GQL allows OFFSET as a synonym.
    text = replaceWord(text, "OFFSET", "SKIP");
    return text;
}

// =============================================================================
// Graph patterns (Phase 3): GQL path patterns → LadybugDB Cypher patterns.
//
// Quantified single-edge patterns (edge `{m,n}` / single-hop QPPI) map onto the
// engine's recursive relationship form `[e*<TYPE> <range>]` (iC_RecursiveDetail
// in Cypher.g4). Path mode / search prefixes map onto iC_RecursiveType.
// Quantifier bounds follow Neo4j's QuantifiedPathPattern semantics
// (AddElementUniquenessPredicates.getLowerBound/getUpperBound, Apache-2.0):
//   * → 0..∞,  + → 1..∞,  {n} → n..n,  {m,n} → m..n,  {m,} → m..∞,  {,n} → 0..n,
//   ? → 0..1.
// Verified engine facts (see path.test): lower bound 0 is supported (0-hop rows
// bind start=end with an empty edge list), bare `*` in Cypher means 1..∞ so GQL
// `*` must emit an explicit `0..`, and MATCH allows edge repetition (≈ GQL
// REPEATABLE ELEMENTS / WALK).
// =============================================================================

namespace {

// GQL quantifier → Cypher oC_RangeLiteral text (placed after '*').
std::string quantifierRange(GQLParser::GraphPatternQuantifierContext *q) {
    if (q->ASTERISK()) return "0..";
    if (q->PLUS_SIGN()) return "1..";
    if (auto *fixed = q->fixedQuantifier()) {
        return fixed->unsignedInteger()->getText();
    }
    auto *gen = q->generalQuantifier();
    std::string lo = gen->lowerBound() ? gen->lowerBound()->getText() : "0";
    if (!gen->upperBound()) {
        return lo + "..";
    }
    return lo + ".." + gen->upperBound()->getText();
}

enum class EdgeDir { Left, Right, Both };

struct EdgeShape {
    EdgeDir dir = EdgeDir::Both;
    GQLParser::ElementPatternFillerContext *filler = nullptr;
};

EdgeShape edgeShape(GQLParser::EdgePatternContext *e) {
    EdgeShape s;
    if (auto *full = e->fullEdgePattern()) {
        if (auto *x = full->fullEdgePointingLeft()) {
            s.dir = EdgeDir::Left;
            s.filler = x->elementPatternFiller();
        } else if (auto *x = full->fullEdgePointingRight()) {
            s.dir = EdgeDir::Right;
            s.filler = x->elementPatternFiller();
        } else if (auto *x = full->fullEdgeUndirected()) {
            s.filler = x->elementPatternFiller();
        } else if (auto *x = full->fullEdgeLeftOrUndirected()) {
            s.filler = x->elementPatternFiller();
        } else if (auto *x = full->fullEdgeUndirectedOrRight()) {
            s.filler = x->elementPatternFiller();
        } else if (auto *x = full->fullEdgeLeftOrRight()) {
            s.filler = x->elementPatternFiller();
        } else if (auto *x = full->fullEdgeAnyDirection()) {
            s.filler = x->elementPatternFiller();
        }
        return s;
    }
    // Abbreviated forms carry no filler. GQL's undirected / mixed-direction
    // spellings collapse to LadybugDB's ANY-direction `--` (a directed property
    // graph has no undirected edges to distinguish).
    auto *ab = e->abbreviatedEdgePattern();
    if (ab->LEFT_ARROW()) {
        s.dir = EdgeDir::Left;
    }
    return s;
}

// A pattern factor ready to be flattened into node/edge events.
struct FlatEvent {
    bool isEdge = false;
    std::string nodeBinding;                   // for node events (may be "")
    GQLParser::EdgePatternContext *edge = nullptr; // for edge events
    std::string range;                         // edge: "" = fixed single hop
    std::string innerStart, innerEnd;          // paren unit end nodes ("" = none)
};

} // namespace

std::string GqlToCypherTransformer::translatePathPatternPrefix(
    GQLParser::PathPatternPrefixContext *ctx) {
    if (auto *mode = ctx->pathModePrefix()) {
        auto *m = mode->pathMode();
        if (m->SIMPLE()) {
            unsupported("SIMPLE path mode (LadybugDB has WALK/TRAIL/ACYCLIC only)");
        }
        // WALK is LadybugDB's default recursive semantic: emit no type.
        if (m->TRAIL()) return "TRAIL";
        if (m->ACYCLIC()) return "ACYCLIC";
        return "";
    }
    auto *search = ctx->pathSearchPrefix();
    if (auto *all = search->allPathSearch()) {
        (void)all;
        unsupported("ALL PATHS search prefix");
    }
    if (auto *any = search->anyPathSearch()) {
        if (any->numberOfPaths()) {
            unsupported("ANY <k> PATHS search prefix");
        }
        unsupported("ANY PATHS search prefix");
    }
    auto *sh = search->shortestPathSearch();
    if (sh->countedShortestPathSearch() || sh->countedShortestGroupSearch()) {
        unsupported("SHORTEST <k>/GROUP(S) search prefix");
    }
    // ANY SHORTEST -> one shortest path per binding (= Cypher SHORTEST);
    // ALL SHORTEST -> all shortest paths. An accompanying path mode is
    // redundant: any shortest path is automatically a trail and acyclic.
    return sh->allShortestPathSearch() ? "ALL SHORTEST" : "SHORTEST";
}

void GqlToCypherTransformer::translateFiller(
    GQLParser::ElementPatternFillerContext *ctx, std::vector<std::string> &wheres,
    std::string &head, std::string &props) {
    head.clear();
    props.clear();
    if (!ctx) {
        return;
    }
    checkPatternSupported(ctx); // label-expression operators etc.
    if (ctx->elementVariableDeclaration()) {
        head += sourceText(ctx->elementVariableDeclaration());
    }
    if (auto *lab = ctx->isLabelExpression()) {
        // Simple labels only (operators rejected above). Normalise both the
        // `:Label` and `IS Label` spellings to Cypher's `:Label`.
        head += ":" + sourceText(lab->labelExpression());
    }
    if (auto *pred = ctx->elementPatternPredicate()) {
        if (auto *p = pred->elementPropertySpecification()) {
            props += sourceText(p);
        } else if (auto *w = pred->elementPatternWhereClause()) {
            checkPatternSupported(w->searchCondition());
            wheres.push_back(finishExpr(sourceText(w->searchCondition())));
        }
    }
}

std::string GqlToCypherTransformer::translateNodePattern(
    GQLParser::NodePatternContext *ctx, std::vector<std::string> &wheres) {
    std::string head, props;
    translateFiller(ctx->elementPatternFiller(), wheres, head, props);
    return "(" + head + props + ")";
}

std::string GqlToCypherTransformer::translateEdgePattern(
    GQLParser::EdgePatternContext *ctx, const std::string &recDetail) {
    EdgeShape shape = edgeShape(ctx);
    std::string head, props;
    if (shape.filler) {
        std::vector<std::string> unused;
        translateFiller(shape.filler, unused, head, props);
        // An inline WHERE on an edge filler cannot be hoisted from here (this
        // helper has no where sink); reject rather than drop it.
        if (shape.filler->elementPatternPredicate() &&
            shape.filler->elementPatternPredicate()->elementPatternWhereClause()) {
            unsupported("inline WHERE on an edge pattern");
        }
    }
    std::string left, right;
    switch (shape.dir) {
    case EdgeDir::Left:
        left = "<-[";
        right = "]-";
        break;
    case EdgeDir::Right:
        left = "-[";
        right = "]->";
        break;
    case EdgeDir::Both:
        left = "-[";
        right = "]-";
        break;
    }
    if (recDetail.empty()) {
        // Plain fixed-hop edge. Keep abbreviated forms abbreviated.
        if (!shape.filler && ctx->abbreviatedEdgePattern()) {
            switch (shape.dir) {
            case EdgeDir::Left:
                return "<--";
            case EdgeDir::Right:
                return "-->";
            default:
                return "--";
            }
        }
        return left + head + props + right;
    }
    // Recursive (var-length) form: iC_RecursiveDetail sits between the type
    // and the property map — [e:Knows*TRAIL 1..3 {since: 2020}].
    return left + head + recDetail + props + right;
}

std::string GqlToCypherTransformer::translatePathPattern(
    GQLParser::PathPatternContext *ctx, std::vector<std::string> &wheres) {
    std::string recType;
    if (auto *pre = ctx->pathPatternPrefix()) {
        recType = translatePathPatternPrefix(pre);
    }
    auto *expr = ctx->pathPatternExpression();
    auto *term = dynamic_cast<GQLParser::PpePathTermContext *>(expr);
    if (!term) {
        unsupported("path pattern union/multiset alternation");
    }
    std::string body = translatePathTerm(term->pathTerm(), wheres, recType);
    if (ctx->pathVariableDeclaration()) {
        return sourceText(ctx->pathVariableDeclaration()) + " " + body;
    }
    return body;
}

std::string GqlToCypherTransformer::translatePathTerm(
    GQLParser::PathTermContext *ctx, std::vector<std::string> &wheres,
    const std::string &recType) {
    // Flatten factors into node-binding / edge events. Juxtaposition in GQL
    // identifies the end of one factor with the start of the next, so adjacent
    // node bindings merge into one node and an edge always sits between two
    // node slots (implicit `()` when no neighbour provides one).
    std::vector<FlatEvent> events;

    std::function<void(GQLParser::PathTermContext *, bool)> flatten;
    flatten = [&](GQLParser::PathTermContext *termCtx, bool quantifiedOuter) {
        auto factors = termCtx->pathFactor();
        for (size_t i = 0; i < factors.size(); ++i) {
            auto *f = factors[i];
            GQLParser::PathPrimaryContext *primary = nullptr;
            std::string range;
            bool quantified = false;
            if (auto *p = dynamic_cast<GQLParser::PfQuantifiedPathPrimaryContext *>(f)) {
                primary = p->pathPrimary();
                range = quantifierRange(p->graphPatternQuantifier());
                quantified = true;
            } else if (auto *p =
                           dynamic_cast<GQLParser::PfQuestionedPathPrimaryContext *>(f)) {
                primary = p->pathPrimary();
                range = "0..1";
                quantified = true;
            } else {
                primary = dynamic_cast<GQLParser::PfPathPrimaryContext *>(f)->pathPrimary();
            }
            if (auto *el = dynamic_cast<GQLParser::PpElementPatternContext *>(primary)) {
                if (el->elementPattern()->nodePattern()) {
                    if (quantified) {
                        unsupported("quantified path pattern over a node pattern");
                    }
                    FlatEvent ev;
                    std::string head, props;
                    translateFiller(el->elementPattern()->nodePattern()->elementPatternFiller(),
                                    wheres, head, props);
                    ev.nodeBinding = "(" + head + props + ")";
                    events.push_back(std::move(ev));
                } else {
                    FlatEvent ev;
                    ev.isEdge = true;
                    ev.edge = el->elementPattern()->edgePattern();
                    ev.range = range;
                    if (quantifiedOuter && quantified) {
                        unsupported("nested quantified path pattern");
                    }
                    events.push_back(std::move(ev));
                }
                continue;
            }
            if (auto *paren =
                    dynamic_cast<GQLParser::PpParenthesizedPathPatternExpressionContext *>(
                        primary)) {
                auto *ppe = paren->parenthesizedPathPatternExpression();
                if (ppe->subpathVariableDeclaration()) {
                    unsupported("subpath variable in parenthesized path pattern");
                }
                if (ppe->pathModePrefix()) {
                    unsupported("path mode inside parenthesized path pattern");
                }
                if (ppe->parenthesizedPathPatternWhereClause()) {
                    unsupported("WHERE inside parenthesized path pattern");
                }
                size_t before = events.size();
                auto *innerExpr = dynamic_cast<GQLParser::PpePathTermContext *>(
                    ppe->pathPatternExpression());
                if (!innerExpr) {
                    unsupported("path pattern union/multiset alternation");
                }
                flatten(innerExpr->pathTerm(), quantified);
                if (quantified) {
                    // QPPI: the interior must be exactly one edge between two
                    // empty anonymous nodes — interior bindings would be lists
                    // in GQL and have no Cypher var-length counterpart.
                    std::vector<FlatEvent> inner(events.begin() + before, events.end());
                    events.resize(before);
                    int innerEdges = 0;
                    FlatEvent edge;
                    for (auto &e : inner) {
                        if (e.isEdge) {
                            innerEdges++;
                            edge = e;
                        } else if (e.nodeBinding != "()") {
                            unsupported(
                                "quantified path pattern with interior node bindings");
                        }
                    }
                    if (innerEdges != 1) {
                        unsupported("quantified path pattern with multiple edges");
                    }
                    if (!edge.range.empty()) {
                        unsupported("nested quantified path pattern");
                    }
                    edge.range = range; // outer quantifier supplies the bounds
                    FlatEvent startNode;
                    startNode.nodeBinding = "()";
                    FlatEvent endNode;
                    endNode.nodeBinding = "()";
                    events.push_back(startNode);
                    events.push_back(edge);
                    events.push_back(endNode);
                }
                continue;
            }
            unsupported("simplified path pattern (-/.../-)");
        }
    };

    flatten(ctx, false);

    // Build the chain: consecutive node bindings merge (juxtaposition); each
    // edge is emitted between two node slots.
    std::string out;
    std::string pendingNode;
    bool haveNode = false;
    int edgeCount = 0;
    FlatEvent *soleEdge = nullptr;

    auto bindNode = [&](const std::string &b) {
        std::string text = b.empty() ? "()" : b;
        if (!haveNode) {
            pendingNode = text;
            haveNode = true;
            return;
        }
        // Merge with the pending boundary node.
        if (pendingNode == "()") {
            pendingNode = text;
        } else if (text != "()" && text != pendingNode) {
            unsupported("juxtaposed node patterns (distinct variables)");
        }
    };
    for (auto &ev : events) {
        if (!ev.isEdge) {
            bindNode(ev.nodeBinding);
            continue;
        }
        if (!haveNode) {
            bindNode("");
        }
        std::string recDetail;
        if (!ev.range.empty()) {
            recDetail = "*" + ev.range;
        }
        out += pendingNode + translateEdgePattern(ev.edge, recDetail);
        haveNode = false;
        edgeCount++;
        soleEdge = &ev;
    }
    if (haveNode) {
        out += pendingNode;
    }

    // Path mode / search prefix maps onto a recursive type — only a single-edge
    // pattern has one. Fixed single hops drop a no-op prefix silently.
    if (!recType.empty() && edgeCount > 1) {
        unsupported("path mode/search prefix on multi-hop pattern");
    }
    if (edgeCount == 1 && !recType.empty()) {
        FlatEvent &ev = *soleEdge;
        if (ev.range.empty()) {
            // Fixed 1-hop: every mode/search is a no-op on a single edge.
            return out;
        }
        if ((recType == "SHORTEST" || recType == "ALL SHORTEST") &&
            (ev.range.rfind("0", 0) == 0)) {
            unsupported("shortest path with lower bound 0 (empty paths)");
        }
        // Re-emit the single edge with the recursive type injected.
        std::string recDetail = "*" + recType + " " + ev.range;
        std::string head, props;
        EdgeShape shape = edgeShape(ev.edge);
        if (shape.filler) {
            std::vector<std::string> unused;
            translateFiller(shape.filler, unused, head, props);
        }
        std::string left = "-[", right = "]-";
        if (shape.dir == EdgeDir::Left) {
            left = "<-[";
        } else if (shape.dir == EdgeDir::Right) {
            right = "]->";
        }
        // Rebuild the chain with the recType-bearing edge (out has exactly
        // one edge between two node slots).
        std::string rebuilt;
        // Re-run the chain walk with a modified emitter.
        pendingNode.clear();
        haveNode = false;
        for (auto &e2 : events) {
            if (!e2.isEdge) {
                std::string text = e2.nodeBinding.empty() ? "()" : e2.nodeBinding;
                if (!haveNode) {
                    pendingNode = text;
                    haveNode = true;
                } else if (pendingNode == "()") {
                    pendingNode = text;
                }
                continue;
            }
            if (!haveNode) {
                pendingNode = "()";
                haveNode = true;
            }
            rebuilt += pendingNode + left + head + recDetail + props + right;
            haveNode = false;
        }
        if (haveNode) {
            rebuilt += pendingNode;
        }
        return rebuilt;
    }
    return out;
}

std::string GqlToCypherTransformer::translateGraphPattern(
    GQLParser::GraphPatternContext *ctx, std::vector<std::string> &wheres) {
    if (auto *mm = ctx->matchMode()) {
        // LadybugDB MATCH already allows edge repetition (≈ GQL's REPEATABLE
        // ELEMENTS); DIFFERENT EDGES has no engine enforcement.
        if (mm->differentEdgesMatchMode()) {
            unsupported("DIFFERENT EDGES match mode");
        }
    }
    if (ctx->keepClause()) {
        unsupported("KEEP clause");
    }
    std::vector<std::string> parts;
    for (auto *p : ctx->pathPatternList()->pathPattern()) {
        parts.push_back(translatePathPattern(p, wheres));
    }
    std::string out = joinCommas(parts);
    if (auto *w = ctx->graphPatternWhereClause()) {
        checkPatternSupported(w->searchCondition());
        wheres.push_back(finishExpr(sourceText(w->searchCondition())));
    }
    return out;
}

// =============================================================================
// SELECT statement (GQL query root) → MATCH ... [WITH ...] RETURN ...
// =============================================================================

std::string GqlToCypherTransformer::translateSelectStatement(
    GQLParser::SelectStatementContext *ctx) {
    bool distinct = false;
    if (auto *sq = ctx->setQuantifier()) {
        if (sq->DISTINCT()) {
            distinct = true;
        }
    }

    const bool star = ctx->ASTERISK() != nullptr;
    std::vector<SelectItemInfo> items;
    if (!star) {
        auto *list = ctx->selectItemList();
        if (!list) {
            unsupported("SELECT without projection");
        }
        for (auto *item : list->selectItem()) {
            auto *expr = item->aggregatingValueExpression();
            SelectItemInfo info;
            info.exprCtx = expr;
            info.exprStart = expr->getStart()->getStartIndex();
            info.exprStop = expr->getStop()->getStopIndex();
            info.exprText = query.substr(info.exprStart, info.exprStop - info.exprStart + 1);
            if (item->selectItemAlias()) {
                info.alias = sourceText(item->selectItemAlias()->identifier());
            }
            info.hasAggregate = containsAggregate(expr);
            items.push_back(std::move(info));
        }
    }

    // ---- FROM GRAPH ... MATCH ... ----
    std::string prefix;
    std::string matchText;
    std::vector<std::string> wheres;
    if (auto *body = ctx->selectStatementBody()) {
        if (body->selectQuerySpecification()) {
            unsupported("SELECT ... FROM <nested query>");
        }
        auto *matches = body->selectGraphMatchList();
        if (!matches || matches->selectGraphMatch().size() != 1) {
            unsupported("multiple FROM GRAPH MATCH clauses");
        }
        auto *graphMatch = matches->selectGraphMatch(0);
        std::string graphText = sourceText(graphMatch->graphExpression());
        if (!iequals(graphText, "CURRENT_GRAPH") &&
            !iequals(graphText, "CURRENT_PROPERTY_GRAPH")) {
            // Labels resolve against the session graph, so a named FROM GRAPH
            // must be routed through USE GRAPH (session-sticky, documented).
            prefix = "USE GRAPH " + finishExpr(graphText) + "; ";
        }
        matchText = translateMatchStatement(graphMatch->matchStatement(), wheres);
    }

    // ---- WHERE (match-level predicates, hoisted fillers and SELECT WHERE
    //      merge into one clause) ----
    if (auto *w = ctx->whereClause()) {
        wheres.push_back(finishExpr(sourceText(w->searchCondition())));
    }
    std::string whereText;
    for (size_t i = 0; i < wheres.size(); ++i) {
        whereText += (i ? " AND " : " WHERE ") + wheres[i];
    }

    // ---- GROUP BY ----
    std::vector<std::string> explicitKeys;
    if (auto *gb = ctx->groupByClause()) {
        auto *list = gb->groupingElementList();
        if (list->emptyGroupingSet()) {
            unsupported("GROUP BY ()");
        }
        for (auto *key : list->groupingElement()) {
            // ISO GQL: <grouping element> ::= <binding variable reference>
            explicitKeys.push_back(sourceText(key->bindingVariableReference()));
        }
    }

    bool hasAgg = star ? false
                       : std::any_of(items.begin(), items.end(),
                                     [](const SelectItemInfo &i) { return i.hasAggregate; });
    std::string havingRaw;
    if (auto *h = ctx->havingClause()) {
        havingRaw = sourceText(h->searchCondition());
        hasAgg = true;
    }

    // ---- ORDER BY / OFFSET / LIMIT (page suffix; rewritten after grouping) ----
    auto buildOrderPage = [&](const std::vector<std::pair<std::string, std::string>> &pairs) {
        std::ostringstream page;
        if (auto *ob = ctx->orderByClause()) {
            page << ' ' << finishExpr(replaceExprs(sourceText(ob), pairs));
        }
        if (auto *off = ctx->offsetClause()) {
            page << " SKIP " << finishExpr(sourceText(off->nonNegativeIntegerSpecification()));
        }
        if (auto *lim = ctx->limitClause()) {
            page << " LIMIT " << finishExpr(sourceText(lim->nonNegativeIntegerSpecification()));
        }
        return page.str();
    };

    // (exprText -> output name) pairs. ORDER BY / HAVING reference *new*
    // occurrences of the item expressions, so rewrites are textual.
    auto itemPairs = [&]() {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (auto &item : items) {
            if (!item.alias.empty()) {
                pairs.emplace_back(item.exprText, item.alias);
            }
        }
        return pairs;
    };

    if (distinct && hasAgg) {
        unsupported("SELECT DISTINCT with aggregation");
    }
    if (!hasAgg && !explicitKeys.empty()) {
        unsupported("GROUP BY without aggregation (use SELECT DISTINCT)");
    }

    // No aggregation: plain MATCH ... RETURN. DISTINCT narrows ORDER BY to
    // projected names (same binder rule as aggregation), so rewrite either way.
    if (!hasAgg) {
        std::string proj = star ? "*" : renderSelectItems(items, {});
        return prefix + matchText + whereText + " RETURN " + (distinct ? "DISTINCT " : "") +
               finishExpr(proj) + buildOrderPage(itemPairs());
    }

    // Implicit grouping with no HAVING: Cypher's RETURN groups by the
    // non-aggregated items exactly like GQL's implicit grouping; ORDER BY can
    // only reference projected names after aggregation.
    if (explicitKeys.empty() && havingRaw.empty()) {
        std::string proj = star ? "*" : renderSelectItems(items, {});
        return prefix + matchText + whereText + " RETURN " + finishExpr(proj) +
               buildOrderPage(itemPairs());
    }

    // General grouped form: WITH keys, aggs [WHERE having] RETURN items.
    if (star) {
        unsupported("SELECT * with GROUP BY/HAVING");
    }

    // Assign an alias to every distinct aggregate expression text (select
    // items first, then HAVING). Identical texts share one alias and one WITH
    // projection -- an aggregate appearing in both SELECT and HAVING must not
    // be projected twice.
    std::vector<std::pair<std::string, std::string>> aggPairs; // aggText -> alias
    auto registerAgg = [&](const std::string &text, const std::string &userAlias) {
        for (auto &p : aggPairs) {
            if (p.first == text) return;
        }
        std::string alias = userAlias.empty() ? "__gql_agg" + std::to_string(aggPairs.size())
                                              : userAlias;
        aggPairs.emplace_back(text, alias);
    };
    for (auto &item : items) {
        std::vector<Span> local;
        int counter = 0;
        collectAggregates(item.exprCtx, local, counter);
        for (auto &span : local) {
            std::string text = query.substr(span.start, span.stop - span.start + 1);
            bool whole = (span.start == item.exprStart && span.stop == item.exprStop);
            registerAgg(text, whole ? item.alias : "");
        }
    }
    if (!havingRaw.empty()) {
        std::vector<Span> havingAggs;
        int counter = 0;
        collectAggregates(ctx->havingClause()->searchCondition(), havingAggs, counter);
        for (auto &span : havingAggs) {
            registerAgg(query.substr(span.start, span.stop - span.start + 1), "");
        }
    }

    // WITH keys (explicit: raw binding variables; implicit: non-agg items).
    std::vector<std::string> withParts;
    std::vector<std::pair<std::string, std::string>> keyPairs;
    if (!explicitKeys.empty()) {
        for (auto &key : explicitKeys) {
            withParts.push_back(finishExpr(key));
        }
    } else {
        int keyCounter = 0;
        for (auto &item : items) {
            if (item.hasAggregate) continue;
            std::string alias =
                item.alias.empty() ? "__gql_key" + std::to_string(keyCounter++) : item.alias;
            withParts.push_back(finishExpr(item.exprText) + " AS " + alias);
            keyPairs.emplace_back(item.exprText, alias);
        }
    }
    for (auto &p : aggPairs) {
        withParts.push_back(finishExpr(p.first) + " AS " + p.second);
    }
    std::vector<std::pair<std::string, std::string>> allPairs = keyPairs;
    allPairs.insert(allPairs.end(), aggPairs.begin(), aggPairs.end());

    // HAVING -> WHERE on the WITH output (keys/aggs replaced by aliases).
    std::string havingText;
    if (!havingRaw.empty()) {
        havingText = " WHERE " + finishExpr(replaceExprs(havingRaw, allPairs));
    }

    // RETURN items: key/agg expressions replaced by their WITH aliases.
    std::vector<std::string> returnItems;
    for (auto &item : items) {
        std::string out = finishExpr(replaceExprs(item.exprText, allPairs));
        if (!item.alias.empty() && out != item.alias) {
            out += " AS " + item.alias;
        }
        returnItems.push_back(out);
    }

    std::ostringstream out;
    out << prefix << matchText << whereText << " WITH " << joinCommas(withParts) << havingText
        << " RETURN " << joinCommas(returnItems) << buildOrderPage(allPairs);
    return out.str();
}

// GQL names an unnamed result item after its source text ("max(x)"); LadybugDB
// would otherwise name it from the normalized function spelling ("MAX(x)").
// Backtick-quote the source text as the column alias.
static std::string quoteColumnAlias(const std::string &name) {
    std::string out = "`";
    for (char c : name) {
        if (c == '`') out += "``";
        else out += c;
    }
    out += "`";
    return out;
}

std::string GqlToCypherTransformer::renderSelectItems(
    const std::vector<SelectItemInfo> &items, const std::vector<Span> &replacements) const {
    std::vector<std::string> parts;
    for (auto &item : items) {
        std::string original = item.exprText;
        std::string text = item.exprText;
        std::vector<Span> reps = replacements;
        std::sort(reps.begin(), reps.end(),
                  [](const Span &a, const Span &b) { return a.start > b.start; });
        for (auto &rep : reps) {
            if (rep.start < item.exprStart || rep.stop > item.exprStop) continue;
            text.replace(rep.start - item.exprStart, rep.stop - rep.start + 1, rep.replacement);
        }
        if (!item.alias.empty()) {
            text += " AS " + item.alias;
        } else {
            text += " AS " + quoteColumnAlias(original);
        }
        parts.push_back(text);
    }
    return joinCommas(parts);
}

std::string GqlToCypherTransformer::translateReturnStatement(
    GQLParser::ReturnStatementContext *ctx, GQLParser::OrderByAndPageStatementContext *page) {
    auto *body = ctx->returnStatementBody();
    if (!body) {
        unsupported("RETURN without body");
    }
    bool distinct = false;
    if (auto *sq = body->setQuantifier()) {
        if (sq->DISTINCT()) {
            distinct = true;
        }
    }
    std::string pageText;
    if (page) {
        pageText = " " + translateOrderByAndPage(page);
    }

    if (body->groupByClause()) {
        // RETURN ... GROUP BY uses the same grouping rewrite as SELECT.
        if (body->ASTERISK()) {
            unsupported("RETURN * with GROUP BY");
        }
        std::vector<SelectItemInfo> items;
        for (auto *item : body->returnItemList()->returnItem()) {
            auto *expr = item->aggregatingValueExpression();
            SelectItemInfo info;
            info.exprCtx = expr;
            info.exprStart = expr->getStart()->getStartIndex();
            info.exprStop = expr->getStop()->getStopIndex();
            info.exprText = query.substr(info.exprStart, info.exprStop - info.exprStart + 1);
            if (item->returnItemAlias()) {
                info.alias = sourceText(item->returnItemAlias()->identifier());
            }
            info.hasAggregate = containsAggregate(expr);
            items.push_back(std::move(info));
        }
        std::vector<std::string> explicitKeys;
        auto *list = body->groupByClause()->groupingElementList();
        if (list->emptyGroupingSet()) {
            unsupported("GROUP BY ()");
        }
        for (auto *key : list->groupingElement()) {
            explicitKeys.push_back(sourceText(key->bindingVariableReference()));
        }
        bool hasAgg = std::any_of(items.begin(), items.end(),
                                  [](const SelectItemInfo &i) { return i.hasAggregate; });
        if (!hasAgg) {
            unsupported("RETURN ... GROUP BY without aggregation");
        }

        // Textual aggregate aliasing (same scheme as SELECT's grouped form).
        std::vector<std::pair<std::string, std::string>> aggPairs;
        auto registerAgg = [&](const std::string &text, const std::string &userAlias) {
            for (auto &p : aggPairs) {
                if (p.first == text) return;
            }
            std::string alias = userAlias.empty() ? "__gql_agg" + std::to_string(aggPairs.size())
                                                  : userAlias;
            aggPairs.emplace_back(text, alias);
        };
        for (auto &item : items) {
            std::vector<Span> local;
            int counter = 0;
            collectAggregates(item.exprCtx, local, counter);
            for (auto &span : local) {
                std::string text = query.substr(span.start, span.stop - span.start + 1);
                bool whole = (span.start == item.exprStart && span.stop == item.exprStop);
                registerAgg(text, whole ? item.alias : "");
            }
        }
        std::vector<std::string> withParts;
        for (auto &key : explicitKeys) {
            withParts.push_back(finishExpr(key));
        }
        for (auto &p : aggPairs) {
            withParts.push_back(finishExpr(p.first) + " AS " + p.second);
        }
        std::vector<std::string> returnItems;
        for (auto &item : items) {
            std::string out = finishExpr(replaceExprs(item.exprText, aggPairs));
            if (!item.alias.empty() && out != item.alias) {
                out += " AS " + item.alias;
            } else if (item.alias.empty()) {
                // GQL names unnamed result items after their source text.
                out += " AS " + quoteColumnAlias(item.exprText);
            }
            returnItems.push_back(out);
        }
        // ORDER BY after a grouped RETURN can only reference key/agg aliases.
        if (page && page->orderByClause()) {
            std::string orderText = replaceExprs(sourceText(page->orderByClause()), aggPairs);
            pageText = " " + finishExpr(orderText);
            if (page->offsetClause()) {
                pageText += " SKIP " +
                            finishExpr(sourceText(page->offsetClause()->nonNegativeIntegerSpecification()));
            }
            if (page->limitClause()) {
                pageText += " LIMIT " +
                            finishExpr(sourceText(page->limitClause()->nonNegativeIntegerSpecification()));
            }
        }
        return "WITH " + joinCommas(withParts) + " RETURN " + joinCommas(returnItems) + pageText;
    }

    std::string proj;
    if (body->ASTERISK()) {
        proj = "*";
    } else {
        std::vector<std::string> parts;
        for (auto *item : body->returnItemList()->returnItem()) {
            std::string original = sourceText(item->aggregatingValueExpression());
            std::string text = original;
            if (item->returnItemAlias()) {
                text += " AS " + sourceText(item->returnItemAlias()->identifier());
            } else {
                // GQL names unnamed result items after their source text.
                text += " AS " + quoteColumnAlias(original);
            }
            parts.push_back(text);
        }
        proj = joinCommas(parts);
    }
    return std::string("RETURN ") + (distinct ? "DISTINCT " : "") + finishExpr(proj) + pageText;
}

// =============================================================================
// Write primitives
// =============================================================================

std::string GqlToCypherTransformer::translateInsertStatement(
    GQLParser::InsertStatementContext *ctx) {
    // GQL INSERT == Cypher CREATE. Emitting the keyword structurally (instead
    // of a whole-text keyword replace) keeps string literals intact.
    auto *pattern = ctx->insertGraphPattern();
    checkPatternSupported(pattern);
    return "CREATE " + finishExpr(sourceText(pattern));
}

std::string GqlToCypherTransformer::translateSetStatement(
    GQLParser::SetStatementContext *ctx) {
    // GQL SET is order-independent (all RHS evaluated before assignment);
    // Cypher SET is sequential. When no item's RHS touches a property written
    // by another item the direct mapping is equivalent; otherwise snapshot the
    // RHS values first via WITH.
    auto items = ctx->setItemList()->setItem();
    std::vector<std::string> directParts;
    std::vector<std::string> snapshotParts;
    std::vector<std::string> assignParts;
    std::vector<std::string> snapshotVars; // target variables must survive the WITH
    bool needsSnapshot = false;

    auto addSnapshotVar = [&](const std::string &var) {
        if (std::find(snapshotVars.begin(), snapshotVars.end(), var) == snapshotVars.end()) {
            snapshotVars.push_back(var);
        }
    };

    for (size_t i = 0; i < items.size(); ++i) {
        auto *item = items[i];
        if (item->setLabelItem()) {
            unsupported("SET label (labels are table types in LadybugDB)");
        }
        if (auto *prop = item->setPropertyItem()) {
            std::string var = sourceText(prop->bindingVariableReference());
            std::string name = sourceText(prop->propertyName());
            std::string value = finishExpr(sourceText(prop->valueExpression()));
            directParts.push_back(var + "." + name + " = " + value);
            addSnapshotVar(var);
            snapshotParts.push_back(value + " AS __gql_set" + std::to_string(i));
            assignParts.push_back(var + "." + name + " = __gql_set" + std::to_string(i));
            for (size_t j = 0; j < items.size(); ++j) {
                if (j == i) continue;
                auto *other = items[j]->setPropertyItem();
                if (!other) continue;
                std::string otherVar = sourceText(other->bindingVariableReference());
                std::string otherName = sourceText(other->propertyName());
                if (otherVar == var &&
                    value.find(var + "." + otherName) != std::string::npos) {
                    needsSnapshot = true;
                }
            }
        } else if (auto *all = item->setAllPropertiesItem()) {
            std::string var = sourceText(all->bindingVariableReference());
            std::string props = all->propertyKeyValuePairList()
                                    ? "{" + sourceText(all->propertyKeyValuePairList()) + "}"
                                    : "{}";
            props = finishExpr(props);
            directParts.push_back(var + " = " + props);
            addSnapshotVar(var);
            snapshotParts.push_back(props + " AS __gql_set" + std::to_string(i));
            assignParts.push_back(var + " = __gql_set" + std::to_string(i));
        } else {
            unsupported("SET item");
        }
    }

    if (!needsSnapshot) {
        return "SET " + joinCommas(directParts);
    }
    // Snapshot form: keep the target variables in scope across the WITH.
    std::string with = joinCommas(snapshotVars);
    if (!snapshotParts.empty()) {
        with += (with.empty() ? "" : ", ") + joinCommas(snapshotParts);
    }
    return "WITH " + with + " SET " + joinCommas(assignParts);
}

std::string GqlToCypherTransformer::translateRemoveStatement(
    GQLParser::RemoveStatementContext *ctx) {
    std::vector<std::string> parts;
    for (auto *item : ctx->removeItemList()->removeItem()) {
        if (auto *prop = item->removePropertyItem()) {
            // LadybugDB's Cypher has no REMOVE; on a fixed schema the closest
            // equivalent is assigning NULL (documented approximation).
            parts.push_back(sourceText(prop->bindingVariableReference()) + "." +
                            sourceText(prop->propertyName()) + " = NULL");
        } else if (item->removeLabelItem()) {
            unsupported("REMOVE label (labels are table types in LadybugDB)");
        } else {
            unsupported("REMOVE item");
        }
    }
    return "SET " + joinCommas(parts);
}

std::string GqlToCypherTransformer::translateDeleteStatement(
    GQLParser::DeleteStatementContext *ctx) {
    // GQL (NODETACH | DETACH)? DELETE list → Cypher (DETACH)? DELETE list.
    // GQL's default is NODETACH, matching Cypher's plain DELETE.
    std::string text = ctx->DETACH() ? "DETACH DELETE " : "DELETE ";
    text += finishExpr(sourceText(ctx->deleteItemList()));
    return text;
}

// =============================================================================
// Graph types (Phase 4): CREATE GRAPH TYPE / typed CREATE GRAPH → node/rel
// table schema DDL.
//
// A GQL graph type is canonicalized to named node types (property types) plus
// named edge types (endpoint pair + property types), local aliases stripped —
// the canonical form Neo4j's GraphTypeCanonicalizer normalizes to (adapted from
// neo4j/neo4j GraphTypeCanonicalizer.scala, Apache-2.0; see
// THIRD_PARTY_NOTICES.md). LadybugDB has no graph-type catalog object, so
// CREATE GRAPH TYPE keeps the canonical spec in a process-local registry and
// "CREATE GRAPH g TYPE t" expands it to CREATE GRAPH + CREATE NODE/REL TABLE
// DDL inside that graph's schema. The registry lives for the lifetime of the
// process (documented limitation).
// =============================================================================

namespace {

struct FillerInfo {
    std::vector<std::string> labels;
    std::string labelName; // single label, "" when the label set is empty
    std::vector<GraphTypeProp> props;
};

std::string upperCopy(const std::string &s) {
    std::string out(s.size(), '\0');
    std::transform(s.begin(), s.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}

// Span-based source text (mirrors GqlToCypherTransformer::sourceText, which is
// a member and not reachable from these file-local helpers).
std::string ctxText(const std::string &query, antlr4::ParserRuleContext *ctx) {
    if (!ctx) return "";
    auto *startToken = ctx->getStart();
    auto *stopToken = ctx->getStop();
    if (!startToken || !stopToken) return "";
    size_t startIdx = startToken->getStartIndex();
    size_t stopIdx = stopToken->getStopIndex();
    if (startIdx > query.size() || stopIdx + 1 > query.size() || stopIdx < startIdx) {
        return "";
    }
    return query.substr(startIdx, stopIdx - startIdx + 1);
}

// Graph-type registry lookup (the registry is injected per database — see
// GqlToCypherTransformer's registry member). Keyed by upper-cased type name.
const GraphTypeSpec *findGraphType(const GraphTypeRegistry &registry, const std::string &name) {
    auto it = registry.find(upperCopy(name));
    return it == registry.end() ? nullptr : &it->second;
}

// GQL property value type → LadybugDB column type text (ISO GQL predefined
// types; spellings per GQL.g4 §18.7-18.9). A trailing NOT NULL is accepted and
// dropped: LadybugDB DDL has no NOT NULL constraint (documented approximation).
std::string mapGqlPropertyType(const std::string &raw) {
    std::string text;
    bool pendingSpace = false;
    for (char c : raw) {
        if (std::isspace(static_cast<unsigned char>(c))) {
            pendingSpace = !text.empty();
            continue;
        }
        if (pendingSpace) {
            text += ' ';
            pendingSpace = false;
        }
        text += c;
    }
    std::string upper = upperCopy(text);

    auto endsWith = [&](const char *suffix) {
        size_t n = std::char_traits<char>::length(suffix);
        return upper.size() > n && upper.compare(upper.size() - n, n, suffix) == 0 &&
               upper[upper.size() - n - 1] == ' ';
    };
    if (endsWith("NOT NULL")) {
        upper.erase(upper.size() - 8);
        text.erase(text.size() - 8);
        while (!upper.empty() && upper.back() == ' ') {
            upper.pop_back();
            text.pop_back();
        }
    }

    std::string name = upper;
    std::string args;
    auto paren = upper.find('(');
    if (paren != std::string::npos) {
        name = upper.substr(0, paren);
        args = text.substr(paren); // digits/commas only — case-free
        while (!name.empty() && name.back() == ' ') name.pop_back();
    }
    // GQL "SIGNED?/UNSIGNED <verbose integer type>" spellings.
    if (name.rfind("UNSIGNED ", 0) == 0) name = name.substr(9);
    if (name.rfind("SIGNED ", 0) == 0) name = name.substr(7);

    if (name == "BOOL" || name == "BOOLEAN") return "BOOL";
    if (name == "STRING" || name == "CHAR" || name == "CHARACTER" || name == "VARCHAR") {
        return "STRING";
    }
    if (name == "BYTES" || name == "BINARY" || name == "VARBINARY") return "BLOB";
    if (name == "INT8" || name == "INTEGER8") return "INT8";
    if (name == "INT16" || name == "INTEGER16" || name == "SMALLINT" ||
        name == "SMALL INTEGER") {
        return "INT16";
    }
    if (name == "INT32" || name == "INTEGER32") return "INT32";
    if (name == "INT64" || name == "INTEGER64" || name == "BIGINT" ||
        name == "BIG INTEGER" || name == "INTEGER") {
        return "INT64";
    }
    if (name == "INT128" || name == "INTEGER128") return "INT128";
    if (name == "INT") return "INT32";
    if (name == "UINT8") return "UINT8";
    if (name == "UINT16" || name == "USMALLINT") return "UINT16";
    if (name == "UINT32" || name == "UINT") return "UINT32";
    if (name == "UINT64" || name == "UBIGINT") return "UINT64";
    if (name == "UINT128") return "UINT128";
    if (name == "FLOAT32" || name == "FLOAT" || name == "REAL") return "FLOAT";
    if (name == "FLOAT64" || name == "DOUBLE" || name == "DOUBLE PRECISION") return "DOUBLE";
    if (name == "DECIMAL" || name == "DEC") {
        if (args.empty()) {
            GqlToCypherTransformer::unsupported("DECIMAL without precision");
        }
        return "DECIMAL" + args;
    }
    if (name == "DATE") return "DATE";
    if (name == "TIMESTAMP" || name == "TIMESTAMP WITHOUT TIME ZONE" ||
        name == "LOCAL DATETIME") {
        return "TIMESTAMP";
    }
    if (name == "TIMESTAMP WITH TIME ZONE" || name == "ZONED DATETIME") return "TIMESTAMP_TZ";
    if (name == "DURATION") return "INTERVAL";
    if (name == "TIME" || name == "TIME WITH TIME ZONE" || name == "TIME WITHOUT TIME ZONE" ||
        name == "LOCAL TIME" || name == "ZONED TIME") {
        // GQL has no bare TIME (spelled LOCAL TIME / TIME [WITH|WITHOUT TIME ZONE]
        // / ZONED TIME); LadybugDB has no TIME type at all.
        GqlToCypherTransformer::unsupported("TIME type (LadybugDB has no TIME type)");
    }
    GqlToCypherTransformer::unsupported("property type " + text);
}

// Labels of a <label set phrase>: LABEL x | LABELS x [& y]... | :x [& y]...
std::vector<std::string> labelSetLabels(GQLParser::LabelSetPhraseContext *ctx,
                                        const std::string &query) {
    std::vector<std::string> labels;
    if (!ctx) return labels;
    if (ctx->labelName()) {
        labels.push_back(ctxText(query, ctx->labelName()));
        return labels;
    }
    if (auto *spec = ctx->labelSetSpecification()) {
        for (auto *l : spec->labelName()) {
            labels.push_back(ctxText(query, l));
        }
    }
    return labels;
}

std::string singleLabel(const std::vector<std::string> &labels, const std::string &what) {
    if (labels.empty()) return "";
    if (labels.size() > 1) {
        GqlToCypherTransformer::unsupported(
            "multi-label " + what + " (LadybugDB nodes have a single label)");
    }
    return labels[0];
}

std::vector<GraphTypeProp> parsePropertyTypes(
    GQLParser::PropertyTypesSpecificationContext *ctx, const std::string &query) {
    std::vector<GraphTypeProp> props;
    if (!ctx || !ctx->propertyTypeList()) return props;
    for (auto *p : ctx->propertyTypeList()->propertyType()) {
        GraphTypeProp prop;
        prop.name = ctxText(query, p->propertyName());
        prop.type = mapGqlPropertyType(ctxText(query, p->propertyValueType()));
        if (iequals(prop.name, "_gql_id")) {
            GqlToCypherTransformer::unsupported(
                "property name '_gql_id' (reserved for the synthetic primary key)");
        }
        props.push_back(std::move(prop));
    }
    return props;
}

FillerInfo parseNodeTypeFiller(GQLParser::NodeTypeFillerContext *ctx,
                               const std::string &query) {
    FillerInfo info;
    if (!ctx) return info;
    GQLParser::NodeTypeKeyLabelSetContext *key = ctx->nodeTypeKeyLabelSet();
    if (key && key->labelSetPhrase()) {
        // The key label set names the identifying label of the node type.
        auto labels = labelSetLabels(key->labelSetPhrase(), query);
        info.labelName = singleLabel(labels, "key label set");
        info.labels = labels;
    }
    if (auto *content = ctx->nodeTypeImpliedContent()) {
        if (content->nodeTypeLabelSet()) {
            auto labels = labelSetLabels(content->nodeTypeLabelSet()->labelSetPhrase(), query);
            std::string label = singleLabel(labels, "label set");
            if (!label.empty()) {
                if (info.labelName.empty()) info.labelName = label;
                info.labels.insert(info.labels.end(), labels.begin(), labels.end());
            }
        }
        if (content->nodeTypePropertyTypes()) {
            info.props = parsePropertyTypes(
                content->nodeTypePropertyTypes()->propertyTypesSpecification(), query);
        }
    }
    return info;
}

FillerInfo parseEdgeTypeFiller(GQLParser::EdgeTypeFillerContext *ctx,
                               const std::string &query) {
    FillerInfo info;
    if (!ctx) return info;
    if (auto *key = ctx->edgeTypeKeyLabelSet()) {
        if (key->labelSetPhrase()) {
            auto labels = labelSetLabels(key->labelSetPhrase(), query);
            info.labelName = singleLabel(labels, "key label set");
            info.labels = labels;
        }
    }
    if (auto *content = ctx->edgeTypeImpliedContent()) {
        if (content->edgeTypeLabelSet()) {
            auto labels = labelSetLabels(content->edgeTypeLabelSet()->labelSetPhrase(), query);
            std::string label = singleLabel(labels, "label set");
            if (!label.empty()) {
                if (info.labelName.empty()) info.labelName = label;
                info.labels.insert(info.labels.end(), labels.begin(), labels.end());
            }
        }
        if (content->edgeTypePropertyTypes()) {
            info.props = parsePropertyTypes(
                content->edgeTypePropertyTypes()->propertyTypesSpecification(), query);
        }
    }
    return info;
}

// Element-type names (node type names, their aliases and labels) all resolve
// to the canonical node type name — the same alias-stripped form
// GraphTypeCanonicalizer produces.
void registerTypeName(std::map<std::string, std::string> &names, const std::string &key,
                      const std::string &canonical) {
    if (key.empty()) return;
    auto [it, inserted] = names.emplace(upperCopy(key), canonical);
    if (!inserted && !iequals(it->second, canonical)) {
        GqlToCypherTransformer::unsupported("name '" + key +
                                            "' used for multiple element types in a graph type");
    }
}

std::string resolveTypeName(const std::map<std::string, std::string> &names,
                            const std::string &key, const std::string &what) {
    auto it = names.find(upperCopy(key));
    if (it == names.end()) {
        GqlToCypherTransformer::unsupported(what + " references undefined node type " + key);
    }
    return it->second;
}

// Endpoint reference in an edge type pattern: (alias) | (label filler) | ().
std::string resolveEndpointRef(const std::string &query,
                               const std::map<std::string, std::string> &names,
                               const std::string &aliasText,
                               GQLParser::NodeTypeFillerContext *filler,
                               const std::string &what) {
    if (!aliasText.empty()) {
        return resolveTypeName(names, aliasText, what);
    }
    if (filler) {
        FillerInfo info = parseNodeTypeFiller(filler, query);
        if (!info.labelName.empty()) {
            return resolveTypeName(names, info.labelName, what);
        }
    }
    GqlToCypherTransformer::unsupported(
        what + " with an anonymous endpoint reference (name the node type)");
}

void parseNodeTypeSpecification(GQLParser::NodeTypeSpecificationContext *ctx,
                                const std::string &query, GraphTypeSpec &spec,
                                std::map<std::string, std::string> &names) {
    std::string name;
    std::string alias;
    FillerInfo filler;

    if (auto *pattern = ctx->nodeTypePattern()) {
        if (pattern->nodeTypeName()) name = ctxText(query, pattern->nodeTypeName());
        if (pattern->localNodeTypeAlias()) {
            alias = ctxText(query, pattern->localNodeTypeAlias());
        }
        filler = parseNodeTypeFiller(pattern->nodeTypeFiller(), query);
    } else {
        auto *phrase = ctx->nodeTypePhrase();
        auto *phraseFiller = phrase->nodeTypePhraseFiller();
        if (phraseFiller->nodeTypeName()) name = ctxText(query, phraseFiller->nodeTypeName());
        if (phrase->localNodeTypeAlias()) alias = ctxText(query, phrase->localNodeTypeAlias());
        filler = parseNodeTypeFiller(phraseFiller->nodeTypeFiller(), query);
    }
    if (name.empty()) name = filler.labelName;
    if (name.empty()) {
        GqlToCypherTransformer::unsupported("anonymous node type in a graph type");
    }
    for (const auto &n : spec.nodes) {
        if (iequals(n.name, name)) {
            GqlToCypherTransformer::unsupported("duplicate node type name " + name +
                                                " in a graph type");
        }
    }
    spec.nodes.push_back({name, std::move(filler.props)});
    registerTypeName(names, name, name);
    registerTypeName(names, alias, name);
    for (const auto &label : filler.labels) {
        registerTypeName(names, label, name);
    }
}

void parseEdgeTypeSpecification(GQLParser::EdgeTypeSpecificationContext *ctx,
                                const std::string &query, GraphTypeSpec &spec,
                                const std::map<std::string, std::string> &names) {
    const std::string what = "edge type";
    std::string name;
    std::string from;
    std::string to;
    FillerInfo filler;

    auto rejectUndirected = []() {
        GqlToCypherTransformer::unsupported(
            "undirected edge type (LadybugDB rel tables are directed)");
    };

    if (auto *pattern = ctx->edgeTypePattern()) {
        if (pattern->edgeTypePatternUndirected()) rejectUndirected();
        if (pattern->edgeKind() &&
            upperCopy(ctxText(query, pattern->edgeKind())) == "UNDIRECTED") {
            rejectUndirected();
        }
        if (pattern->edgeTypeName()) name = ctxText(query, pattern->edgeTypeName());
        auto *directed = pattern->edgeTypePatternDirected();
        if (auto *right = directed->edgeTypePatternPointingRight()) {
            filler = parseEdgeTypeFiller(right->arcTypePointingRight()->edgeTypeFiller(), query);
            auto *srcRef = right->sourceNodeTypeReference();
            auto *dstRef = right->destinationNodeTypeReference();
            from = resolveEndpointRef(query, names,
                srcRef->sourceNodeTypeAlias() ? ctxText(query, srcRef->sourceNodeTypeAlias()) : "",
                srcRef->nodeTypeFiller(), what);
            to = resolveEndpointRef(query, names,
                dstRef->destinationNodeTypeAlias()
                    ? ctxText(query, dstRef->destinationNodeTypeAlias())
                    : "",
                dstRef->nodeTypeFiller(), what);
        } else {
            // (destination)<-[filler]-(source)
            auto *left = directed->edgeTypePatternPointingLeft();
            filler = parseEdgeTypeFiller(left->arcTypePointingLeft()->edgeTypeFiller(), query);
            auto *dstRef = left->destinationNodeTypeReference();
            auto *srcRef = left->sourceNodeTypeReference();
            to = resolveEndpointRef(query, names,
                dstRef->destinationNodeTypeAlias()
                    ? ctxText(query, dstRef->destinationNodeTypeAlias())
                    : "",
                dstRef->nodeTypeFiller(), what);
            from = resolveEndpointRef(query, names,
                srcRef->sourceNodeTypeAlias() ? ctxText(query, srcRef->sourceNodeTypeAlias()) : "",
                srcRef->nodeTypeFiller(), what);
        }
    } else {
        auto *phrase = ctx->edgeTypePhrase();
        if (phrase->edgeKind() && upperCopy(ctxText(query, phrase->edgeKind())) == "UNDIRECTED") {
            rejectUndirected();
        }
        auto *phraseFiller = phrase->edgeTypePhraseFiller();
        if (phraseFiller->edgeTypeName()) name = ctxText(query, phraseFiller->edgeTypeName());
        filler = parseEdgeTypeFiller(phraseFiller->edgeTypeFiller(), query);

        auto *pair = phrase->endpointPairPhrase()->endpointPair();
        if (pair->endpointPairUndirected()) rejectUndirected();
        auto *directed = pair->endpointPairDirected();
        if (auto *right = directed->endpointPairPointingRight()) {
            from = resolveTypeName(names, ctxText(query, right->sourceNodeTypeAlias()), what);
            to = resolveTypeName(names, ctxText(query, right->destinationNodeTypeAlias()), what);
        } else {
            // (destination, <- source)
            auto *left = directed->endpointPairPointingLeft();
            to = resolveTypeName(names, ctxText(query, left->destinationNodeTypeAlias()), what);
            from = resolveTypeName(names, ctxText(query, left->sourceNodeTypeAlias()), what);
        }
    }

    if (name.empty()) name = filler.labelName;
    if (name.empty()) {
        GqlToCypherTransformer::unsupported("edge type without a name in a graph type");
    }
    // Same edge type name over several endpoint pairs is one rel table with
    // multiple FROM/TO connections; its property types must agree.
    for (const auto &e : spec.edges) {
        if (!iequals(e.name, name)) continue;
        if (e.props.size() != filler.props.size()) {
            GqlToCypherTransformer::unsupported(
                "edge type " + name + " with conflicting property types across endpoint pairs");
        }
        for (size_t i = 0; i < e.props.size(); i++) {
            if (e.props[i].name != filler.props[i].name || e.props[i].type != filler.props[i].type) {
                GqlToCypherTransformer::unsupported(
                    "edge type " + name + " with conflicting property types across endpoint pairs");
            }
        }
    }
    spec.edges.push_back({name, from, to, std::move(filler.props)});
}

GraphTypeSpec parseGraphTypeSpecification(
    GQLParser::NestedGraphTypeSpecificationContext *ctx, const std::string &query) {
    GraphTypeSpec spec;
    auto *body = ctx->graphTypeSpecificationBody();
    if (!body || !body->elementTypeList()) {
        GqlToCypherTransformer::unsupported("empty graph type specification");
    }
    auto elements = body->elementTypeList()->elementTypeSpecification();
    // Two passes: edge endpoints may forward-reference node types.
    std::map<std::string, std::string> names;
    std::vector<GQLParser::EdgeTypeSpecificationContext *> edges;
    for (auto *el : elements) {
        if (el->nodeTypeSpecification()) {
            parseNodeTypeSpecification(el->nodeTypeSpecification(), query, spec, names);
        } else if (el->edgeTypeSpecification()) {
            edges.push_back(el->edgeTypeSpecification());
        } else {
            GqlToCypherTransformer::unsupported("element type specification");
        }
    }
    for (auto *e : edges) {
        parseEdgeTypeSpecification(e, query, spec, names);
    }
    return spec;
}

// "CREATE GRAPH g" + per-element "CREATE NODE TABLE"/"CREATE REL TABLE" DDL.
// Node tables carry a synthetic "_gql_id SERIAL PRIMARY KEY": GQL node types
// have no key property and LadybugDB node tables require a primary key. The
// column is auto-filled on INSERT (documented approximation). "_gql_id" is not
// one of the engine's reserved column names (InternalKeyword::_ID etc.).
std::string emitGraphTypeDdl(const std::string &graphName, const GraphTypeSpec &spec) {
    std::string out = "CREATE GRAPH " + graphName + "; USE GRAPH " + graphName;
    for (const auto &node : spec.nodes) {
        out += "; CREATE NODE TABLE " + node.name + "(_gql_id SERIAL PRIMARY KEY";
        for (const auto &p : node.props) {
            out += ", " + p.name + " " + p.type;
        }
        out += ")";
    }
    // Group edge types by name (first-seen order): one rel table per edge type,
    // one FROM/TO connection per endpoint pair.
    std::vector<std::string> order;
    std::map<std::string, std::vector<const GraphTypeEdge *>> groups;
    for (const auto &e : spec.edges) {
        std::string key = upperCopy(e.name);
        if (!groups.contains(key)) order.push_back(e.name);
        groups[key].push_back(&e);
    }
    for (const auto &name : order) {
        const auto &pairs = groups.at(upperCopy(name));
        out += "; CREATE REL TABLE " + name + "(";
        for (size_t i = 0; i < pairs.size(); i++) {
            if (i) out += ", ";
            out += "FROM " + pairs[i]->from + " TO " + pairs[i]->to;
        }
        if (!pairs[0]->props.empty()) {
            out += ", ";
            for (size_t i = 0; i < pairs[0]->props.size(); i++) {
                if (i) out += ", ";
                out += pairs[0]->props[i].name + " " + pairs[0]->props[i].type;
            }
        }
        out += ")";
    }
    // GQL catalog statements return empty results; the DDL above leaves a
    // message row, so end with a zero-row tail.
    out += "; ";
    out += GqlToCypherTransformer::EMPTY_RESULT_CYPHER;
    return out;
}

// Graph type reference → type name ("$param" and qualified names rejected).
std::string graphTypeRefName(const std::string &query,
                             GQLParser::GraphTypeReferenceContext *ref) {
    if (!ref || !ref->catalogGraphTypeParentAndName()) {
        GqlToCypherTransformer::unsupported("graph type reference parameter");
    }
    auto *parentAndName = ref->catalogGraphTypeParentAndName();
    if (parentAndName->catalogObjectParentReference()) {
        GqlToCypherTransformer::unsupported("qualified graph type name");
    }
    return ctxText(query, parentAndName->graphTypeName());
}

} // namespace

// =============================================================================
// Catalog / session
// =============================================================================

std::string GqlToCypherTransformer::translateCreateGraphStatement(
    GQLParser::CreateGraphStatementContext *ctx) {
    if (ctx->OR() || ctx->REPLACE()) {
        unsupported("CREATE OR REPLACE GRAPH");
    }

    auto parentAndName = ctx->catalogGraphParentAndName();
    if (!parentAndName || !parentAndName->graphName()) {
        unsupported("CREATE GRAPH without a graph name");
    }
    if (parentAndName->catalogObjectParentReference()) {
        unsupported("qualified graph name (schemas are not mapped)");
    }
    std::string name = sourceText(parentAndName->graphName());

    // The GQL grammar lets a regular identifier be the keyword TYPE, so
    // "CREATE GRAPH TYPE <t> AS COPY OF <u>" is ambiguous with a graph named
    // TYPE and ANTLR resolves it to CREATE GRAPH. Detect that mis-parse and
    // reinterpret it as the CREATE GRAPH TYPE statement it clearly is.
    if (iequals(name, "TYPE")) {
        auto *of = ctx->ofGraphType();
        if (!of || !of->graphTypeReference() || !ctx->graphSource()) {
            unsupported("CREATE GRAPH named TYPE (quote the name: CREATE GRAPH \"TYPE\" ...)");
        }
        std::string typeName = graphTypeRefName(query, of->graphTypeReference());
        std::string other = sourceText(ctx->graphSource()->graphExpression());
        const GraphTypeSpec *srcSpec = findGraphType(*registry, other);
        if (!srcSpec) {
            throw common::RuntimeException{"Graph type " + other + " is not defined"};
        }
        if (registry->contains(upperCopy(typeName))) {
            throw common::RuntimeException{"Graph type " + typeName + " already exists"};
        }
        (*registry)[upperCopy(typeName)] = *srcSpec;
        return EMPTY_RESULT_CYPHER;
    }

    if (ctx->graphSource()) {
        unsupported("CREATE GRAPH ... AS COPY OF <graph>");
    }
    // GQL "IF NOT EXISTS" is enforced at rewrite time (LadybugDB Cypher has no
    // IF NOT EXISTS for CREATE GRAPH).
    sawIfNotExistsCreateGraph = ctx->IF() && ctx->NOT();
    createGraphName = name;

    // Open ("ANY") property graph → LadybugDB's ANY graph: arbitrary labels and
    // properties without a schema, the same open-graph semantics.
    if (ctx->openGraphType()) {
        return "CREATE GRAPH " + name + " ANY; " + std::string(EMPTY_RESULT_CYPHER);
    }

    // Typed graph: expand the graph type into node/rel table DDL.
    auto *of = ctx->ofGraphType();
    if (!of) {
        unsupported("CREATE GRAPH without a graph type clause");
    }
    if (of->graphTypeLikeGraph()) {
        unsupported("CREATE GRAPH ... LIKE <graph>");
    }
    GraphTypeSpec spec;
    if (of->graphTypeReference()) {
        std::string typeName = graphTypeRefName(query, of->graphTypeReference());
        const GraphTypeSpec *src = findGraphType(*registry, typeName);
        if (!src) {
            throw common::RuntimeException{"Graph type " + typeName + " is not defined"};
        }
        spec = *src;
    } else if (of->nestedGraphTypeSpecification()) {
        spec = parseGraphTypeSpecification(of->nestedGraphTypeSpecification(), query);
    } else {
        unsupported("CREATE GRAPH type reference (parameter)");
    }
    return emitGraphTypeDdl(name, spec);
}

std::string GqlToCypherTransformer::translateCreateGraphTypeStatement(
    GQLParser::CreateGraphTypeStatementContext *ctx) {
    auto *parentAndName = ctx->catalogGraphTypeParentAndName();
    if (!parentAndName || !parentAndName->graphTypeName()) {
        unsupported("CREATE GRAPH TYPE without a type name");
    }
    if (parentAndName->catalogObjectParentReference()) {
        unsupported("qualified graph type name");
    }
    std::string name = sourceText(parentAndName->graphTypeName());
    bool orReplace = ctx->OR() && ctx->REPLACE();
    bool ifNotExists = ctx->IF() && ctx->NOT();

    auto *src = ctx->graphTypeSource();
    GraphTypeSpec spec;
    if (src->nestedGraphTypeSpecification()) {
        spec = parseGraphTypeSpecification(src->nestedGraphTypeSpecification(), query);
    } else if (src->copyOfGraphType()) {
        std::string other = graphTypeRefName(query, src->copyOfGraphType()->graphTypeReference());
        const GraphTypeSpec *srcSpec = findGraphType(*registry, other);
        if (!srcSpec) {
            throw common::RuntimeException{"Graph type " + other + " is not defined"};
        }
        spec = *srcSpec;
    } else {
        unsupported("CREATE GRAPH TYPE ... LIKE <graph>");
    }

    std::string key = upperCopy(name);
    if (registry->contains(key) && !orReplace) {
        if (ifNotExists) return EMPTY_RESULT_CYPHER;
        throw common::RuntimeException{"Graph type " + name + " already exists"};
    }
    (*registry)[key] = std::move(spec);
    return EMPTY_RESULT_CYPHER;
}

std::string GqlToCypherTransformer::translateDropGraphTypeStatement(
    GQLParser::DropGraphTypeStatementContext *ctx) {
    auto *parentAndName = ctx->catalogGraphTypeParentAndName();
    if (!parentAndName || !parentAndName->graphTypeName()) {
        unsupported("DROP GRAPH TYPE without a type name");
    }
    if (parentAndName->catalogObjectParentReference()) {
        unsupported("qualified graph type name");
    }
    std::string name = sourceText(parentAndName->graphTypeName());
    if (registry->erase(upperCopy(name)) == 0) {
        if (ctx->IF()) return EMPTY_RESULT_CYPHER;
        throw common::RuntimeException{"Graph type " + name + " does not exist"};
    }
    return EMPTY_RESULT_CYPHER;
}

std::string GqlToCypherTransformer::translateDropGraphStatement(
    GQLParser::DropGraphStatementContext *ctx) {
    auto parentAndName = ctx->catalogGraphParentAndName();
    if (!parentAndName || !parentAndName->graphName()) {
        unsupported("DROP GRAPH without a graph name");
    }
    if (parentAndName->catalogObjectParentReference()) {
        unsupported("qualified graph name (schemas are not mapped)");
    }
    std::string text = "DROP GRAPH ";
    if (ctx->IF()) {
        text += "IF EXISTS ";
    }
    text += sourceText(parentAndName->graphName());
    return text + "; " + EMPTY_RESULT_CYPHER;
}

std::string GqlToCypherTransformer::translateSessionSetGraphClause(
    GQLParser::SessionSetGraphClauseContext *ctx) {
    // SESSION SET GRAPH g == USE GRAPH g (both are session-sticky).
    return "USE GRAPH " + finishExpr(sourceText(ctx->graphExpression()));
}

// =============================================================================
// Pattern safety: GQL-only pattern features reach Cypher as syntax errors, so
// reject them up front with a named error instead.
// =============================================================================

namespace {

bool walkForUnsupportedPatterns(antlr4::tree::ParseTree *node, std::string &feature) {
    if (dynamic_cast<GQLParser::GraphPatternQuantifierContext *>(node)) {
        feature = "quantified path pattern (...{*|+|{m,n}})";
        return true;
    }
    if (dynamic_cast<GQLParser::MatchModeContext *>(node)) {
        feature = "match mode (REPEATABLE ELEMENTS / DIFFERENT RELATIONSHIPS)";
        return true;
    }
    if (dynamic_cast<GQLParser::PathModeContext *>(node)) {
        feature = "path mode (WALK/TRAIL/SIMPLE/ACYCLIC prefix)";
        return true;
    }
    if (dynamic_cast<GQLParser::PathPatternPrefixContext *>(node)) {
        feature = "path search prefix (ALL/ANY/SHORTEST ...)";
        return true;
    }
    if (dynamic_cast<GQLParser::LabelExpressionNegationContext *>(node) ||
        dynamic_cast<GQLParser::LabelExpressionConjunctionContext *>(node) ||
        dynamic_cast<GQLParser::LabelExpressionDisjunctionContext *>(node) ||
        dynamic_cast<GQLParser::LabelExpressionWildcardContext *>(node) ||
        dynamic_cast<GQLParser::LabelExpressionParenthesizedContext *>(node)) {
        feature = "label expression operator (&, !, |, %, parentheses)";
        return true;
    }
    for (auto *child : node->children) {
        if (walkForUnsupportedPatterns(child, feature)) {
            return true;
        }
    }
    return false;
}

} // namespace

void GqlToCypherTransformer::checkPatternSupported(antlr4::ParserRuleContext *ctx) {
    std::string feature;
    if (walkForUnsupportedPatterns(ctx, feature)) {
        unsupported(feature);
    }
}

// =============================================================================
// Aggregate detection
// =============================================================================

void GqlToCypherTransformer::collectAggregates(antlr4::tree::ParseTree *node,
                                               std::vector<Span> &out, int &counter) {
    if (auto *agg = dynamic_cast<GQLParser::AggregateFunctionContext *>(node)) {
        // Reject stddev early — LadybugDB has no such aggregate.
        if (auto *gen = agg->generalSetFunction()) {
            auto *type = gen->generalSetFunctionType();
            if (type && (type->STDDEV_SAMP() || type->STDDEV_POP())) {
                unsupported("STDDEV_SAMP/STDDEV_POP aggregation");
            }
        }
        size_t start = agg->getStart()->getStartIndex();
        size_t stop = agg->getStop()->getStopIndex();
        out.push_back(Span{start, stop, "__gql_agg" + std::to_string(counter++)});
        return; // don't descend into nested aggregates
    }
    if (!node) return;
    for (auto *child : node->children) {
        collectAggregates(child, out, counter);
    }
}

bool GqlToCypherTransformer::containsAggregate(antlr4::tree::ParseTree *node) {
    if (!node) return false;
    if (dynamic_cast<GQLParser::AggregateFunctionContext *>(node)) {
        return true;
    }
    for (auto *child : node->children) {
        if (containsAggregate(child)) {
            return true;
        }
    }
    return false;
}

// =============================================================================
// Expression-level mappings
// =============================================================================

// GQL spellings that differ from LadybugDB's function catalog. Adapted from
// Neo4j's GQLAliasFunctionNameRewriter (Apache-2.0) and re-targeted: most GQL
// names (UPPER/LOWER/CEILING/LN/COUNT/SUM/...) already match LadybugDB, so
// only the divergent ones are listed here.
// Mapping table informed by:
//   https://github.com/neo4j/neo4j — Apache License 2.0
static const std::pair<const char *, const char *> GQL_FUNCTION_MAP[] = {
    {"COLLECT_LIST", "COLLECT"},
    {"PERCENTILE_DISC", "PERCENTILEDISC"},
    {"PERCENTILE_CONT", "PERCENTILECONT"},
    {"CHAR_LENGTH", "SIZE"},
    {"CHARACTER_LENGTH", "SIZE"},
    {"LOCAL_DATETIME", "TIMESTAMP"},
    {"ZONED_DATETIME", "TIMESTAMP"},
    {"PATH_LENGTH", "LENGTH"},
    {"ELEMENT_ID", "internal_id"},
};

// Copies `text` to `out`, replacing function identifiers outside literals.
static void mapIdentifiersInto(const std::string &text, std::string &out) {
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        // Copy string / quoted-identifier literals verbatim.
        if (c == '\'' || c == '"' || c == '`') {
            char quote = c;
            out += c;
            ++i;
            while (i < text.size()) {
                out += text[i];
                if (text[i] == quote) {
                    if (i + 1 < text.size() && text[i + 1] == quote) {
                        out += text[i + 1];
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t start = i;
            while (i < text.size() &&
                   (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) {
                ++i;
            }
            std::string word = text.substr(start, i - start);
            bool afterDot = start > 0 && text[start - 1] == '.';
            bool isCall = i < text.size() && text[i] == '(';
            if (!afterDot && isCall) {
                std::string upper(word.size(), '\0');
                std::transform(word.begin(), word.end(), upper.begin(),
                               [](unsigned char ch) { return std::toupper(ch); });
                bool mapped = false;
                for (auto &entry : GQL_FUNCTION_MAP) {
                    if (upper == entry.first) {
                        out += entry.second;
                        mapped = true;
                        break;
                    }
                }
                if (mapped) continue;
                if (upper == "LOCAL_TIME" || upper == "ZONED_TIME") {
                    GqlToCypherTransformer::unsupported("function " + upper +
                                                        " (no TIME type in LadybugDB)");
                }
            }
            out += word;
            continue;
        }
        out += c;
        ++i;
    }
}

std::string GqlToCypherTransformer::mapIdentifiers(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    mapIdentifiersInto(text, out);
    return out;
}

std::string GqlToCypherTransformer::mapOperators(const std::string &text) {
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        char c = text[i];
        if (c == '\'' || c == '"' || c == '`') {
            char quote = c;
            out += c;
            ++i;
            while (i < text.size()) {
                out += text[i];
                if (text[i] == quote) {
                    if (i + 1 < text.size() && text[i + 1] == quote) {
                        out += text[i + 1];
                        i += 2;
                        continue;
                    }
                    ++i;
                    break;
                }
                ++i;
            }
            continue;
        }
        // GQL `||` string concatenation → LadybugDB `+`.
        if (c == '|' && i + 1 < text.size() && text[i + 1] == '|') {
            out += '+';
            i += 2;
            continue;
        }
        out += c;
        ++i;
    }
    return out;
}

std::string GqlToCypherTransformer::finishExpr(const std::string &text) const {
    return mapOperators(mapIdentifiers(text));
}

// =============================================================================
// Helpers
// =============================================================================

std::string GqlToCypherTransformer::sourceText(antlr4::ParserRuleContext *ctx) const {
    if (!ctx) return "";

    auto startToken = ctx->getStart();
    auto stopToken = ctx->getStop();

    if (!startToken || !stopToken) return "";

    size_t startIdx = startToken->getStartIndex();
    size_t stopIdx = stopToken->getStopIndex();

    if (startIdx > query.size() || stopIdx + 1 > query.size() || stopIdx < startIdx) {
        return "";
    }

    return query.substr(startIdx, stopIdx - startIdx + 1);
}

std::string GqlToCypherTransformer::replaceWord(const std::string &str,
                                                const std::string &from,
                                                const std::string &to) {
    std::regex wordRe("\\b" + from + "\\b", std::regex::icase);
    return std::regex_replace(str, wordRe, to);
}

} // namespace gql_extension
} // namespace lbug
