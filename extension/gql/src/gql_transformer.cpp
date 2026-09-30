#include "gql_transformer.hpp"

#include "common/exception/runtime.h"

#include <algorithm>
#include <cctype>
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
    if (prim->createGraphTypeStatement()) {
        unsupported("CREATE GRAPH TYPE (typed graphs are not mapped yet)");
    }
    if (prim->dropGraphTypeStatement()) {
        unsupported("DROP GRAPH TYPE (typed graphs are not mapped yet)");
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
    checkPatternSupported(ctx);
    return finishExpr(sourceText(ctx));
}

std::string GqlToCypherTransformer::translateFilterStatement(
    GQLParser::FilterStatementContext *ctx) {
    GQLParser::SearchConditionContext *cond = ctx->searchCondition();
    if (!cond && ctx->whereClause()) {
        cond = ctx->whereClause()->searchCondition();
    }
    return "WHERE " + finishExpr(sourceText(cond));
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
        matchText = translateMatchStatement(graphMatch->matchStatement());
    }

    // ---- WHERE ----
    std::string whereText;
    if (auto *w = ctx->whereClause()) {
        whereText = " WHERE " + finishExpr(sourceText(w->searchCondition()));
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

std::string GqlToCypherTransformer::renderSelectItems(
    const std::vector<SelectItemInfo> &items, const std::vector<Span> &replacements) const {
    std::vector<std::string> parts;
    for (auto &item : items) {
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
            std::string text = sourceText(item->aggregatingValueExpression());
            if (item->returnItemAlias()) {
                text += " AS " + sourceText(item->returnItemAlias()->identifier());
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
// Catalog / session
// =============================================================================

std::string GqlToCypherTransformer::translateCreateGraphStatement(
    GQLParser::CreateGraphStatementContext *ctx) {
    if (ctx->OR() || ctx->REPLACE()) {
        unsupported("CREATE OR REPLACE GRAPH");
    }
    // GQL "IF NOT EXISTS" is enforced at rewrite time (LadybugDB Cypher has no
    // IF NOT EXISTS for CREATE GRAPH).
    sawIfNotExistsCreateGraph = ctx->IF() && ctx->NOT();

    auto parentAndName = ctx->catalogGraphParentAndName();
    if (!parentAndName || !parentAndName->graphName()) {
        unsupported("CREATE GRAPH without a graph name");
    }
    std::string name = sourceText(parentAndName->graphName());
    createGraphName = name;
    // PROPERTY / ANY / IF NOT EXISTS modifiers don't exist in LadybugDB Cypher.
    return "CREATE GRAPH " + name;
}

std::string GqlToCypherTransformer::translateDropGraphStatement(
    GQLParser::DropGraphStatementContext *ctx) {
    auto parentAndName = ctx->catalogGraphParentAndName();
    if (!parentAndName || !parentAndName->graphName()) {
        unsupported("DROP GRAPH without a graph name");
    }
    std::string text = "DROP GRAPH ";
    if (ctx->IF()) {
        text += "IF EXISTS ";
    }
    text += sourceText(parentAndName->graphName());
    return text;
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
