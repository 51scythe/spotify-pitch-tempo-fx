"""Read-only: report whether Spotify.dll+0xe25b24 is patched (MinHook jmp) in the main Spotify process."""
import ctypes, ctypes.wintypes as wt, subprocess, sys

RVA = 0xe25b24
k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]
psapi.EnumProcessModulesEx.argtypes = [wt.HANDLE, ctypes.POINTER(ctypes.c_void_p), wt.DWORD, ctypes.POINTER(wt.DWORD), wt.DWORD]
psapi.GetModuleBaseNameW.argtypes = [wt.HANDLE, ctypes.c_void_p, wt.LPWSTR, wt.DWORD]

out = subprocess.run(["tasklist", "/fi", "imagename eq Spotify.exe", "/fo", "csv", "/nh"], capture_output=True, text=True).stdout
pids = [int(line.split('","')[1]) for line in out.splitlines() if line.startswith('"Spotify.exe"')]
for pid in pids:
    h = k32.OpenProcess(0x0410, False, pid)
    if not h:
        continue
    mods = (ctypes.c_void_p * 2048)(); need = wt.DWORD()
    psapi.EnumProcessModulesEx(h, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
    names = {}
    for m in mods[: need.value // ctypes.sizeof(ctypes.c_void_p)]:
        buf = ctypes.create_unicode_buffer(260)
        psapi.GetModuleBaseNameW(h, m, buf, 260)
        names[buf.value.lower()] = m
    if "spotify.dll" not in names or "spotify-pitch-tempo.dll" not in names:
        continue
    data = (ctypes.c_ubyte * 16)(); got = ctypes.c_size_t()
    k32.ReadProcessMemory(h, names["spotify.dll"] + RVA, data, 16, ctypes.byref(got))
    print(f"pid={pid} bytes={bytes(data).hex(' ')} patched={data[0] == 0xE9}")
    sys.exit(0)
print("no Spotify process with the plugin DLL loaded")
