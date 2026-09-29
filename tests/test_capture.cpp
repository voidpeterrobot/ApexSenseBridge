#include "capture/FeedbackRecorder.h"
#include "capture/CaptureVerifier.h"
#include "capture/CaptureInputPolicy.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace asb::capture;
namespace {
void require(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
void put(std::vector<std::uint8_t>& b,std::size_t o,std::uint64_t v,std::size_t n=4) {
    for(std::size_t i=0;i<n;++i) b[o+i]=static_cast<std::uint8_t>(v>>(i*8));
}
std::vector<std::uint8_t> audio(std::uint64_t sequence=1) {
    std::vector<std::uint8_t> b(104);
    b[0]='A'; b[1]='S'; b[2]='B'; b[3]='R';
    put(b,4,1,2); put(b,6,1,2); put(b,8,b.size()); put(b,12,80);
    put(b,16,sequence,8); put(b,24,1,8); put(b,32,sequence*1000,8);
    put(b,40,1); put(b,48,48000); put(b,52,4,2); put(b,54,16,2);
    put(b,56,1); put(b,60,8); put(b,68,1); put(b,72,1); put(b,84,8);
    put(b,96,0x8000,2); put(b,98,0x7fff,2); put(b,100,0xffff,2); put(b,102,1,2);
    return b;
}
std::string read(const std::filesystem::path& p) {
    std::ifstream s(p,std::ios::binary); return {std::istreambuf_iterator<char>(s),{}};
}
}
int main() {
    try {
        asb::HidDeviceInfo selected{}; selected.vendorId=0x37d7;selected.productId=0x2502;
        selected.containerId=L"container-a";selected.instanceId=L"exact-instance";
        std::vector<asb::HidDeviceInfo> devices{selected};std::vector<unsigned> slots{1};
        require(selectCaptureXInputSlot(devices,selected,slots)==1,"unique matching identity");
        require(!selectCaptureXInputSlot(devices,selected,{}),"sole unmatched pad never selected");
        slots.push_back(2);require(!selectCaptureXInputSlot(devices,selected,slots),"ambiguous slots refused");slots.pop_back();
        auto other=selected;other.containerId=L"container-b";devices.push_back(other);
        require(!selectCaptureXInputSlot(devices,selected,slots),"ambiguous containers refused");
        auto neutral=mapCaptureXInput({});require(neutral.lx==128 && neutral.ly==128 && neutral.rx==128 && neutral.ry==128,"exact neutral input");
        auto mapped=mapCaptureXInput({-32768,32767,32767,-32768,255,255,asb::platform::xinputButton::kA|asb::platform::xinputButton::kBack});
        require(mapped.lx==0 && mapped.ly==0 && mapped.rx==255 && mapped.ry==255 && mapped.l2==255 && mapped.r2==255,"axes and independent triggers");
        require((mapped.buttons & asb::dualsense::button::kCross) && (mapped.buttons & asb::dualsense::button::kTouchpadClick),"capture buttons policy");
        auto bytes=audio(); RecordView view; std::string error;
        require(decode(bytes,view,error)&&view.validPcm,"signed audio decode");
        require(u16(view.payload,0)==0x8000 && u16(view.payload,2)==0x7fff,"signed extrema preserved");
        auto invalid=bytes; put(invalid,80,1);
        require(decode(invalid,view,error)&&!view.validPcm,"offset gap rejected for analysis");
        invalid=bytes; put(invalid,92,0xffffffff);
        require(decode(invalid,view,error)&&!view.validPcm,"packet failure retained");
        invalid=bytes; put(invalid,84,7);
        require(decode(invalid,view,error)&&!view.validPcm,"partial PCM frame rejected");
        invalid=bytes; put(invalid,56,0xffffffff);
        require(!decode(invalid,view,error),"count overflow rejected");
        invalid=bytes; put(invalid,4,2,2);
        require(!decode(invalid,view,error),"unknown ABI rejected");
        invalid=bytes; put(invalid,6,42,2);
        require(!decode(invalid,view,error),"unknown kind rejected");
        invalid=bytes; put(invalid,48,44100);
        require(!decode(invalid,view,error),"unsupported format rejected");
        for(std::size_t length=0;length<bytes.size();++length)
            require(!decode(std::span(bytes).first(length),view,error),"truncation rejected");
        auto root=std::filesystem::temp_directory_path()/("asb-capture-tests-"+
            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        require(std::filesystem::create_directory(root),"test directory");
        {
            FeedbackRecorder r(root/"ok"); require(r.start(error),"start");
            r.submit(bytes); bytes[96]=99; // callback buffer dies/changes immediately
            require(r.finish("unit-test","fake",error),"complete capture");
            const auto data=read(root/"ok"/"transfers.bin");
            require(data.size()==104 && static_cast<unsigned char>(data[96])==0,"owned callback copy");
            const auto metadata=read(root/"ok"/"session.json");
            require(metadata.find("\"complete\":true")!=std::string::npos,"completion metadata");
            require(metadata.find("\"min\":-32768")!=std::string::npos,"signed metrics");
            require(read(root/"ok"/"manifest.json").find(sha256File(root/"ok"/"transfers.bin"))!=std::string::npos,"manifest hash");
            auto fixture=std::vector<std::uint8_t>(52);
            const auto setTag=[&](std::size_t offset,const char* tag){std::copy_n(tag,4,fixture.begin()+offset);};
            setTag(0,"RIFF");put(fixture,4,44);setTag(8,"WAVE");setTag(12,"fmt ");put(fixture,16,16);
            put(fixture,20,1,2);put(fixture,22,4,2);put(fixture,24,48000);put(fixture,28,384000);
            put(fixture,32,8,2);put(fixture,34,16,2);setTag(36,"data");put(fixture,40,8);
            const auto original=audio();std::copy(original.begin()+96,original.end(),fixture.begin()+44);
            const auto writeFixture=[&]{std::ofstream f(root/"fixture.wav",std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<const char*>(fixture.data()),fixture.size());};
            writeFixture();std::string report;
            require(verifyCapture(root/"ok",root/"fixture.wav",report,error),"exact fixture verification");
            fixture[44]=1;writeFixture();
            require(!verifyCapture(root/"ok",root/"fixture.wav",report,error),"sample mismatch rejected");
            fixture[44]=0;writeFixture();
            { std::ofstream f(root/"ok"/"events.jsonl",std::ios::app);f << ' '; }
            require(!verifyCapture(root/"ok",root/"fixture.wav",report,error),"manifest tampering rejected");
            FeedbackRecorder duplicate(root/"ok"); require(!duplicate.start(error),"never overwrite evidence");
        }
        {
            Limits tiny; tiny.queueBytes=100;
            FeedbackRecorder r(root/"overflow",tiny); require(r.start(error),"overflow start");
            r.submit(audio()); require(r.failed(),"bounded callback queue");
            require(!r.finish("unit-test","fake",error),"overflow incomplete");
        }
        {
            Limits tiny; tiny.outputBytes=tiny.metadataReserve+1024;
            tiny.queueBytes=1024;
            FeedbackRecorder r(root/"storage",tiny); require(r.start(error),"storage start");
            r.submit(audio()); require(!r.finish("unit-test","fake",error),"storage budget incomplete");
        }
        {
            FeedbackRecorder r(root/"gap"); require(r.start(error),"gap start");
            r.submit(audio(2)); require(!r.finish("unit-test","fake",error),"sequence gap incomplete");
        }
        {
            Limits continuous;continuous.continuous=true;continuous.seconds=0;continuous.outputBytes=continuous.metadataReserve+1024;continuous.queueBytes=4096;
            FeedbackRecorder r(root/"continuous",continuous);require(r.start(error),"continuous start");r.submit(audio());r.submit(audio(2));require(r.finish("unit-test","fake",error),"continuous evidence hit cumulative storage budget");require(read(root/"continuous"/"transfers.bin").size()==208,"continuous raw records lost");
            continuous.queueBytes=100;FeedbackRecorder small(root/"continuous-overflow",continuous);require(small.start(error),"continuous bounded start");small.submit(audio());require(small.failed()&&!small.finish("unit-test","fake",error),"continuous capture queue unbounded");
        }
        {
            FeedbackRecorder r(root/"bad"); require(r.start(error),"invalid start");
            auto bad=audio(); put(bad,80,1); r.submit(bad);
            require(!r.finish("unit-test","fake",error),"invalid packets incomplete");
            require(read(root/"bad"/"transfers.bin").size()==bad.size(),"invalid raw retained");
        }
        {
            FeedbackRecorder r(root/"cancel"); require(r.start(error),"cancel start");
            r.submit(audio()); r.fail("operator cancellation");
            require(!r.finish("unit-test","fake",error),"cancel incomplete");
        }
        {
            FeedbackRecorder r(root/"writer-fault",{},[]{throw std::runtime_error("simulated disk failure");});
            require(r.start(error),"writer fault start");r.submit(audio());
            require(!r.finish("unit-test","fake",error),"writer failure incomplete");
            require(read(root/"writer-fault"/"session.json").find("capture writer failed")!=std::string::npos,"writer failure retained");
        }
        {
            std::atomic_bool entered=false,release=false;
            Limits limits;limits.queueBytes=2048;
            FeedbackRecorder r(root/"slow-writer",limits,[&]{
                entered=true;
                const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
                while(!release && std::chrono::steady_clock::now()<end) std::this_thread::sleep_for(std::chrono::milliseconds(1));
            });
            require(r.start(error),"slow writer start");r.submit(audio());
            const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(2);
            while(!entered && std::chrono::steady_clock::now()<end) std::this_thread::yield();
            for(unsigned i=2;i<50;++i) r.submit(audio(i));
            release=true;
            require(r.failed(),"slow disk exhausts bounded queue without callback disk I/O");
            require(!r.finish("unit-test","fake",error),"slow writer overflow incomplete");
        }
        {
            FeedbackRecorder r(root/"generations");require(r.start(error),"generation start");
            r.submit(audio());auto next=audio(2);put(next,24,2,8);r.submit(next);
            require(r.finish("unit-test","fake",error),"raw generation boundary preserved");
            std::string report;
            require(!verifyCapture(root/"generations",root/"fixture.wav",report,error),"verifier cannot stitch generations");
        }
        for(bool expected:{false,true}) {
            FeedbackRecorder r(root/(expected?"terminal-disconnect":"unexpected-disconnect"));
            require(r.start(error),"disconnect start");r.submit(audio());
            auto event=audio(2);event.resize(88);put(event,6,3,2);put(event,8,88);put(event,24,2,8);
            put(event,40,0);put(event,48,0);put(event,52,0);put(event,56,0);put(event,60,8);put(event,68,4);
            put(event,80,2);put(event,84,0);r.submit(event,expected);
            require(r.finish("unit-test","fake",error)==expected,"terminal disconnect classified at receipt, not drain time");
        }
        std::cout << "Capture tests passed; evidence: " << root.string() << '\n';
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
