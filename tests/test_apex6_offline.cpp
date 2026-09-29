#include "apex6/GripDsp.h"
#include "fixtures/apex6/ReferenceVectors.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>

using namespace asb::apex6;
namespace {
unsigned checks = 0;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class E = ProtocolError, class F> void rejects(F operation) {
    bool caught = false;
    try { operation(); } catch (const E&) { caught = true; }
    check(caught, "expected refusal");
}
Bytes unhex(const std::string& text) {
    Bytes result;
    for (std::size_t i = 0; i < text.size(); i += 2)
        result.push_back(static_cast<std::uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
    return result;
}
void sameFrame(const Frame& frame, const char* text) {
    auto expected = unhex(text);
    check(std::equal(frame.begin(), frame.end(), expected.begin(), expected.end()), "frame differs from supplied golden");
}
Bytes reply(std::uint8_t cmd, const Bytes& payload, bool v21 = false, std::size_t checksum = 31) {
    const std::size_t start = v21 ? 3 : 5;
    Bytes bytes(std::max(checksum + 1, start + payload.size() + 1));
    bytes[0] = 0x5a; bytes[1] = 0xa5; bytes[2] = cmd;
    if (!v21) bytes[3] = 1;
    std::copy(payload.begin(), payload.end(), bytes.begin() + start);
    for (std::size_t i = 2; i < checksum; ++i) bytes[checksum] += bytes[i];
    return bytes;
}
void codecs() {
    for (auto q : vectors::quantizers) check(quantize(q.input) == q.expected, "native quantizer fixture");
    for (const auto& p : vectors::packets) sameFrame(waveform(p.columns, p.enabled, p.selector), p.hex);
    for (double value : {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
        rejects([&] { quantize(value); });
    rejects([] { waveform({}, {false, true, true}, 4); });
    rejects([] { frame(1, Bytes(28)); });
    rejects([] { gripWaveform(std::vector<StereoSample>(9)); });
    sameFrame(frame(0x53, unhex("0112024000")), "5aa55307011202400000000000000000000000000000000000000000000000af");
    sameFrame(frame(0x53, unhex("011200")), "5aa553050112000000000000000000000000000000000000000000000000006b");
    sameFrame(gripWaveform({}), "5aa5571b9880808080808080808080808080808080808080808080808000000a");
    sameFrame(waveform({}, {false,false,false}), "5aa5571b808080808080808080808080808080808080808080808080800000f2");
    const std::array<StereoSample, 2> stereo = {{{-1, .5}, {1, 0}}};
    const auto packet = gripWaveform(stereo);
    check(packet[4] == 0x98 && packet[5] == 128 && packet[6] == 0 && packet[7] == 191, "grip-only routing");
    for (unsigned i = 2; i < 8; ++i)
        check(packet[5 + 3 * i] == 128 && packet[6 + 3 * i] == 128 && packet[7 + 3 * i] == 128, "neutral padding");

    for (auto pos : {6u, 31u, 63u}) {
        auto body = reply(0x53, {0}, false, pos);
        check(replyPayload(body, 0x53, 1).value() == Bytes{0}, "compact/padded ACK");
        body[pos] ^= 1;
        rejects([&] { replyPayload(body, 0x53, 1); });
    }
    check(!replyPayload(unhex("5aa50401000005"), 0x53, 1), "unrelated reply must not match");
    rejects<EnvelopeError>([] { replyPayload(unhex("5aa55300000154"), 0x53, 1); });
    auto status = replyPayload(unhex("5aa55301000155"), 0x53, 1).value();
    rejects<ModeStatusError>([&] { requireModeSuccess(status); });
    requireModeSuccess(Bytes{0});
    rejects([] { requireModeSuccess(Bytes{}); });
    rejects([] { replyPayload(unhex("5aa55302000055"), 0x53, 1); });
    auto padded = reply(0x53, {0}); padded.push_back(1);
    rejects([&] { replyPayload(padded, 0x53, 1); });
    const auto compact = unhex("5aa55301000054");
    for (std::size_t length = 4; length < 7; ++length)
        rejects([&] { replyPayload(std::span(compact).first(length), 0x53, 1); });
}
void readback() {
    check(configState(unhex("0065006600670068006900")).slot == 0, "active slot");
    rejects([] { configState(Bytes(10)); });
    auto invalid = Bytes(11); invalid[0] = 4;
    rejects([&] { configState(invalid); });
    for (const auto& golden : vectors::ram) {
        const auto bytes = unhex(golden.hex);
        Bytes info{1, 0, static_cast<std::uint8_t>(golden.id), 0,
            static_cast<std::uint8_t>(golden.length), static_cast<std::uint8_t>(golden.length >> 8)};
        for (unsigned i = 0; i < 4; ++i) info.push_back(static_cast<std::uint8_t>(golden.crc >> (8 * i)));
        const auto fingerprint = ramInfo(static_cast<std::uint8_t>(golden.id), info);
        auto envelope = reply(0xa3, info, true);
        check(replyPayload(envelope, 0xa3, 10, true).value() == info, "V21 envelope");
        std::vector<Bytes> chunks;
        for (std::size_t offset = 0; offset < bytes.size(); offset += 16) {
            const auto count = std::min<std::size_t>(16, bytes.size() - offset);
            Bytes chunk{0,0,static_cast<std::uint8_t>(golden.id),0,info[4],info[5],
                static_cast<std::uint8_t>(offset / 16),static_cast<std::uint8_t>(count)};
            chunk.insert(chunk.end(), bytes.begin() + offset, bytes.begin() + offset + count);
            chunks.push_back(chunk);
        }
        check(assembleRam(fingerprint, fingerprint, chunks) == bytes, "synthetic RAM CRC and bytes");
        auto changed = fingerprint; changed.slot = 1;
        rejects([&] { assembleRam(fingerprint, changed, chunks); });
        for (unsigned field = 0; field < 8; ++field) {
            auto corrupt = chunks; corrupt[0][field] ^= 1;
            rejects([&] { assembleRam(fingerprint, fingerprint, corrupt); });
        }
        auto corrupt = chunks; corrupt[0][8] ^= 1;
        rejects([&] { assembleRam(fingerprint, fingerprint, corrupt); });
        if (chunks.back()[7] < 16) {
            chunks.back()[7] = 16; chunks.back().resize(24, 0x55);
            check(assembleRam(fingerprint, fingerprint, chunks) == bytes, "padded final RAM chunk");
        }
        if (golden.id == 6) {
            auto restore = gripRestore(bytes);
            check(restore[0].target == 0x10 && restore[1].target == 0x11 && restore[0].parameters == Bytes{0x40}, "synthetic grip map");
            sameFrame(frame(0x53, Bytes{1, restore[0].target, restore[0].mode, restore[0].parameters[0]}),
                "5aa55306011001400000000000000000000000000000000000000000000000ab");
            auto bad = bytes; bad[0] = 3;
            rejects([&] { gripRestore(bad); });
            bad = bytes; bad[23] = 2;
            rejects([&] { gripRestore(bad); });
            bad[23] = 0; check(gripRestore(bad)[0].parameters.empty(), "disabled mode has no parameter");
        }
    }
    rejects([] { ramInfo(2, Bytes(10)); });
    rejects([] { ramInfo(6, unhex("01000600011000000000")); });
    rejects([] { gripRestore(Bytes(63)); });
}
void append16(Bytes& bytes, int value) {
    const auto raw = static_cast<std::uint16_t>(value);
    bytes.push_back(static_cast<std::uint8_t>(raw)); bytes.push_back(static_cast<std::uint8_t>(raw >> 8));
}
Bytes signal(unsigned channels, unsigned frames, double leftHz, double rightHz) {
    Bytes bytes;
    for (unsigned i = 0; i < frames; ++i) {
        if (channels == 4) { append16(bytes, 32767); append16(bytes, -32768); }
        append16(bytes, static_cast<int>(3276 * std::sin(2 * std::numbers::pi * leftHz * i / 48000)));
        append16(bytes, static_cast<int>(3276 * std::sin(2 * std::numbers::pi * rightHz * i / 48000)));
    }
    return bytes;
}
void dsp() {
    GripStrength stage(4,.25);DspMetrics stageMetrics;
    check(stage.apply({.0625,-.03125},stageMetrics)==StereoSample{.25,-.125},"shared pulse strength stage");
    rejects([&]{stage.apply({.1,std::numeric_limits<double>::infinity()},stageMetrics);});
    check(stageMetrics.clippedSamples==0&&stageMetrics.peakBeforeLimit==.25,"invalid sample mutated strength metrics");
    rejects([&]{stage.apply({std::numeric_limits<double>::max(),0},stageMetrics);});
    rejects([] { StereoResampler invalid(3); });
    rejects([] { StereoResampler invalid(4, std::numeric_limits<double>::quiet_NaN()); });
    rejects([] { StereoResampler invalid(2, -1); });
    rejects([] { StereoResampler invalid(2, 12.001); });
    GripStrength maximum(12,1);DspMetrics maximumMetrics;
    check(maximum.apply({.0625,-.0625},maximumMetrics)==StereoSample{.75,-.75},"12x exact scale");
    check(maximumMetrics.clippedSamples==0&&maximumMetrics.overrangeSamples==0,"12x tone is not overrange");
    check(maximum.apply({.125,-.125},maximumMetrics)==StereoSample{1,-1}&&maximumMetrics.clippedSamples==2,"12x overflow bounded");
    for (double limit : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
                         std::numeric_limits<double>::quiet_NaN()})
        rejects([&] { StereoResampler invalid(2, 1, limit); });
    for (auto hz : {80.,100.,120.,200.,300.}) {
        auto pcm = signal(4, 24000, hz, 0);
        StereoResampler whole(4), fragmented(4), stereo(2);
        const auto expected = whole.feed(pcm);
        check(expected.size() == 500, "48:1 output frame count");
        check(expected == stereo.feed(signal(2,24000,hz,0)), "speaker channels excluded");
        std::vector<StereoSample> pieces;
        for (std::size_t offset = 0; offset < pcm.size();) {
            const auto count = std::min<std::size_t>(1 + (offset * 17) % 307, pcm.size() - offset);
            auto part = fragmented.feed(std::span(pcm).subspan(offset, count));
            pieces.insert(pieces.end(), part.begin(), part.end()); offset += count;
        }
        fragmented.finish();
        check(expected == pieces, "byte fragmentation changed FIR phase/history");
        double energy = 0;
        for (std::size_t i = 20; i < expected.size(); ++i) {
            check(expected[i][1] == 0, "inactive-channel leakage");
            energy += expected[i][0] * expected[i][0];
        }
        check(energy > 1.0, "passband signal disappeared");
        fragmented.feed(Bytes{0}); rejects([&] { fragmented.finish(); });
        fragmented.reset(); check(fragmented.pendingBytes() == 0, "reset pending fragment");
        const auto neutral = fragmented.feed(Bytes(48 * 8));
        check(neutral == std::vector<StereoSample>{{0,0}}, "generation reset leaked old history");
    }
    StereoResampler independent(4);
    auto mixed = independent.feed(signal(4, 24000, 80, 120));
    StereoResampler left(2), right(2);
    auto l = left.feed(signal(2, 24000, 80, 0)), r = right.feed(signal(2, 24000, 120, 0));
    for (std::size_t i = 0; i < mixed.size(); ++i)
        check(mixed[i][0] == l[i][0] && mixed[i][1] == r[i][0], "stereo channel independence");
    // Independent steady-state oracle: normalized FIR must preserve signed DC.
    Bytes dc;
    for (unsigned i = 0; i < 960; ++i) { append16(dc,-32768); append16(dc,32767); }
    StereoResampler unity(2), amplified(2,4), muted(2,0);
    auto steady = unity.feed(dc), loud = amplified.feed(dc), silence = muted.feed(dc);
    check(std::abs(steady.back()[0] + 1) < 1e-12, "negative full-scale sign/DC");
    check(std::abs(steady.back()[1] - 32767.0/32768) < 1e-12, "positive full-scale DC");
    check(amplified.metrics().overrangeSamples > 0 && quantize(loud.back()[0]) == 0, "overrange before clamping");
    for (auto s : silence) check(s == StereoSample{0,0}, "zero gain");
    StereoResampler limited(2, 4, .0625), limitMuted(2, 4, 0);
    const auto bounded = limited.feed(dc);
    check(bounded.back() == StereoSample{-.0625,.0625}, "symmetric signed peak limit");
    check(limited.metrics().overrangeSamples > 0 && limited.metrics().clippedSamples > 0,
          "limiting must retain overrange and clipping evidence");
    check(limited.metrics().peakBeforeLimit > 3.9 && limited.metrics().peakAfterLimit == .0625,
          "pre/post peak metrics");
    for (auto s : limitMuted.feed(dc)) check(s == StereoSample{0,0}, "zero peak limit mutes");
    StereoResampler smallLimit(2, 2, .0625), splitLimit(2, 2, .0625);
    auto tone = signal(2, 24000, 125, 0);
    const auto limitedTone = smallLimit.feed(tone);
    std::vector<StereoSample> splitTone;
    for (std::size_t offset = 0; offset < tone.size();) {
        const auto count = std::min<std::size_t>(113, tone.size() - offset);
        auto chunk = splitLimit.feed(std::span(tone).subspan(offset, count));
        splitTone.insert(splitTone.end(), chunk.begin(), chunk.end()); offset += count;
    }
    check(splitTone == limitedTone && splitLimit.metrics().clippedSamples == smallLimit.metrics().clippedSamples,
          "strength and metrics invariant across fragmented input");
    for (auto s : limitedTone) check(std::abs(s[0]) <= .0625 && s[1] == 0, "limit and silent channel preservation");
    StereoResampler unscaled(2), doubled(2, 2);
    auto original = unscaled.feed(tone), stronger = doubled.feed(tone);
    for (std::size_t i = 0; i < original.size(); ++i)
        check(stronger[i][0] == 2 * original[i][0] && stronger[i][1] == 0, "linear gain below limit");
    // A single impulse placed at input 48 samples is centered at output sample
    // 8 (48+384 samples): checks decimation phase and 8-ms FIR group delay.
    Bytes impulse(48 * 20 * 4);
    impulse[47 * 4] = 0xff; impulse[47 * 4 + 1] = 0x7f;
    StereoResampler ir(2);
    const auto response = ir.feed(impulse);
    const auto peak = std::max_element(response.begin(), response.end(), [](auto a, auto b) { return a[0] < b[0]; });
    check(peak - response.begin() == 8, "FIR group delay/decimation phase");
    check(response[8][0] > .014 && response[8][0] < .015, "impulse amplitude");
    StereoResampler stopband(2);
    const auto alias = stopband.feed(signal(2,24000,2000,0));
    for (std::size_t i = 20; i < alias.size(); ++i) check(std::abs(alias[i][0]) < .0001, "anti-alias filter");
    rejects([&] { ir.feed(Bytes(1024*1024+1)); });
}
void scheduling() {
    using Time = PacketScheduler::Time;
    PacketScheduler queue;
    const std::vector<StereoSample> samples(48, StereoSample{.5,-.5});
    queue.enqueue(samples, Time(0));
    check(queue.queued() == 40 && queue.dropped() == 8, "bounded newest queue");
    check(queue.tick(Time(0)).has_value(), "initial packet");
    check(!queue.tick(Time(7999)), "early packet");
    check(queue.tick(Time(8000)).has_value(), "8-ms packet");
    auto stale = queue.tick(Time(100000));
    sameFrame(*stale, "5aa5571b9880808080808080808080808080808080808080808080808000000a");
    check(queue.underruns() == 8 && queue.queued() == 0 && queue.dropped() == 32, "stall drops old samples");
    check(!queue.tick(Time(100000)), "must not burst after stall");
    rejects([&] { queue.tick(Time(99999)); });
    queue.reset(Time(101000));
    queue.enqueue(std::span(samples).first(4), Time(101000));
    queue.tick(Time(101000)); check(queue.underruns() == 12, "partial packet padding");
    rejects([] { PacketScheduler invalid(7); }); rejects([] { PacketScheduler invalid(101); });
    const std::vector<StereoSample> nan = {{std::numeric_limits<double>::quiet_NaN(),0}};
    rejects([&] { queue.enqueue(nan, Time(102000)); });
    PacketScheduler aged;
    aged.enqueue(samples, Time(0));
    auto atDeadline = aged.tick(Time(40001));
    check(aged.queued() == 0 && atDeadline.has_value(), "aged data discarded");
}
}
int main() {
    try {
        codecs(); readback(); dsp(); scheduling();
        std::cout << checks << " offline checks passed; no physical output tested\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
