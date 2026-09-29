#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace asb::capture {
inline constexpr std::size_t headerSize = 80;
inline constexpr std::size_t maxRecordSize = 1024 * 1024;
inline constexpr std::size_t maxPackets = 4096;
enum class Kind : std::uint16_t { Audio = 1, Hid = 2, Event = 3 };
struct Packet { std::uint32_t offset, requested, actual; std::int32_t status; };
struct RecordView {
    Kind kind{};
    std::uint64_t sequence{}, generation{}, timestampNs{};
    std::uint32_t endpoint{}, direction{}, source{}, flags{};
    std::vector<Packet> packets;
    std::span<const std::uint8_t> payload;
    bool validPcm = false;
};
std::uint16_t u16(std::span<const std::uint8_t> bytes, std::size_t offset);
std::uint32_t u32(std::span<const std::uint8_t> bytes, std::size_t offset);
std::uint64_t u64(std::span<const std::uint8_t> bytes, std::size_t offset);
// Structural/ABI errors return false. Invalid packet evidence returns true with
// validPcm=false: retain the raw record, but never analyze it as valid silence.
bool decode(std::span<const std::uint8_t> bytes, RecordView& out, std::string& error);
std::string jsonString(const std::string& text);
} // namespace asb::capture
