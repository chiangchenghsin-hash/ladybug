#include "utils/fts_utils.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <memory>
#include <optional>

#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "cppjieba/Jieba.hpp"
#include "function/stem.h"
#include "libstemmer.h"
#include "mecab.h"
#include "re2.h"
#include "storage/storage_manager.h"
#include "storage/table/node_table.h"
#include "utils/dict_dir_resolver.h"

namespace lbug {
namespace fts_extension {

using namespace lbug::common;
using namespace lbug::storage;
using namespace lbug::transaction;
using namespace lbug::catalog;

void FTSUtils::normalizeQuery(std::string& query, const RE2& ignorePattern,
    bool protectWildcardChars) {
    // Wildcard characters in the query must survive normalization, even if the ignore pattern
    // would match them (e.g. a negated character class like [^[:alnum:]-]+ matches any
    // non-alphanumeric character, including '*' and '?'). To achieve this, we normalize the
    // query in segments between wildcard characters and re-attach the wildcard characters
    // afterwards. Document contents are normalized without wildcard protection.
    if (protectWildcardChars) {
        std::string result;
        std::string segment;
        auto normalizeSegment = [&]() {
            if (!segment.empty()) {
                RE2::GlobalReplace(&segment, ignorePattern, " ");
                result += segment;
                segment.clear();
            }
        };
        for (auto c : query) {
            if (c == '*' || c == '?') {
                normalizeSegment();
                result += c;
            } else {
                segment += c;
            }
        }
        normalizeSegment();
        StringUtils::toLower(result);
        query = std::move(result);
    } else {
        RE2::GlobalReplace(&query, ignorePattern, " ");
        StringUtils::toLower(query);
    }
}

struct StopWordsChecker {
    ValueVector termsVector;
    storage::NodeTable* stopWordsTable;
    transaction::Transaction* tx;
    common::offset_t offset = common::INVALID_OFFSET;
    std::function<bool(const std::string& term)> isStopWord;

    StopWordsChecker(MemoryManager* mm, NodeTable* stopWordsTable, Transaction* tx,
        bool defaultStopWords);
};

StopWordsChecker::StopWordsChecker(MemoryManager* mm, NodeTable* stopwordsTable, Transaction* tx,
    bool defaultStopWords)
    : termsVector{LogicalType::STRING(), mm}, stopWordsTable{stopwordsTable}, tx{tx} {
    termsVector.state = common::DataChunkState::getSingleValueDataChunkState();
    if (defaultStopWords) {
        isStopWord = [](const std::string& term) {
            return StopWords::getDefaultStopWords().contains(term);
        };
    } else {
        isStopWord = [this](const std::string& term) {
            termsVector.setValue(0, term);
            return stopWordsTable->lookupPK(this->tx, &termsVector, 0 /* vectorPos */, offset);
        };
    }
}

bool FTSUtils::hasWildcardPattern(const std::string& term) {
    for (auto& c : term) {
        if (c == '*' || c == '?') {
            return true;
        }
    }
    return false;
}

std::vector<std::string> FTSUtils::stemTerms(std::vector<std::string> terms,
    const FTSConfig& config, MemoryManager* mm, NodeTable* stopwordsTable, Transaction* tx,
    bool isConjunctive, bool isQuery) {
    if (config.stemmer == "none" && !isConjunctive) {
        return terms;
    }
    std::optional<StopWordsChecker> checker;
    if (isConjunctive) {
        checker.emplace(mm, stopwordsTable, tx, config.stopWordsSource == StopWords::DEFAULT_VALUE);
    }
    if (config.stemmer == "none") {
        std::vector<std::string> result;
        for (auto& term : terms) {
            if (checker->isStopWord(term)) {
                continue;
            }
            result.push_back(term);
        }
        return result;
    }
    StemFunction::validateStemmer(config.stemmer);
    auto sbStemmer = sb_stemmer_new(reinterpret_cast<const char*>(config.stemmer.c_str()), "UTF_8");
    if (!sbStemmer) {
        // If stemmer creation fails, fall back to returning original terms to avoid crashes.
        return terms;
    }
    std::vector<std::string> result;
    for (auto& term : terms) {
        if (checker && checker->isStopWord(term)) {
            continue;
        }
        if (isQuery && hasWildcardPattern(term)) {
            result.push_back(term);
            continue;
        }
        auto stemData = sb_stemmer_stem(sbStemmer, reinterpret_cast<const sb_symbol*>(term.c_str()),
            term.length());
        if (stemData) {
            result.push_back(std::string(reinterpret_cast<const char*>(stemData)));
        } else {
            // If stemming fails for a term, keep the original term.
            result.push_back(term);
        }
    }
    sb_stemmer_delete(sbStemmer);
    return result;
}

std::vector<std::string> FTSUtils::tokenizeString(std::string& str, const FTSConfig& config) {
    std::vector<std::string> terms;
    if (config.tokenizer == "jieba") {
        cppjieba::Jieba jieba(config.jiebaDictDir + "/jieba.dict.utf8",
            config.jiebaDictDir + "/hmm_model.utf8", config.jiebaDictDir + "/user.dict.utf8",
            config.jiebaDictDir + "/idf.utf8", config.jiebaDictDir + "/stop_words.utf8");
        jieba.CutForSearch(str, terms);
        // CutForSearch keeps the whitespace between words as separate tokens. Whitespace is
        // never a meaningful term, so we skip those tokens.
        std::erase_if(terms, [](const std::string& term) {
            return std::all_of(term.begin(), term.end(),
                [](unsigned char c) { return std::isspace(c); });
        });
    } else if (config.tokenizer == "mecab") {
        // Same cross-machine fallback as the jieba tokenizer: a stale
        // machine-specific path from the catalog is retried against the
        // extension-adjacent dictionary before giving up.
        auto dictDir = resolveDictDir(config.jiebaDictDir, DictKind::MECAB);
        // Pass the rc file explicitly. Without -r, MeCab falls back to
        // MECAB_DEFAULT_RC, a build-time absolute path that does not exist in
        // deployed environments — the tagger then fails to create.
        std::unique_ptr<MeCab::Tagger, decltype(&MeCab::deleteTagger)> tagger(
            MeCab::createTagger((std::string("-d ") + dictDir + " -r " + dictDir + "/mecabrc").c_str()),
            MeCab::deleteTagger);
        if (!tagger) {
            auto lastError = MeCab::getLastError();
            throw common::BinderException{std::format(
                "Failed to create mecab tagger with dict dir: '{}'. (mecab error: {})", dictDir,
                lastError ? lastError : "unknown")};
        }
        // parseToNode() returns nodes whose surface points into `str`, so the
        // surface must be copied out while `str` is still alive.
        const MeCab::Node* node = tagger->parseToNode(str.c_str());
        for (; node; node = node->next) {
            if (node->stat == MECAB_BOS_NODE || node->stat == MECAB_EOS_NODE) {
                continue;
            }
            if (node->surface == nullptr || node->length == 0) {
                continue;
            }
            terms.emplace_back(node->surface, node->length);
        }
    } else {
        terms = StringUtils::split(str, " ", true /* ignoreEmptyStringParts */);
    }
    return terms;
}

} // namespace fts_extension
} // namespace lbug
