#include "capture/CaptureVerifier.h"
#include "capture/FeedbackRecorder.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace asb::capture {
namespace {
std::vector<std::uint8_t> readSmall(const std::filesystem::path& path,std::size_t max) {
    const auto size=std::filesystem::file_size(path);
    if(size>max) throw std::runtime_error("verification file exceeds limit");
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    std::ifstream file(path,std::ios::binary);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(size)))
        throw std::runtime_error("verification file read failed");
    return bytes;
}
bool tag(std::span<const std::uint8_t> b,std::size_t o,const char* text) {
    return o<=b.size() && b.size()-o>=4 && std::equal(b.begin()+o,b.begin()+o+4,text);
}
bool zeroFrame(std::span<const std::uint8_t> b,std::size_t frame) {
    return std::all_of(b.begin()+frame*8,b.begin()+frame*8+8,[](auto value){return value==0;});
}
}
std::vector<std::uint8_t> loadFixturePcm(const std::filesystem::path& wav) {
        const auto fixture=readSmall(wav,64*1024*1024);
        if(!tag(fixture,0,"RIFF") || !tag(fixture,8,"WAVE") || u32(fixture,4)!=fixture.size()-8)
            throw std::runtime_error("invalid RIFF/WAVE size");
        std::span<const std::uint8_t> pcm;
        bool format=false;
        for(std::size_t o=12;o<fixture.size();) {
            if(fixture.size()-o<8) throw std::runtime_error("truncated WAV chunk");
            const auto n=u32(fixture,o+4);
            if(n>fixture.size()-o-8) throw std::runtime_error("WAV chunk outside file");
            if(tag(fixture,o,"fmt ")) {
                if(format || n<16 || u16(fixture,o+8)!=1 || u16(fixture,o+10)!=4 ||
                   u32(fixture,o+12)!=48000 || u32(fixture,o+16)!=384000 ||
                   u16(fixture,o+20)!=8 || u16(fixture,o+22)!=16)
                    throw std::runtime_error("fixture must be PCM 4ch/48k/s16le");
                format=true;
            }
            if(tag(fixture,o,"data")) {
                if(!pcm.empty() || !n || n%8) throw std::runtime_error("invalid WAV data");
                pcm=std::span(fixture).subspan(o+8,n);
            }
            o+=8+static_cast<std::size_t>(n)+(n&1);
            if(o>fixture.size()) throw std::runtime_error("missing WAV pad byte");
        }
        if(!format || pcm.empty()) throw std::runtime_error("missing WAV format/data");
        return {pcm.begin(),pcm.end()};
}
bool verifyCapture(const std::filesystem::path& directory,const std::filesystem::path& wav,std::string& report,std::string& error) {
    try {
        std::string expected="{\"schema\":1,\"complete\":true,\"sha256\":{";
        bool first=true;
        for(const auto* file:{"session.json","transfers.bin","events.jsonl"}) {
            if(!first) expected+=','; first=false;
            expected+=jsonString(file)+':'+jsonString(sha256File(directory/file));
        }
        expected+="}}\n";
        const auto manifest=readSmall(directory/"manifest.json",65536);
        if(std::string(manifest.begin(),manifest.end())!=expected)
            throw std::runtime_error("missing, incomplete, noncanonical, or mismatched manifest");
        const auto session=readSmall(directory/"session.json",65536);
        if(!std::string(session.begin(),session.end()).starts_with("{\"schema\":1,\"capture_abi\":1,\"complete\":true,"))
            throw std::runtime_error("capture was not finalized complete");
        const auto pcm=loadFixturePcm(wav);
        // Bound offline verifier memory separately from the callback queue.
        constexpr std::size_t maximum=64*1024*1024;
        if(std::filesystem::file_size(directory/"transfers.bin")>maximum)
            throw std::runtime_error("verifier supports up to 64 MiB transfer evidence");
        std::ifstream file(directory/"transfers.bin",std::ios::binary);
        std::vector<std::uint8_t> audio;
        std::uint64_t sequence=0,generation=0;
        bool audioSeen=false;
        while(file.peek()!=std::char_traits<char>::eof()) {
            std::vector<std::uint8_t> record(headerSize);
            if(!file.read(reinterpret_cast<char*>(record.data()),headerSize)) throw std::runtime_error("truncated capture header");
            const auto size=u32(record,8);
            if(size<headerSize || size>maxRecordSize) throw std::runtime_error("record size invalid");
            record.resize(size);
            if(!file.read(reinterpret_cast<char*>(record.data()+headerSize),size-headerSize)) throw std::runtime_error("truncated capture data");
            RecordView view; std::string why;
            if(!decode(record,view,why)) throw std::runtime_error(why);
            if(view.sequence!=++sequence) throw std::runtime_error("sequence gap");
            if(view.kind==Kind::Event && u32(view.payload,0)==6) throw std::runtime_error("backend reported capture failure");
            if(view.kind!=Kind::Audio) continue;
            if(!view.validPcm) throw std::runtime_error("invalid PCM packets");
            if(audioSeen && generation!=view.generation) throw std::runtime_error("audio spans multiple generations");
            generation=view.generation; audioSeen=true;
            for(const auto& packet:view.packets) {
                if(packet.requested>maximum-audio.size()) throw std::runtime_error("verification audio limit exceeded");
                const auto data=view.payload.subspan(packet.offset,packet.requested);
                audio.insert(audio.end(),data.begin(),data.end());
            }
        }
        if(!file.eof()) throw std::runtime_error("capture read failure");
        const auto fixtureFrames=pcm.size()/8, capturedFrames=audio.size()/8;
        std::size_t firstFixture=0,firstCapture=0;
        while(firstFixture<fixtureFrames && zeroFrame(pcm,firstFixture)) ++firstFixture;
        while(firstCapture<capturedFrames && zeroFrame(audio,firstCapture)) ++firstCapture;
        if(firstFixture==fixtureFrames) throw std::runtime_error("all-zero fixture cannot prove exact ingestion");
        if(firstCapture<firstFixture) throw std::runtime_error("fixture leading silence missing");
        const auto start=firstCapture-firstFixture;
        if(capturedFrames<fixtureFrames || start>capturedFrames-fixtureFrames ||
           !std::equal(pcm.begin(),pcm.end(),audio.begin()+start*8)) throw std::runtime_error("PCM differs from fixture");
        for(std::size_t i=start+fixtureFrames;i<capturedFrames;++i)
            if(!zeroFrame(audio,i)) throw std::runtime_error("nonzero data follows fixture");
        const auto hash=sha256File(wav);
        report="{\"exact_match\":true,\"fixture_frames\":"+std::to_string(fixtureFrames)+
            ",\"captured_frames\":"+std::to_string(capturedFrames)+",\"generation\":"+std::to_string(generation)+
            ",\"fixture_sha256\":"+jsonString(hash)+",\"report_fixture\":"+
            (hash=="667bd4cc16f0c6862eb5ed080620de9b57c2de7ec53b1732ea3e41185c77f5b0"?"true":"false")+"}";
        return true;
    } catch(const std::exception& e) { error=e.what(); return false; }
}
} // namespace asb::capture
