#pragma once
#include "apex6/experiment/Pulse.h"
namespace asb::apex6::experiment {
std::string pulseReviewManifest(const GripBaseline&,const std::string& sourceHash,const std::string& executableHash);
std::string pulseResultJson(const GripPulseResult&);
}
