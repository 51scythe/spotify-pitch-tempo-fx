"""Read process-local native telemetry; optionally send one control transaction.

The running controller owns the heartbeat. No audio settings or devices are changed.
"""
import argparse
import ctypes as c
import json
import os
import pathlib
import urllib.request
import urllib.parse

p = argparse.ArgumentParser()
p.add_argument("pid", type=int)
p.add_argument("--tempo", type=float)
p.add_argument("--pitch", type=float, help="semitones")
p.add_argument("--linked", type=int, choices=[0, 1])
p.add_argument("--enabled", type=int, choices=[0, 1])
for effect in ("reverb", "distortion", "muffle"):
    p.add_argument("--"+effect, type=int, help="amount 0..100 percent")
a = p.parse_args()
names = "magic version sequence tempo pitch linked enabled attached streams active rateSupported formatSupported sampleRate channels bits appliedTempo appliedPitch frames buffers lastError heartbeat initCalls initFlags stopCalls resetCalls dspLatency processingErrors telemetrySequence appliedSequence transitioning allApplied bypassConfirmed faulted capacityReached untracked init3Calls silentBuffers reverb distortion muffle appliedReverb appliedDistortion appliedMuffle fxLatency fxFrames".split()
class Control(c.Structure):
    _fields_ = [(n, c.c_uint32 if n in ("magic", "version") else c.c_int32) for n in names]
k = c.WinDLL("kernel32", use_last_error=True)
k.OpenFileMappingW.argtypes = [c.c_uint32,c.c_int,c.c_wchar_p]; k.OpenFileMappingW.restype=c.c_void_p
k.MapViewOfFile.argtypes=[c.c_void_p,c.c_uint32,c.c_uint32,c.c_uint32,c.c_size_t];k.MapViewOfFile.restype=c.c_void_p
k.UnmapViewOfFile.argtypes=[c.c_void_p]; k.CloseHandle.argtypes=[c.c_void_p]
h=k.OpenFileMappingW(4,False,f"Local\\SpotifyPitchTempo-{a.pid}")
if not h: raise SystemExit(f"No hook mapping for PID {a.pid}; Windows error {c.get_last_error()}")
ptr=k.MapViewOfFile(h,4,0,0,c.sizeof(Control))
if not ptr: k.CloseHandle(h); raise SystemExit("MapViewOfFile failed")
try:
    s=Control.from_address(ptr)
    if s.magic!=0x53505431 or s.version!=3: raise SystemExit("Protocol mismatch")
    updates={}
    if a.tempo is not None:
        if not 0.5<=a.tempo<=1.5: raise SystemExit("Tempo must be 0.5..1.5")
        updates["tempo"]=round(a.tempo*1000)
    if a.pitch is not None:
        if not -12<=a.pitch<=12: raise SystemExit("Pitch must be -12..12")
        updates["pitch"]=round(a.pitch*100)
    if a.linked is not None: updates["linked"]=a.linked
    if a.enabled is not None: updates["enabled"]=a.enabled
    for effect in ("reverb", "distortion", "muffle"):
        value=getattr(a,effect)
        if value is not None:
            if not 0<=value<=100: raise SystemExit(effect+" amount must be 0..100")
            updates[effect]=value
    if updates:
        # All commands, including diagnostics, use the controller's sole UI-thread writer.
        token=(pathlib.Path(os.environ["LOCALAPPDATA"])/"SpotifyPitchTempo"/"bridge-token.txt").read_text().strip()
        headers={"X-SPT-Token":token}
        with urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:43827/status",headers=headers),timeout=3) as response:
            current=json.load(response)
        if current["pid"]!=a.pid: raise SystemExit("Controller is attached to another PID")
        config={key:current[key] for key in ("tempo","pitch","linked","enabled","reverb","distortion","muffle")}
        config.update(updates)
        body=urllib.parse.urlencode(config).encode()
        with urllib.request.urlopen(urllib.request.Request("http://127.0.0.1:43827/config",data=body,headers=headers),timeout=3) as response:
            response.read()
    for attempt in range(100):
        seq=s.telemetrySequence
        if seq&1: continue
        out={name:getattr(s,name) for name in names}
        if seq==s.telemetrySequence: break
    else: raise SystemExit("Telemetry busy; retry")
    out["lastErrorHex"]=f"0x{s.lastError&0xffffffff:08X}"
    print(json.dumps(out,indent=2))
finally:
    k.UnmapViewOfFile(ptr);k.CloseHandle(h)
