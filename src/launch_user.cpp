// Run a task using the existing desktop Explorer user's token. This avoids
// applying Spicetify as administrator when the coding harness is elevated.
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <cstdio>
#include <string>
int wmain(int argc,wchar_t** argv) {
    if(argc<2) return 2;
    HWND shell=GetShellWindow();DWORD pid=0;GetWindowThreadProcessId(shell,&pid);
    if(!pid) {std::printf("Desktop shell not found\n");return 3;}
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid), token=nullptr, primary=nullptr;
    if(!process || !OpenProcessToken(process,TOKEN_QUERY|TOKEN_DUPLICATE,&token)) {std::printf("Token error %lu\n",GetLastError());return 4;}
    TOKEN_ELEVATION elevation{};DWORD size=0;
    GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size);
    std::printf("Explorer PID %lu elevated=%lu\n",pid,elevation.TokenIsElevated);
    if(elevation.TokenIsElevated) return 5;
    if(!DuplicateTokenEx(token,MAXIMUM_ALLOWED,nullptr,SecurityImpersonation,TokenPrimary,&primary))return 6;
    std::wstring command;
    for(int i=1;i<argc;++i) {if(i>1)command+=L" ";command+=L"\"";command+=argv[i];command+=L"\"";}
    STARTUPINFOW si{};si.cb=sizeof(si);si.dwFlags=STARTF_USESHOWWINDOW;si.wShowWindow=SW_HIDE;
    PROCESS_INFORMATION pi{};
    BOOL ok=CreateProcessWithTokenW(primary,LOGON_WITH_PROFILE,nullptr,command.data(),CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi);
    std::printf("Launched=%d error=%lu pid=%lu\n",ok,ok?0:GetLastError(),ok?pi.dwProcessId:0);
    if(ok) {CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    CloseHandle(primary);CloseHandle(token);CloseHandle(process);
    return ok?0:7;
}
