#include "capture/RawCapture.h"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace asb::capture {
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t o) {
    if (o > b.size() || b.size()-o < 2) throw std::out_of_range("u16");
    return static_cast<std::uint16_t>(b[o] | (b[o+1] << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t o) {
    return u16(b,o) | (static_cast<std::uint32_t>(u16(b,o+2)) << 16);
}
std::uint64_t u64(std::span<const std::uint8_t> b, std::size_t o) {
    return u32(b,o) | (static_cast<std::uint64_t>(u32(b,o+4)) << 32);
}
bool decode(std::span<const std::uint8_t> b, RecordView& out, std::string& error) {
    out = {};
    auto reject = [&](const char* why) { error = why; return false; };
    if (b.size() < headerSize || b.size() > maxRecordSize) return reject("record size outside ABI bounds");
    if (!std::equal(b.begin(), b.begin()+4, "ASBR") || u16(b,4) != 1 ||
        u32(b,8) != b.size() || u32(b,12) != headerSize) return reject("unsupported capture ABI/header");
    const auto kind = u16(b,6);
    const auto count = u32(b,56), length = u32(b,60);
    if (kind < 1 || kind > 3 || count > maxPackets ||
        headerSize + static_cast<std::uint64_t>(count)*16 + length != b.size())
        return reject("unknown kind or malformed capture lengths");
    out.kind = static_cast<Kind>(kind);
    out.sequence = u64(b,16); out.generation = u64(b,24); out.timestampNs = u64(b,32);
    out.endpoint = u32(b,40); out.direction = u32(b,44);
    out.source = u32(b,68); out.flags = u32(b,72);
    if (!out.sequence || out.flags != 1 || u32(b,76) != 0 || u32(b,64) != 0)
        return reject("unsupported sequence/status semantics");
    out.payload = b.subspan(headerSize + count*16, length);
    if (out.kind == Kind::Event && (count || length != 8 || out.source != 4 ||
        u32(out.payload,0) < 1 || u32(out.payload,0) > 6)) return reject("invalid stream event");
    if (out.kind == Kind::Hid && (count || out.direction != 0 ||
        !((out.source == 2 && out.endpoint == 3) ||
          (out.source == 3 && out.endpoint == 0 && length >= 8)))) return reject("invalid HID source");
    if (out.kind == Kind::Audio && (u32(b,48) != 48000 || u16(b,52) != 4 ||
        u16(b,54) != 16 || out.endpoint != 1 || out.direction != 0 || out.source != 1))
        return reject("unsupported PCM format/source");
    out.validPcm = out.kind == Kind::Audio && count > 0;
    std::uint64_t end = 0;
    for (std::uint32_t i=0; i<count; ++i) {
        const auto o = headerSize + i*16;
        Packet p{u32(b,o), u32(b,o+4), u32(b,o+8), std::bit_cast<std::int32_t>(u32(b,o+12))};
        // This ABI intercepts USB/IP OUT submission. requested is authoritative
        // for analysis; actual is retained, NOT invented from completion.
        if (p.status != 0 || p.actual > p.requested || p.offset != end ||
            p.requested % 8 || p.offset % 8 ||
            static_cast<std::uint64_t>(p.offset)+p.requested > length) out.validPcm = false;
        end = static_cast<std::uint64_t>(p.offset)+p.requested;
        out.packets.push_back(p);
    }
    // Trailing URB padding is retained but not interpreted as samples.
    error.clear();
    return true;
}
std::string jsonString(const std::string& text) {
    constexpr char hex[] = "0123456789abcdef";
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c>>4]; out += hex[c&15]; }
        else out += static_cast<char>(c);
    }
    return out + '"';
}
} // namespace asb::capture
