#include "function/tokenize.h"

#include <format>

#include "binder/expression/expression_util.h"
#include "common/exception/binder.h"
#include "common/string_utils.h"
#include "common/types/string_t.h"
#include "common/types/types.h"
#include "common/vector/value_vector.h"
#include "cppjieba/Jieba.hpp"
#include "expression_evaluator/expression_evaluator_utils.h"
#include "function/scalar_function.h"
#include "mecab.h"
#include "re2.h"
#include "utils/dict_dir_resolver.h"

namespace lbug {
namespace fts_extension {

using namespace function;
using namespace common;

struct JiebaBindData final : public FunctionBindData {
    std::shared_ptr<cppjieba::Jieba> jieba;

    JiebaBindData(common::logical_type_vec_t paramTypes, std::shared_ptr<cppjieba::Jieba> jieba)
        : FunctionBindData{std::move(paramTypes),
              common::LogicalType::LIST(common::LogicalType::STRING())},
          jieba{std::move(jieba)} {}

    std::unique_ptr<FunctionBindData> copy() const override {
        return std::make_unique<JiebaBindData>(copyVector(paramTypes), jieba);
    }
};

static void addTokensToVector(const std::vector<std::string>& tokens, list_entry_t& result,
    common::ValueVector& resultVector) {
    result = ListVector::addList(&resultVector, tokens.size());
    for (auto i = 0u; i < tokens.size(); i++) {
        ListVector::getDataVector(&resultVector)->setValue(result.offset + i, tokens[i]);
    }
}

struct JiebaTokenizer {
    static void operation(string_t& text, string_t& /*tokenizerName*/, string_t& /*extraParam*/,
        list_entry_t& result, common::ValueVector& resultVector, void* dataPtr) {
        std::vector<std::string> tokens;
        auto bindData = reinterpret_cast<JiebaBindData*>(dataPtr);
        bindData->jieba->CutForSearch(text.getAsString(), tokens);
        addTokensToVector(tokens, result, resultVector);
    }
};

struct SimpleTokenizer {
    static void operation(string_t& text, string_t& /*tokenizerName*/, string_t& /*extraParam*/,
        list_entry_t& result, common::ValueVector& resultVector, void* /*dataPtr*/) {
        auto tokens =
            StringUtils::split(text.getAsString(), " ", true /* ignoreEmptyStringParts */);
        addTokensToVector(tokens, result, resultVector);
    }
};

struct MeCabBindData final : public FunctionBindData {
    std::shared_ptr<MeCab::Tagger> tagger;

    MeCabBindData(common::logical_type_vec_t paramTypes, std::shared_ptr<MeCab::Tagger> tagger)
        : FunctionBindData{std::move(paramTypes),
              common::LogicalType::LIST(common::LogicalType::STRING())},
          tagger{std::move(tagger)} {}

    std::unique_ptr<FunctionBindData> copy() const override {
        return std::make_unique<MeCabBindData>(copyVector(paramTypes), tagger);
    }
};

struct MeCabTokenizer {
    static void operation(string_t& text, string_t& /*tokenizerName*/, string_t& /*extraParam*/,
        list_entry_t& result, common::ValueVector& resultVector, void* dataPtr) {
        auto bindData = reinterpret_cast<MeCabBindData*>(dataPtr);
        // parseToNode() returns nodes whose surface points into the input
        // string, so keep a named copy alive while iterating the nodes.
        auto str = text.getAsString();
        std::vector<std::string> tokens;
        const MeCab::Node* node = bindData->tagger->parseToNode(str.c_str());
        for (; node; node = node->next) {
            if (node->stat == MECAB_BOS_NODE || node->stat == MECAB_EOS_NODE) {
                continue;
            }
            if (node->surface == nullptr || node->length == 0) {
                continue;
            }
            tokens.emplace_back(node->surface, node->length);
        }
        addTokensToVector(tokens, result, resultVector);
    }
};

static std::unique_ptr<FunctionBindData> bindFunc(const ScalarBindFuncInput& input) {
    if (input.arguments[1]->expressionType != ExpressionType::LITERAL) {
        throw BinderException{"The tokenizer parameter must be a literal expression."};
    }
    if (input.arguments[2]->expressionType != ExpressionType::LITERAL) {
        throw BinderException{"The path to the jieba dict directory must be a literal expression."};
    }
    auto value = evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(input.arguments[1],
        input.context);
    auto tokenizer = common::StringUtils::getLower(value.getValue<std::string>());
    if (tokenizer == "jieba") {
        std::string dictDir = evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(
            input.arguments[2], input.context)
                                  .getValue<std::string>();
        std::string dict = dictDir + "/jieba.dict.utf8";
        std::string hmm = dictDir + "/hmm_model.utf8";
        std::string user = dictDir + "/user.dict.utf8"; // Contains custom AI/ML terms
        std::string idf = dictDir + "/idf.utf8";
        std::string stop = dictDir + "/stop_words.utf8";
        auto jieba = std::make_unique<cppjieba::Jieba>(dict, hmm, user, idf, stop);
        input.definition->ptrCast<ScalarFunction>()->execFunc =
            ScalarFunction::TernaryRegexExecFunction<string_t, string_t, string_t, list_entry_t,
                JiebaTokenizer>;
        return std::make_unique<JiebaBindData>(
            binder::ExpressionUtil::getDataTypes(input.arguments), std::move(jieba));
    } else if (tokenizer == "mecab") {
        std::string dictDir = evaluator::ExpressionEvaluatorUtils::evaluateConstantExpression(
            input.arguments[2], input.context)
                                  .getValue<std::string>();
        // Same cross-machine fallback as the write path (FTSUtils::tokenizeString):
        // a stale machine-specific path from the catalog is retried against the
        // extension-adjacent dictionary before giving up.
        dictDir = resolveDictDir(dictDir, DictKind::MECAB);
        // Pass the rc file explicitly. Without -r, MeCab falls back to
        // MECAB_DEFAULT_RC, a build-time absolute path that does not exist in
        // deployed environments — the tagger then fails to create.
        std::shared_ptr<MeCab::Tagger> tagger(
            MeCab::createTagger((std::string("-d ") + dictDir + " -r " + dictDir + "/mecabrc").c_str()),
            MeCab::deleteTagger);
        if (!tagger) {
            auto lastError = MeCab::getLastError();
            throw common::BinderException{std::format(
                "Failed to create mecab tagger with dict dir: '{}'. (mecab error: {})", dictDir,
                lastError ? lastError : "unknown")};
        }
        input.definition->ptrCast<ScalarFunction>()->execFunc =
            ScalarFunction::TernaryRegexExecFunction<string_t, string_t, string_t, list_entry_t,
                MeCabTokenizer>;
        return std::make_unique<MeCabBindData>(
            binder::ExpressionUtil::getDataTypes(input.arguments), std::move(tagger));
    } else if (tokenizer == "simple" || tokenizer == "") {
        input.definition->ptrCast<ScalarFunction>()->execFunc =
            ScalarFunction::TernaryRegexExecFunction<string_t, string_t, string_t, list_entry_t,
                SimpleTokenizer>;
        return FunctionBindData::getSimpleBindData(input.arguments,
            LogicalType::LIST(LogicalType::STRING()));
    } else {
        throw common::BinderException{
            "Unsupported tokenizer: " + tokenizer +
            ".\nSupported tokenizers: 'simple' (default), 'jieba' (advanced Chinese), 'mecab' "
            "(Japanese)"};
    }
}

function::function_set TokenizeFunction::getFunctionSet() {
    function_set result;
    auto function = std::make_unique<ScalarFunction>(name,
        std::vector<LogicalTypeID>{LogicalTypeID::STRING, LogicalTypeID::STRING,
            LogicalTypeID::STRING},
        LogicalTypeID::LIST);
    function->bindFunc = bindFunc;
    result.push_back(std::move(function));
    return result;
}

} // namespace fts_extension
} // namespace lbug
