#include "hyperalgo_extension.h"

#include "hyper_kitting_bool.h"
#include "hyperalgo_functions.h"
#include "hyper_project.h"
#include "main/client_context.h"

namespace lbug {
namespace hyperalgo_extension {

using namespace extension;

void HyperalgoExtension::load(main::ClientContext* context) {
    auto& db = *context->getDatabase();
    // 构造层:hyper_project(FRZ-10;P-1.5 §3.1)
    ExtensionUtils::addTableFunc<HyperProjectFunction>(db);
    // 分析层:P0~P2 七项算法(P-1.5 §3.2)
    ExtensionUtils::addTableFunc<HyperSCCFunction>(db);
    ExtensionUtils::addTableFunc<HyperKittingCCFunction>(db);
    ExtensionUtils::addTableFunc<HyperHitSetFunction>(db);
    ExtensionUtils::addTableFunc<HyperKSCoreFunction>(db);
    ExtensionUtils::addTableFunc<HyperBCyclesFunction>(db);
    ExtensionUtils::addTableFunc<HyperPRWalkFunction>(db);
    ExtensionUtils::addTableFunc<HyperFiedlerFunction>(db);
    // 判定层 v0:P3a-v0(FRZ-11 真值表;扩展内实现,与 Cypher 金本互证)
    ExtensionUtils::addTableFunc<HyperKittingBoolFunction>(db);
}

} // namespace hyperalgo_extension
} // namespace lbug

#if defined(BUILD_DYNAMIC_LOAD)
extern "C" {
#if defined(_WIN32)
#define INIT_EXPORT __declspec(dllexport)
#else
#define INIT_EXPORT __attribute__((visibility("default")))
#endif
INIT_EXPORT void init(lbug::main::ClientContext* context) {
    lbug::hyperalgo_extension::HyperalgoExtension::load(context);
}

INIT_EXPORT const char* name() {
    return lbug::hyperalgo_extension::HyperalgoExtension::EXTENSION_NAME;
}
}
#endif
