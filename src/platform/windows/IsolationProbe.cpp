#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <Xinput.h>
// Deliberately a separate, unlisted process: the engine is allowed through HidHide.
int main(){for(DWORD i=0;i<4;++i){XINPUT_STATE state{};const auto status=XInputGetState(i,&state);if(status!=ERROR_DEVICE_NOT_CONNECTED)return 1;}return 0;}
