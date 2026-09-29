// SPDX-FileCopyrightText: 2026 Mikalai Kaliaha
// SPDX-License-Identifier: MIT
#include "apex6/Gpa6.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>

namespace asb::apex6 {
namespace {
std::uint16_t le16(std::span<const std::uint8_t> p, std::size_t i) {
    return static_cast<std::uint16_t>(p[i] | (p[i + 1] << 8));
}
std::uint32_t le32(std::span<const std::uint8_t> p, std::size_t i) {
    return static_cast<std::uint32_t>(le16(p, i)) |
        (static_cast<std::uint32_t>(le16(p, i + 2)) << 16);
}
bool ramId(std::uint8_t id) { return id == 1 || id == 4 || id == 5 || id == 6; }
std::uint32_t crc32(std::span<const std::uint8_t> data) {
    std::uint32_t crc = 0xffffffffu;
    for (auto byte : data) {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    return ~crc;
}
}
EnvelopeError::EnvelopeError(std::uint8_t cmd, std::uint8_t value)
    : ProtocolError("GPA6 zero-count envelope error " + std::to_string(value)), command(cmd), code(value) {}
ModeStatusError::ModeStatusError(std::uint8_t value)
    : ProtocolError("motor configuration refused: status " + std::to_string(value)), code(value) {}
Frame frame(std::uint8_t command, std::span<const std::uint8_t> payload) {
    if (payload.size() > 27) throw ProtocolError("GPA6 payload exceeds 27 bytes");
    Frame result{};
    result[0] = 0x5a; result[1] = 0xa5; result[2] = command;
    result[3] = static_cast<std::uint8_t>(payload.size() + 2);
    std::copy(payload.begin(), payload.end(), result.begin() + 4);
    result[31] = static_cast<std::uint8_t>(std::accumulate(result.begin() + 2, result.begin() + 31, 0u));
    return result;
}
std::optional<Bytes> replyPayload(std::span<const std::uint8_t> body,
    std::uint8_t command, std::size_t payloadSize, bool v21) {
    if (body.size() < 4 || body[0] != 0x5a || body[1] != 0xa5 || body[2] != command)
        return std::nullopt;
    // GPA6 bodies supported by this port are at most 64 bytes. Reject, never
    // trim unexpected trailing data or silently accept a corrupt ACK.
    if (body.size() > 64 || payloadSize > 59) throw ProtocolError("oversized GPA6 reply");
    const std::size_t start = v21 ? 3 : 5;
    const bool error = !v21 && body[3] == 0;
    const auto end = start + (error ? 1 : payloadSize);
    for (auto check : {end, std::size_t{31}, body.size() - 1}) {
        if (check < end || check >= body.size()) continue;
        if (std::any_of(body.begin() + end, body.begin() + check, [](auto v) { return v != 0; }) ||
            std::any_of(body.begin() + check + 1, body.end(), [](auto v) { return v != 0; })) continue;
        const auto sum = std::accumulate(body.begin() + 2, body.begin() + check, 0u);
        if (static_cast<std::uint8_t>(sum) != body[check]) continue;
        if (error) throw EnvelopeError(command, body[start]);
        if (!v21 && (body[3] != 1 || body[4] != 0)) throw ProtocolError("unexpected GPA6 packet count/index");
        return Bytes(body.begin() + start, body.begin() + end);
    }
    throw ProtocolError("malformed GPA6 reply");
}
void requireModeSuccess(std::span<const std::uint8_t> payload) {
    if (payload.size() != 1) throw ProtocolError("invalid mode status size");
    if (payload[0]) throw ModeStatusError(payload[0]);
}
ModeReply classifyModeReply(std::span<const std::uint8_t> body) {
    Frame captured{};captured[0]=0x5a;captured[1]=0xa5;captured[2]=0x53;captured[5]=1;captured[31]=0x54;
    if(body.size()==captured.size()&&std::equal(body.begin(),body.end(),captured.begin()))return ModeReply::CapturedZeroCountValue1;
    try {auto p=replyPayload(body,0x53,1);if(p){requireModeSuccess(*p);return ModeReply::NormalSuccessAck;}}catch(const ProtocolError&){}
    return ModeReply::InvalidUnexpected;
}
const char* modeReplyName(ModeReply r) {
    switch(r) {
    case ModeReply::NotObserved:return "not_observed";
    case ModeReply::NormalSuccessAck:return "normal_success_ack";
    case ModeReply::CapturedZeroCountValue1:return "captured_zero_count_value_1_unverified";
    default:return "invalid_unexpected";
    }
}
std::uint8_t quantize(double sample) {
    if (!std::isfinite(sample)) throw ProtocolError("waveform sample must be finite");
    // Force the same binary32 rounding points as the native/Python encoder.
    const float x = static_cast<float>(std::clamp(sample, -1.0, 1.0));
    const float shifted = static_cast<float>(static_cast<double>(x) + 1.0);
    const float scaled = static_cast<float>(static_cast<double>(shifted) * 127.5);
    return static_cast<std::uint8_t>(std::clamp(std::floor(static_cast<double>(scaled) + .5), 0.0, 255.0));
}
Frame waveform(const std::array<std::vector<double>, 3>& columns,
    std::array<bool, 3> enabled, std::uint8_t selector) {
    if (selector > 3) throw ProtocolError("invalid waveform selector");
    std::array<std::uint8_t, 25> payload{};
    payload.fill(128);
    payload[0] = static_cast<std::uint8_t>(0x80 | selector |
        (enabled[0] ? 4 : 0) | (enabled[1] ? 8 : 0) | (enabled[2] ? 16 : 0));
    for (std::size_t i = 0; i < 8; ++i)
        for (std::size_t c = 0; c < 3; ++c) {
            const auto offset = columns[c].size() > 8 ? columns[c].size() - 8 : 0;
            if (enabled[c] && offset + i < columns[c].size())
                payload[1 + i * 3 + c] = quantize(columns[c][offset + i]);
        }
    return frame(0x57, payload);
}
Frame gripWaveform(std::span<const std::array<double, 2>> samples) {
    if (samples.size() > 8) throw ProtocolError("grip packet must contain at most eight frames");
    std::array<std::vector<double>, 3> columns;
    for (const auto& sample : samples) {
        columns[1].push_back(sample[0]); columns[2].push_back(sample[1]);
    }
    return waveform(columns);
}
ConfigState configState(std::span<const std::uint8_t> p) {
    if (p.size() != 11 || p[0] > 3) throw ProtocolError("invalid active configuration state");
    ConfigState result{p[0], {}};
    for (std::size_t i = 0; i < 5; ++i) result.crc16[i] = le16(p, 1 + 2 * i);
    return result;
}
RamFingerprint ramInfo(std::uint8_t id, std::span<const std::uint8_t> p) {
    if (!ramId(id) || p.size() != 10 || p[0] != 1 || p[1] != 0 || p[2] != id || p[3] > 3)
        throw ProtocolError("RAM query status, ID or slot mismatch");
    RamFingerprint result{id, p[3], le16(p, 4), le32(p, 6)};
    if (result.length > 4096) throw ProtocolError("RAM exceeds bounded read size");
    return result;
}
Bytes assembleRam(const RamFingerprint& before, const RamFingerprint& after,
    std::span<const Bytes> chunks) {
    if (before != after || !ramId(before.id) || before.slot > 3 || before.length > 4096)
        throw ProtocolError("invalid or changed RAM fingerprint");
    if (chunks.size() != (static_cast<std::size_t>(before.length) + 15) / 16)
        throw ProtocolError("RAM chunk count mismatch");
    Bytes result;
    result.reserve(before.length);
    for (std::size_t i = 0; i < chunks.size(); ++i) {
        const auto& p = chunks[i];
        const auto count = std::min<std::size_t>(16, before.length - result.size());
        if (p.size() < 8 || p[0] != 0 || p[1] != 0 || p[2] != before.id || p[3] != before.slot ||
            le16(p, 4) != before.length || p[6] != i || (p[7] != count && p[7] != 16) || p.size() != 8u + p[7])
            throw ProtocolError("RAM chunk status, ID, slot, length or index mismatch");
        result.insert(result.end(), p.begin() + 8, p.begin() + 8 + count);
    }
    if (crc32(result) != before.crc32) throw ProtocolError("RAM CRC32 mismatch");
    return result;
}
std::array<GripRestore, 2> gripRestore(std::span<const std::uint8_t> mapping) {
    if (mapping.size() != 64 || mapping[0] != 2) throw ProtocolError("unsupported motor mapping layout");
    std::array<GripRestore, 2> result{};
    for (std::size_t i = 0; i < 2; ++i) {
        const auto mode = mapping[23 + i * 2];
        if (mode > 1) throw ProtocolError("unsupported persistent grip mode");
        result[i] = {static_cast<std::uint8_t>(0x10 + i), mode, {}};
        if (mode) result[i].parameters.push_back(mapping[24 + i * 2]);
    }
    return result;
}
}
