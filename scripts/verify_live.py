"""Live Spotify bridge/callback checks. No device changes or audio capture."""
import json
import argparse
import os
from pathlib import Path
import time
import urllib.request
import urllib.parse
import urllib.error

base = "http://127.0.0.1:43827"
token = (Path(os.environ["LOCALAPPDATA"])/"SpotifyPitchTempo"/"bridge-token.txt").read_text().strip()
headers = {"X-SPT-Token": token}
results = []
p = argparse.ArgumentParser()
p.add_argument("--output", type=Path, default=Path("reports") / ("live-pitch-" + time.strftime("%Y%m%d-%H%M%S") + ".json"))
args = p.parse_args()
if args.output.exists(): raise SystemExit("Evidence path exists; choose a new output path")

def request(path="/status", settings=None, extra=None):
    data = urllib.parse.urlencode(settings).encode() if settings is not None else None
    req = urllib.request.Request(base+path, data=data, headers=headers if extra is None else extra)
    with urllib.request.urlopen(req, timeout=3) as response:
        return json.load(response)

def set_config(tempo, pitch=0, linked=1, enabled=1):
    return request("/config", dict(tempo=round(tempo*1000), pitch=round(pitch*100), linked=linked, enabled=enabled,reverb=0,distortion=0,muffle=0))

def settled(timeout=4):
    deadline = time.monotonic()+timeout
    while time.monotonic()<deadline:
        s = request()
        if s["commandSequence"] == s["appliedSequence"] and s["allApplied"] and not s["transitioning"]:
            return s
        time.sleep(.03)
    raise AssertionError(f"Audio did not acknowledge request: {s}")

try:
    for tempo,pitch,linked in [(1,0,1),(.85,0,1),(1.25,0,1),(.85,-2,0),(1.25,3,0),(1,4,0)]:
        immediate = set_config(tempo,pitch,linked)
        before = settled()
        start=time.perf_counter()
        time.sleep(3)
        after=request()
        elapsed=time.perf_counter()-start
        ratio=((after["frames"]-before["frames"])&0xffffffff)/elapsed/after["sampleRate"]
        record=dict(tempo=tempo,pitch=pitch,linked=bool(linked),measuredSubmittedRate=ratio,
            acknowledged=bool(after["allApplied"]),acceptedPitch=after["appliedPitch"]/100,
            processingErrors=after["processingErrors"],lastError=after["lastError"],
            silentPacketDelta=after["silentBuffers"]-before["silentBuffers"],
            immediateWasPending=immediate["commandSequence"]!=immediate["appliedSequence"] or bool(immediate["transitioning"]))
        assert abs(ratio-tempo)<tempo*.03,record
        assert not after["faulted"] and not after["processingErrors"] and not after["lastError"],record
        if not linked: assert abs(after["appliedPitch"]/100-pitch)<.02,record
        results.append(record)
        print(json.dumps(record),flush=True)
    for label,path,settings,custom,expected in [
        ("missing-token","/status",None,{},403),
        ("unapproved-origin","/status",None,{**headers,"Origin":"https://example.com"},403),
        ("out-of-range","/config",dict(tempo=9999,pitch=0,linked=1,enabled=1,reverb=0,distortion=0,muffle=0),None,400),
    ]:
        try: request(path,settings,custom)
        except urllib.error.HTTPError as exc:
            assert exc.code==expected,(label,exc.code)
            results.append(dict(check=label,status=exc.code,passed=True))
        else: raise AssertionError(f"{label} was accepted")
finally:
    set_config(1,0,1,0)
    final=settled()
    assert final["bypassConfirmed"] and final["appliedTempo"]==1000 and final["appliedPitch"]==0,final
    results.append(dict(check="final-bypass",status=final))
    args.output.write_text(json.dumps(results,indent=2))
