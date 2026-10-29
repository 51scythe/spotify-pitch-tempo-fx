"""Measure Spotify's submitted PCM frame rate against wall-clock time.

Uses existing hook counters, not audio recording. Source consumption pacing is
measured independently of SetSampleRate's return value.
"""
import json
import pathlib
import subprocess
import sys
import time

pid = sys.argv[1]
status_script = str(pathlib.Path(__file__).with_name("status.py"))
def status(*args):
    return json.loads(subprocess.check_output([sys.executable,status_script,pid,*args],text=True))

results=[]
try:
    for tempo,pitch,linked in [(1,0,1),(.85,0,1),(1.25,0,1),(.85,-2,0),(1.25,3,0)]:
        status("--tempo",str(tempo),"--pitch",str(pitch),"--linked",str(linked),"--enabled","1")
        time.sleep(1)
        before=status(); start=time.perf_counter()
        time.sleep(5)
        after=status(); elapsed=time.perf_counter()-start
        source_frames=(after["frames"]-before["frames"])&0xffffffff
        effective_rate=source_frames/elapsed/after["sampleRate"] if after["sampleRate"] else 0
        record={"requestedTempo":tempo,"requestedPitchSemitones":pitch,"linked":bool(linked),
                "elapsedSeconds":elapsed,"submittedFrames":source_frames,
                "measuredSourceRate":effective_rate,"rateApplied":after["appliedTempo"]/1000,
                "pitchApplied":after["appliedPitch"]/100,"activeStreams":after["active"],
                "lastError":after["lastErrorHex"],"processingErrors":after["processingErrors"],
                "sourceRateWithin3Percent":abs(effective_rate-tempo)<tempo*.03}
        results.append(record);print(json.dumps(record),flush=True)
finally:
    status("--tempo","1","--pitch","0","--linked","1","--enabled","0")
    pathlib.Path("reports/live-source-rate.json").write_text(json.dumps(results,indent=2))
