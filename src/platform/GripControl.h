#pragma once
#include "platform/Apex6Settings.h"
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace asb::platform {
// Separate protocol: existing readiness/status mappings remain byte-compatible.
class GripControl {
public:
    virtual ~GripControl()=default;
    virtual std::optional<double> poll()=0;
};
std::unique_ptr<GripControl> createGripControl(std::string_view token,double initial);
}
