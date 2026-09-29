#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "platform/GripControl.h"
#include "platform/SessionControl.h"
#include <cstring>
#include <stdexcept>

namespace asb::platform {
namespace {
struct Block {std::uint32_t magic=0x47425341,version=1;char token[32]{};std::uint32_t revision=0,reserved=0;double gain=1;};
static_assert(sizeof(Block)==56);
class Control final:public GripControl {
    HANDLE mapping_=nullptr,mutex_=nullptr;Block* block_=nullptr;std::string token_;std::uint32_t revision_=0;
public:
    explicit Control(std::string_view token,double initial):token_(token){
        if(!isValidSessionToken(token)||!validGripGain(initial))throw std::runtime_error("Invalid grip control session");
        const auto name=std::wstring(L"Local\\ApexSenseBridge.Grip.v1.")+std::wstring(token.begin(),token.end());
        mutex_=CreateMutexW(nullptr,FALSE,(name+L".Lock").c_str());
        mapping_=CreateFileMappingW(INVALID_HANDLE_VALUE,nullptr,PAGE_READWRITE,0,sizeof(Block),name.c_str());
        const bool exists=GetLastError()==ERROR_ALREADY_EXISTS;
        if(mapping_)block_=static_cast<Block*>(MapViewOfFile(mapping_,FILE_MAP_ALL_ACCESS,0,0,sizeof(Block)));
        if(!mutex_||!block_||exists){cleanup();throw std::runtime_error("Grip control session already exists or cannot be created");}
        Block initialBlock;initialBlock.gain=initial;std::memcpy(initialBlock.token,token.data(),32);*block_=initialBlock;
    }
    void cleanup(){if(block_)UnmapViewOfFile(block_);if(mapping_)CloseHandle(mapping_);if(mutex_)CloseHandle(mutex_);}
    ~Control()override{cleanup();}
    std::optional<double> poll()override{
        const auto wait=WaitForSingleObject(mutex_,0);
        if(wait==WAIT_TIMEOUT)return {};
        if(wait!=WAIT_OBJECT_0&&wait!=WAIT_ABANDONED)throw std::runtime_error("Grip control lock failed");
        const auto b=*block_;ReleaseMutex(mutex_);
        if(wait==WAIT_ABANDONED||b.magic!=0x47425341||b.version!=1||b.reserved||
           std::memcmp(b.token,token_.data(),32)||!validGripGain(b.gain))throw std::runtime_error("Invalid or stale grip control command");
        if(b.revision==revision_)return {};
        if(b.revision<revision_)throw std::runtime_error("Stale grip control revision");
        revision_=b.revision;return b.gain;
    }
};
}
std::unique_ptr<GripControl> createGripControl(std::string_view token,double initial){return std::make_unique<Control>(token,initial);}
}
