#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
namespace asb::capture {
std::vector<std::uint8_t> loadFixturePcm(const std::filesystem::path& wav);
// Read-only verification. Requires our canonical finalized manifest, then
// compares the host audio record stream against an exact 4ch/48k/s16 WAV.
bool verifyCapture(const std::filesystem::path& capture, const std::filesystem::path& wav,
                   std::string& report, std::string& error);
}
