#include "apex6/GripDsp.h"
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <string>

namespace {
using namespace asb::apex6;
std::uint32_t le(std::span<const std::uint8_t> bytes, std::size_t offset, unsigned count) {
    if (offset > bytes.size() || count > bytes.size() - offset) throw ProtocolError("truncated WAV field");
    std::uint32_t value = 0;
    for (unsigned i = 0; i < count; ++i) value |= std::uint32_t(bytes[offset + i]) << (8 * i);
    return value;
}
bool tag(std::span<const std::uint8_t> bytes, std::size_t offset, const char* name) {
    return offset <= bytes.size() && bytes.size() - offset >= 4 && std::equal(name, name + 4, bytes.begin() + offset);
}
std::span<const std::uint8_t> pcmData(const Bytes& bytes, unsigned& channels) {
    if (bytes.size() < 12 || !tag(bytes, 0, "RIFF") || !tag(bytes, 8, "WAVE") || le(bytes, 4, 4) != bytes.size() - 8)
        throw ProtocolError("expected exact RIFF/WAVE file");
    bool format = false, data = false;
    std::span<const std::uint8_t> pcm;
    for (std::size_t offset = 12; offset < bytes.size();) {
        if (bytes.size() - offset < 8) throw ProtocolError("truncated WAV chunk");
        const auto size = le(bytes, offset + 4, 4);
        const auto start = offset + 8;
        if (size > bytes.size() - start) throw ProtocolError("truncated WAV chunk data");
        if (tag(bytes, offset, "fmt ")) {
            if (format || size != 16) throw ProtocolError("expected one canonical PCM fmt chunk");
            channels = le(bytes, start + 2, 2);
            if (le(bytes, start, 2) != 1 || (channels != 2 && channels != 4) ||
                le(bytes, start + 4, 4) != 48000 || le(bytes, start + 8, 4) != 48000 * channels * 2 ||
                le(bytes, start + 12, 2) != channels * 2 || le(bytes, start + 14, 2) != 16)
                throw ProtocolError("requires PCM s16le, 48 kHz, 2 or 4 channels");
            format = true;
        } else if (tag(bytes, offset, "data")) {
            if (data) throw ProtocolError("multiple WAV data chunks");
            pcm = std::span(bytes).subspan(start, size); data = true;
        }
        offset = start + size;
        if (size & 1) {
            if (offset == bytes.size()) throw ProtocolError("missing WAV chunk padding");
            ++offset;
        }
    }
    if (!format || !data || pcm.empty() || pcm.size() % (channels * 2 * 48))
        throw ProtocolError("WAV must contain whole 1-ms PCM blocks");
    return pcm;
}
int run(const std::filesystem::path& path, double gain, double peakLimit) {
    // No device paths, handles, drivers, endpoint enumeration or playback.
    if (!std::filesystem::is_regular_file(path)) throw ProtocolError("input must be a regular WAV file");
    const auto size = std::filesystem::file_size(path);
    if (size > 64 * 1024 * 1024) throw ProtocolError("WAV exceeds 64 MiB bound");
    std::ifstream file(path, std::ios::binary);
    Bytes bytes(static_cast<std::size_t>(size));
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())) || file.peek() != EOF)
        throw ProtocolError("WAV read failed or file size changed");
    unsigned channels = 0;
    auto pcm = pcmData(bytes, channels);
    StereoResampler dsp(channels, gain, peakLimit); // software settings, not physical authorization
    std::vector<StereoSample> samples;
    while (!pcm.empty()) {
        const auto count = std::min<std::size_t>(pcm.size(), 32768);
        auto part = dsp.feed(pcm.first(count));
        samples.insert(samples.end(), part.begin(), part.end()); pcm = pcm.subspan(count);
    }
    dsp.finish();
    std::cout << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "{\"schema\":\"asb.apex6.offline.v1\",\"physical_output\":false,\"channels\":" << channels
        << ",\"gain\":" << gain << ",\"peak_limit\":" << peakLimit
        << ",\"strength_policy\":\"fixed_gain_then_symmetric_clip\",\"fir_tail_flushed\":false}\n";
    const char* hex = "0123456789abcdef";
    for (std::size_t offset = 0; offset < samples.size(); offset += 8) {
        const auto count = std::min<std::size_t>(8, samples.size() - offset);
        const auto packet = gripWaveform(std::span(samples).subspan(offset, count));
        std::string encoded;
        for (auto byte : packet) { encoded += hex[byte >> 4]; encoded += hex[byte & 15]; }
        std::cout << "{\"packet\":" << offset / 8 << ",\"nominal_sample_offset\":" << offset
            << ",\"neutral_padding_frames\":" << 8 - count << ",\"private_frame_hex\":\"" << encoded << "\"}\n";
    }
    std::cout << "{\"complete\":true,\"input_frames\":" << dsp.metrics().inputFrames
        << ",\"output_frames\":" << dsp.metrics().outputFrames << ",\"overrange_samples\":" << dsp.metrics().overrangeSamples
        << ",\"clipped_samples\":" << dsp.metrics().clippedSamples
        << ",\"peak_before_limit\":" << dsp.metrics().peakBeforeLimit
        << ",\"peak_after_limit\":" << dsp.metrics().peakAfterLimit
        << ",\"neutral_padding_frames\":" << (8 - samples.size() % 8) % 8 << "}\n";
    if (!std::cout) throw ProtocolError("offline preview output failed");
    return 0;
}
}
#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    try {
        if (argc < 3 || std::filesystem::path(argv[1]) != "render-wav") {
            std::cerr << "Offline only. Usage: ApexSenseBridgeHapticOffline render-wav INPUT.wav [--gain 0..12] [--peak-limit 0..1]\n"
                         "Writes packet preview JSONL to stdout; never transmits or plays audio.\n";
            return 2;
        }
        double gain = 1, peakLimit = 1;
        bool haveGain = false, haveLimit = false;
        for (int i = 3; i < argc; i += 2) {
            if (i + 1 == argc) throw ProtocolError("missing strength option value");
            const auto option = std::filesystem::path(argv[i]).string();
            const auto text = std::filesystem::path(argv[i + 1]).string();
            double value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw ProtocolError("invalid strength value");
            if (option == "--gain" && !haveGain) { gain = value; haveGain = true; }
            else if (option == "--peak-limit" && !haveLimit) { peakLimit = value; haveLimit = true; }
            else throw ProtocolError("unknown or duplicate strength option");
        }
        return run(std::filesystem::path(argv[2]), gain, peakLimit);
    } catch (const std::exception& error) {
        std::cerr << "Offline render refused: " << error.what() << '\n'; return 1;
    }
}
