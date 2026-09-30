#pragma once

#include "extension/extension.h"

namespace lbug {
namespace hyperalgo_extension {

class HyperalgoExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "HYPERALGO";

public:
    static void load(main::ClientContext* context);
};

} // namespace hyperalgo_extension
} // namespace lbug
