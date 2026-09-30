#pragma once

#include "extension/extension.h"

namespace lbug {
namespace main {
class ClientContext;
}

namespace timeseries_extension {

// 类名与扩展目录名一致(TimeseriesExtension),与生成器(扩展名 + Extension)对齐
class TimeseriesExtension final : public extension::Extension {
public:
    static constexpr char EXTENSION_NAME[] = "timeseries";
    static void load(main::ClientContext* context);
};

} // namespace timeseries_extension
} // namespace lbug
