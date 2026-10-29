#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <string>
#include <vector>
#include <cmath>
#include "control.h"
#include "bridge.h"

using namespace spt;
namespace {
constexpr int kEnable=101, kTempo=102, kPitch=103, kLinked=104, kReset=105, kAttach=106;
HWND window, enableBox, tempoSlider, pitchSlider, linkedBox, tempoLabel, pitchLabel, statusLabel;
HWND effectSliders[3]{},effectLabels[3]{};
const wchar_t* effectNames[3]={L"Reverb",L"Distortion",L"Muffle"};
SharedControl* shared = nullptr;
HANDLE mapping = nullptr;
DWORD spotifyPid = 0;
std::wstring notice = L"Attach to Spotify to begin. Effects start bypassed.";
bool launchRequested = false;
bool backgroundMode = false;
Settings settings;
SharedControl telemetry{};
std::wstring installedSpotify();

bool supportedBuild() {
    std::wstring dll = installedSpotify();
    dll.replace(dll.size()-3, 3, L"dll");
    HANDLE file = CreateFileW(dll.c_str(), GENERIC_READ, FILE_SHARE_READ|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    HCRYPTPROV provider = 0; HCRYPTHASH hash = 0;
    bool ok = CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash);
    BYTE buffer[65536]; DWORD bytes = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &bytes, nullptr)) { ok = false; break; }
        if (!bytes) break;
        ok = CryptHashData(hash, buffer, bytes, 0) != FALSE;
    }
    BYTE digest[32]; DWORD size = sizeof(digest); std::string text;
    if (ok) ok = CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) != FALSE;
    if (ok) for (BYTE value : digest) { char hex[3]; sprintf_s(hex, "%02x", value); text += hex; }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    CloseHandle(file);
    return ok && text == "93ba5ee486e148114a1d6db388cce318b04d2a011376ff6301f5ee0dea432a12";
}

std::wstring exeDirectory() {
    wchar_t path[32768]; GetModuleFileNameW(nullptr, path, 32768);
    std::wstring value(path); return value.substr(0, value.find_last_of(L'\\'));
}
std::wstring installedSpotify() {
    wchar_t appdata[32768]{}; GetEnvironmentVariableW(L"APPDATA", appdata, 32768);
    return std::wstring(appdata) + L"\\Spotify\\Spotify.exe";
}
bool exactSpotifyPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    wchar_t path[32768]{}; DWORD count = 32768;
    bool ok = QueryFullProcessImageNameW(process, 0, path, &count) && !_wcsicmp(path, installedSpotify().c_str());
    CloseHandle(process); return ok;
}
DWORD findSpotify() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    std::vector<DWORD> candidates;
    if (Process32FirstW(snap, &entry)) do {
        if (!_wcsicmp(entry.szExeFile, L"Spotify.exe") && exactSpotifyPath(entry.th32ProcessID))
            candidates.push_back(entry.th32ProcessID);
    } while (Process32NextW(snap, &entry));
    CloseHandle(snap);
    DWORD result = 0; ULONGLONG earliest = ~0ULL;
    for (DWORD pid : candidates) {
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        FILETIME created{}, end{}, kernel{}, user{};
        if (p && GetProcessTimes(p, &created, &end, &kernel, &user)) {
            ULONGLONG time = (ULONGLONG(created.dwHighDateTime) << 32) | created.dwLowDateTime;
            if (time < earliest) { earliest = time; result = pid; }
        }
        if (p) CloseHandle(p);
    }
    return result;
}
bool openMapping(DWORD pid) {
    wchar_t key[96]; mappingName(key, 96, pid);
    HANDLE handle = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, key);
    if (!handle) return false;
    auto* ptr = static_cast<SharedControl*>(MapViewOfFile(handle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControl)));
    if (!ptr || ptr->magic != kMagic || ptr->version != kVersion) {
        if (ptr) UnmapViewOfFile(ptr); CloseHandle(handle); return false;
    }
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
    mapping = handle; shared = ptr; spotifyPid = pid;
    return true;
}
bool inject(DWORD pid) {
    if (!exactSpotifyPath(pid)) { notice = L"Refused: process is not the installed Spotify.exe."; return false; }
    if (openMapping(pid) && shared->attached) return true;
    if (!supportedBuild()) { notice = L"This backend supports Spotify 1.3.6.1921 only. The installed Spotify.dll differs; update the backend before attaching."; return false; }
    std::wstring dll = exeDirectory() + L"\\spotify-pitch-tempo.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { notice = L"Native DLL is missing beside this controller."; return false; }
    HANDLE process = OpenProcess(PROCESS_CREATE_THREAD|PROCESS_QUERY_INFORMATION|PROCESS_VM_OPERATION|PROCESS_VM_WRITE|PROCESS_VM_READ, FALSE, pid);
    if (!process) { notice = L"Could not open Spotify for the process-local hook."; return false; }
    size_t bytes = (dll.size()+1)*sizeof(wchar_t);
    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
    SIZE_T written = 0;
    bool ok = remote && WriteProcessMemory(process, remote, dll.c_str(), bytes, &written) && written == bytes;
    // Resolve the remote kernel32 base instead of assuming equal ASLR addresses.
    uintptr_t remoteKernel = 0;
    HANDLE modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32, pid);
    MODULEENTRY32W module{}; module.dwSize = sizeof(module);
    if (modules != INVALID_HANDLE_VALUE) {
        if (Module32FirstW(modules, &module)) do {
            if (!_wcsicmp(module.szModule, L"KERNEL32.DLL")) remoteKernel = uintptr_t(module.modBaseAddr);
        } while (Module32NextW(modules, &module));
        CloseHandle(modules);
    }
    auto kernel = GetModuleHandleW(L"kernel32.dll");
    auto load = GetProcAddress(kernel, "LoadLibraryW");
    uintptr_t remoteLoad = remoteKernel + (uintptr_t(load) - uintptr_t(kernel));
    HANDLE thread = (ok && remoteKernel) ? CreateRemoteThread(process, nullptr, 0, reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoad), remote, 0, nullptr) : nullptr;
    DWORD wait = thread ? WaitForSingleObject(thread, 10000) : WAIT_FAILED;
    ok = thread && wait == WAIT_OBJECT_0;
    if (thread) CloseHandle(thread);
    // On timeout the remote loader may still be using its argument.
    if (remote && wait != WAIT_TIMEOUT) VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    CloseHandle(process);
    if (ok) {
        for (int attempt=0; attempt<100; ++attempt) {
            if (openMapping(pid) && shared->attached) { notice = L"Attached. Press play in Spotify; effects are bypassed until enabled."; return true; }
            Sleep(50);
        }
    }
    notice = L"Hook did not become ready. See native error in status; Spotify files were not modified.";
    return false;
}
bool startSpotify() {
    if (DWORD pid = findSpotify()) return inject(pid);
    std::wstring exe = installedSpotify();
    std::wstring args = L"\"" + exe + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), args.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &si, &pi)) {
        notice = L"Could not launch installed Spotify."; return false;
    }
    // Resume briefly because Windows does not expose kernel32 modules before
    // loader initialization. Hook initialization then precedes user playback.
    ResumeThread(pi.hThread); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    Sleep(600);
    return inject(pi.dwProcessId);
}
void syncControls() {
    SendMessageW(enableBox, BM_SETCHECK, settings.enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(linkedBox, BM_SETCHECK, settings.linked ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(tempoSlider, TBM_SETPOS, TRUE, settings.tempo);
    SendMessageW(pitchSlider, TBM_SETPOS, TRUE, settings.pitch/10);
    EnableWindow(pitchSlider, !settings.linked);
    wchar_t text[192];
    swprintf_s(text, L"Tempo: %.2fx", settings.tempo/1000.0); SetWindowTextW(tempoLabel, text);
    double pitch = settings.linked ? 12*std::log2(settings.tempo/1000.0) : settings.pitch/100.0;
    swprintf_s(text, L"Pitch: %+.2f semitones%s", pitch, settings.linked ? L" (linked)" : L""); SetWindowTextW(pitchLabel, text);
    LONG amounts[3]={settings.reverb,settings.distortion,settings.muffle};
    for(int i=0;i<3;++i) {
        SendMessageW(effectSliders[i],TBM_SETPOS,TRUE,amounts[i]);
        swprintf_s(text,L"%s amount: %ld%%",effectNames[i],amounts[i]); SetWindowTextW(effectLabels[i],text);
    }
}
void pushControls() {
    settings.enabled = SendMessageW(enableBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.linked = SendMessageW(linkedBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
    settings.tempo = LONG(SendMessageW(tempoSlider, TBM_GETPOS, 0, 0));
    settings.pitch = LONG(SendMessageW(pitchSlider, TBM_GETPOS, 0, 0))*10;
    settings.reverb=LONG(SendMessageW(effectSliders[0],TBM_GETPOS,0,0));
    settings.distortion=LONG(SendMessageW(effectSliders[1],TBM_GETPOS,0,0));
    settings.muffle=LONG(SendMessageW(effectSliders[2],TBM_GETPOS,0,0));
    if (shared) { writeSettings(shared, settings); shared->heartbeat = LONG(GetTickCount()); }
    syncControls();
}
void updateStatus() {
    if (!shared) { SetWindowTextW(statusLabel, notice.c_str()); return; }
    HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, spotifyPid);
    bool exited = !p || WaitForSingleObject(p, 0) == WAIT_OBJECT_0;
    if (p) CloseHandle(p);
    if (exited) {
        UnmapViewOfFile(shared); CloseHandle(mapping); shared = nullptr; mapping = nullptr;
        notice = L"Spotify exited. Controls detached."; SetWindowTextW(statusLabel, notice.c_str()); return;
    }
    shared->heartbeat = LONG(GetTickCount());
    Settings incoming = readSettings(shared);
    if (incoming.tempo != settings.tempo || incoming.pitch != settings.pitch || incoming.linked != settings.linked || incoming.enabled != settings.enabled ||
        incoming.reverb!=settings.reverb || incoming.distortion!=settings.distortion || incoming.muffle!=settings.muffle) {
        settings = incoming; syncControls();
    }
    wchar_t text[1200];
    readTelemetry(shared, telemetry);
    bool current = telemetry.appliedSequence == shared->sequence;
    const wchar_t* state = !telemetry.active ? L"Waiting for a new Spotify audio stream; restart via shortcut if already playing" :
        telemetry.faulted ? L"Backend fault; restart Spotify" :
        !current || telemetry.transitioning ? L"Applying settings" :
        !settings.enabled && telemetry.bypassConfirmed ? L"Bypassed" :
        telemetry.allApplied ? L"Processing" : L"Request not fully applied";
    double latencyMs = telemetry.sampleRate && telemetry.appliedTempo ? 1000000.0*telemetry.dspLatency/(double(telemetry.sampleRate)*telemetry.appliedTempo) : 0;
    swprintf_s(text, L"%s\r\nPID %lu | streams %ld | active %ld | buffers %ld\r\n"
        L"Audio: %ld Hz, %ld ch, %ld bit | clock control: %s | PCM: %s\r\n"
        L"Applied: %.3fx, %+.2f semitones | DSP latency: %.0f ms\r\n"
        L"Native error: 0x%08lX | processing errors: %ld\r\n%s",
        state, spotifyPid, shared->streams, shared->active, shared->buffers,
        shared->sampleRate, shared->channels, shared->bits, shared->rateSupported ? L"available" : L"not confirmed",
        shared->formatSupported ? L"supported" : L"not confirmed", shared->appliedTempo/1000.0,
        shared->appliedPitch/100.0, latencyMs, DWORD(shared->lastError), shared->processingErrors, notice.c_str());
    SetWindowTextW(statusLabel, text);
}
HWND child(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id=0) {
    HWND result = CreateWindowExW(0, cls, text, WS_CHILD|WS_VISIBLE|style, x,y,w,h,window,reinterpret_cast<HMENU>(INT_PTR(id)),nullptr,nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    return result;
}
LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch(msg) {
    case WM_CREATE:
        window = hwnd;
        child(L"STATIC", L"Spotify Pitch + Tempo  |  Native prototype", 0, 22,18,620,26);
        enableBox = child(L"BUTTON", L"Enable effects", BS_AUTOCHECKBOX,22,52,180,25,kEnable);
        linkedBox = child(L"BUTTON", L"Link pitch to tempo (nightcore / slowed)", BS_AUTOCHECKBOX,210,52,390,25,kLinked);
        tempoLabel = child(L"STATIC", L"", 0,22,96,590,24);
        tempoSlider = child(TRACKBAR_CLASSW,L"",TBS_AUTOTICKS|WS_TABSTOP,18,122,602,42,kTempo);
        SendMessageW(tempoSlider,TBM_SETRANGE,TRUE,MAKELPARAM(500,1500));
        SendMessageW(tempoSlider,TBM_SETTICFREQ,100,0);
        pitchLabel = child(L"STATIC", L"", 0,22,177,590,24);
        pitchSlider = child(TRACKBAR_CLASSW,L"",TBS_AUTOTICKS|WS_TABSTOP,18,203,602,42,kPitch);
        SendMessageW(pitchSlider,TBM_SETRANGE,TRUE,MAKELPARAM(-120,120));
        SendMessageW(pitchSlider,TBM_SETTICFREQ,10,0);
        for(int i=0;i<3;++i) {
            effectLabels[i]=child(L"STATIC",L"",0,22,259+i*62,590,24);
            effectSliders[i]=child(TRACKBAR_CLASSW,L"",TBS_AUTOTICKS|WS_TABSTOP,18,282+i*62,602,32,110+i);
            SendMessageW(effectSliders[i],TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
            SendMessageW(effectSliders[i],TBM_SETTICFREQ,10,0);
        }
        child(L"BUTTON",L"Reset / bypass",BS_PUSHBUTTON,22,452,170,32,kReset);
        child(L"BUTTON",L"Attach to Spotify",BS_PUSHBUTTON,210,452,180,32,kAttach);
        statusLabel = child(L"STATIC",L"",0,22,499,610,150);
        child(L"STATIC",L"Closing this controller requests original audio within 2 seconds.",0,22,655,610,22);
        syncControls(); SetTimer(hwnd,1,250,nullptr); PostMessageW(hwnd,WM_APP,0,0); return 0;
    case WM_APP:
        if (launchRequested) startSpotify();
        else { DWORD pid=findSpotify(); if(pid) inject(pid); }
        if(shared) { writeSettings(shared,Settings{}); shared->heartbeat=LONG(GetTickCount()); }
        startBridge(hwnd,exeDirectory()+L"\\bridge-token.txt");
        if(backgroundMode && !shared) { ShowWindow(hwnd,SW_SHOW); backgroundMode=false; }
        updateStatus(); return 0;
    case kBridgeMessage: {
        auto* request=reinterpret_cast<BridgeRequest*>(lp);
        if(!shared || !shared->attached) {
            request->status=503;request->json="{\"error\":\"Native backend is not attached. Check the helper window and restart Spotify with the Pitch + Tempo shortcut.\"}";return 0;
        }
        if(request->set) {
            settings=request->settings;syncControls();
            writeSettings(shared,settings);shared->heartbeat=LONG(GetTickCount());
        }
        Settings c=readSettings(shared);char json[2048];
        readTelemetry(shared,telemetry);
        auto* t=&telemetry;
        sprintf_s(json,"{\"pid\":%lu,\"tempo\":%ld,\"pitch\":%ld,\"linked\":%ld,\"enabled\":%ld,"
            "\"attached\":%ld,\"active\":%ld,\"rateSupported\":%ld,\"formatSupported\":%ld,"
            "\"appliedTempo\":%ld,\"appliedPitch\":%ld,\"sampleRate\":%ld,\"channels\":%ld,"
            "\"frames\":%ld,\"buffers\":%ld,\"dspLatency\":%ld,\"processingErrors\":%ld,\"lastError\":%ld,"
            "\"commandSequence\":%ld,\"appliedSequence\":%ld,\"transitioning\":%ld,\"allApplied\":%ld,"
            "\"bypassConfirmed\":%ld,\"faulted\":%ld,\"capacityReached\":%ld,\"streams\":%ld,\"untracked\":%ld,"
            "\"init3Calls\":%ld,\"silentBuffers\":%ld,\"protocol\":3,"
            "\"reverb\":%ld,\"distortion\":%ld,\"muffle\":%ld,\"appliedReverb\":%ld,\"appliedDistortion\":%ld,\"appliedMuffle\":%ld,\"fxLatency\":%ld,\"fxFrames\":%ld}",
            spotifyPid,c.tempo,c.pitch,c.linked,c.enabled,shared->attached,t->active,t->rateSupported,
            t->formatSupported,t->appliedTempo,t->appliedPitch,t->sampleRate,t->channels,
            t->frames,t->buffers,t->dspLatency,t->processingErrors,t->lastError,
            shared->sequence,t->appliedSequence,t->transitioning,t->allApplied,t->bypassConfirmed,t->faulted,
            t->capacityReached,t->streams,t->untracked,t->init3Calls,t->silentBuffers,
            c.reverb,c.distortion,c.muffle,t->appliedReverb,t->appliedDistortion,t->appliedMuffle,t->fxLatency,t->fxFrames);
        request->json=json;return 0;
    }
    case WM_HSCROLL: pushControls(); return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case kEnable: case kLinked: pushControls(); break;
        case kReset: settings=Settings{}; syncControls(); pushControls(); break;
        case kAttach:
            { DWORD pid=findSpotify(); if(pid) inject(pid); else startSpotify();
               if(shared) { writeSettings(shared,settings); shared->heartbeat=LONG(GetTickCount()); } }
            break;
        } return 0;
    case WM_APP + 13:
        if(!findSpotify()) startSpotify();
        else ShellExecuteW(nullptr,L"open",installedSpotify().c_str(),nullptr,nullptr,SW_SHOW);
        return 0;
    case WM_TIMER:
        updateStatus();
        if(backgroundMode && bridgeError) { notice=L"Local bridge could not start. Check port 43827 or reinstall the extension."; ShowWindow(hwnd,SW_SHOW); backgroundMode=false; }
        if(backgroundMode && !shared && spotifyPid) PostMessageW(hwnd,WM_CLOSE,0,0);
        return 0;
    case WM_CLOSE:
        if(shared) { settings.enabled=0; writeSettings(shared,settings); }
        DestroyWindow(hwnd); return 0;
    case WM_DESTROY:
        if(shared) UnmapViewOfFile(shared); if(mapping) CloseHandle(mapping);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR args,int show) {
    // A single config writer and heartbeat source.
    HANDLE singleton = CreateMutexW(nullptr, FALSE, L"Local\\SpotifyPitchTempoController");
    if (!singleton) return 1;
    if (GetLastError()==ERROR_ALREADY_EXISTS) {
        HWND existing=FindWindowW(L"SpotifyPitchTempoController",nullptr);
        if(existing) PostMessageW(existing,WM_APP+13,0,0);
        CloseHandle(singleton); return 0;
    }
    launchRequested = wcsstr(args,L"--launch") != nullptr;
    backgroundMode = wcsstr(args,L"--background") != nullptr;
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_BAR_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW cls{}; cls.lpfnWndProc=proc; cls.hInstance=instance;
    cls.lpszClassName=L"SpotifyPitchTempoController"; cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    cls.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1); RegisterClassW(&cls);
    HWND hwnd=CreateWindowExW(0,cls.lpszClassName,L"Spotify Pitch + Tempo",WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_MINIMIZEBOX,CW_USEDEFAULT,CW_USEDEFAULT,666,733,nullptr,nullptr,instance,nullptr);
    if(!hwnd) return 2;
    if(!backgroundMode) {ShowWindow(hwnd,show); UpdateWindow(hwnd);}
    MSG message; while(GetMessageW(&message,nullptr,0,0)>0) { TranslateMessage(&message); DispatchMessageW(&message); }
    CloseHandle(singleton); return 0;
}
