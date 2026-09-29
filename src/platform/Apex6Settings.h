#pragma once
#include <cmath>
#include <optional>
#include <string>

namespace asb::platform {
inline constexpr unsigned kApex6ConsentVersion = 1;
struct Apex6Settings { unsigned consentVersion=0; double gain=1; };
inline bool validGripGain(double value) noexcept {
    return std::isfinite(value) && value >= 0 && value <= 12;
}
Apex6Settings readApex6Settings();
void updateApex6Settings(std::optional<unsigned> consent,std::optional<double> gain);
}
