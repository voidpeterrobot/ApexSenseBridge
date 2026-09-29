// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

// Pure offline codecs. No device ownership, HID wrappers or transmit API.
namespace asb::apex6 {
using Bytes = std::vector<std::uint8_t>;
using Frame = std::array<std::uint8_t, 32>;
struct ProtocolError : std::runtime_error { using std::runtime_error::runtime_error; };
struct EnvelopeError : ProtocolError {
    std::uint8_t command, code;
    EnvelopeError(std::uint8_t command, std::uint8_t code);
};
struct ModeStatusError : ProtocolError {
    std::uint8_t code;
    explicit ModeStatusError(std::uint8_t code);
};

Frame frame(std::uint8_t command, std::span<const std::uint8_t> payload = {});
// Body excludes any HID report-ID byte. Matching malformed replies throw;
// unrelated frames return nullopt. A match does NOT prove request attribution.
std::optional<Bytes> replyPayload(std::span<const std::uint8_t> body,
    std::uint8_t command, std::size_t payloadSize, bool v21 = false);
void requireModeSuccess(std::span<const std::uint8_t> payload);
enum class ModeReply { NotObserved, NormalSuccessAck, CapturedZeroCountValue1, InvalidUnexpected };
// USB body only: exactly 32 bytes for the captured anomaly. Windows callers
// must validate and remove their declared report ID with Layout::unwrap first.
ModeReply classifyModeReply(std::span<const std::uint8_t> body);
const char* modeReplyName(ModeReply);
std::uint8_t quantize(double sample);
// General encoder retained solely for comparison with native fixtures.
// The offline PCM renderer uses gripWaveform: selector 0, no trigger column.
Frame waveform(const std::array<std::vector<double>, 3>& columns,
    std::array<bool, 3> enabled = {false, true, true}, std::uint8_t selector = 0);
Frame gripWaveform(std::span<const std::array<double, 2>> samples);

struct ConfigState {
    std::uint8_t slot;
    std::array<std::uint16_t, 5> crc16;
    bool operator==(const ConfigState&) const = default;
};
ConfigState configState(std::span<const std::uint8_t> payload);
struct RamFingerprint {
    std::uint8_t id, slot;
    std::uint16_t length;
    std::uint32_t crc32;
    bool operator==(const RamFingerprint&) const = default;
};
RamFingerprint ramInfo(std::uint8_t id, std::span<const std::uint8_t> payload);
// Accepts already-decoded V21 payloads; checks every echo and final CRC.
Bytes assembleRam(const RamFingerprint& before, const RamFingerprint& after,
    std::span<const Bytes> chunks);
struct GripRestore { std::uint8_t target, mode; Bytes parameters; };
std::array<GripRestore, 2> gripRestore(std::span<const std::uint8_t> mapping);
}
