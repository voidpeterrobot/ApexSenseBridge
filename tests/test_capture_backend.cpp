#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "capture/WindowsCaptureBackend.h"
#include "capture/FeedbackRecorder.h"
#include <iostream>
#include <thread>
#include <stdexcept>
using namespace asb::capture;
void check(bool value,const char* why) { if(!value) throw std::runtime_error(why); }
int main(int argc,char** argv) {
    try {
        check(argc==2,"fake DLL path required");
        const auto path=std::filesystem::absolute(argv[1]);
        const auto module=LoadLibraryW(path.c_str()); check(module!=nullptr,"fake module");
        auto configure=reinterpret_cast<void(*)(unsigned)>(GetProcAddress(module,"ConfigureCaptureFake"));
        auto created=reinterpret_cast<unsigned(*)()>(GetProcAddress(module,"CaptureFakeCreated"));
        check(configure && created,"fake exports");
        const auto root=std::filesystem::temp_directory_path()/("asb-backend-tests-"+std::to_string(GetCurrentProcessId()));
        std::filesystem::create_directories(root);
        for(unsigned mode=0;mode<7;++mode) {
            configure(mode);
            FeedbackRecorder recorder(root/(std::to_string(mode)+"-"+std::to_string(GetTickCount64())));
            std::string error; check(recorder.start(error),"recorder start");
            WindowsCaptureBackend backend(recorder);
            const bool opened=backend.open(path,error);
            check(opened==(mode==0),"backend open result");
            if(mode==1) check(created()==0,"capabilities checked before server/device creation");
            if(opened) { std::this_thread::sleep_for(std::chrono::milliseconds(20)); check(backend.update({},error),"input update"); }
            check(backend.close(error),"quiesce/detach");
            check(recorder.finish("fake-backend",sha256File(path),error)==opened,"finalization result");
        }
        std::cout << "Fake-DLL capture lifecycle passed (no hardware attached)\n";
        return 0;
    } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
