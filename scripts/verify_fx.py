"""Approved live playback checks: callback evidence, not a listening/loopback test."""
import argparse
import json
import os
from pathlib import Path
import time
import urllib.request
import urllib.parse
import urllib.error

p = argparse.ArgumentParser()
p.add_argument("--output", type=Path, default=Path("reports") / ("live-fx-" + time.strftime("%Y%m%d-%H%M%S") + ".json"))
args = p.parse_args()
if args.output.exists():
    raise SystemExit("Evidence path exists; choose a new output path")
token = (Path(os.environ["LOCALAPPDATA"]) / "SpotifyPitchTempo" / "bridge-token.txt").read_text().strip()
headers = {"X-SPT-Token": token}
defaults = dict(tempo=1000, pitch=0, linked=1, enabled=0, reverb=0, distortion=0, muffle=0)
results = []


def request(settings=None, extra=None, raw=None):
    data = raw if raw is not None else urllib.parse.urlencode(settings).encode() if settings is not None else None
    req = urllib.request.Request("http://127.0.0.1:43827/" + ("config" if data is not None else "status"), data=data, headers=headers if extra is None else extra)
    with urllib.request.urlopen(req, timeout=3) as response:
        return json.load(response)


def wait(config, timeout=5):
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        s = request()
        if s["commandSequence"] == s["appliedSequence"] and s["allApplied"] and not s["transitioning"]:
            assert not s["faulted"] and not s["processingErrors"] and not s["lastError"], s
            for key in ("reverb", "distortion", "muffle"):
                assert s["applied" + key.title()] == (config[key] if config["enabled"] else 0), s
            assert s["appliedTempo"] == (config["tempo"] if config["enabled"] else 1000), s
            if config["enabled"] and not config["linked"]:
                assert abs(s["appliedPitch"] - config["pitch"]) <= 1, s
            return s
        time.sleep(.02)
    raise AssertionError(f"Did not settle: {s}")


try:
    initial = request()
    assert initial["protocol"] == 3 and initial["active"], initial
    results.append(dict(check="initial", status=initial))
    cases = []
    for effect in ("reverb", "distortion", "muffle"):
        for amount in (1, 25, 60, 100):
            cases.append((f"{effect}-{amount}", {effect: amount}))
    cases.extend([
        ("combined", dict(reverb=40, distortion=35, muffle=30)),
        ("combined-independent-slow", dict(tempo=850, pitch=-200, linked=0, reverb=40, distortion=35, muffle=30)),
        ("combined-independent-fast", dict(tempo=1250, pitch=300, linked=0, reverb=40, distortion=35, muffle=30)),
        ("all-maximum", dict(tempo=1500, reverb=100, distortion=100, muffle=100)),
        ("master-disabled-keeps-knobs", dict(enabled=0, tempo=850, pitch=-200, linked=0, reverb=70, distortion=80, muffle=90)),
        ("all-zero-enabled", {}),
    ])
    for label, edits in cases:
        config = {**defaults, "enabled": 1, **edits}
        immediate = request(config)
        pending = immediate["commandSequence"] != immediate["appliedSequence"] or bool(immediate["transitioning"])
        before = wait(config)
        start = time.perf_counter()
        time.sleep(1.5)
        after = wait(config)
        elapsed = time.perf_counter() - start
        submitted = ((after["frames"] - before["frames"]) & 0xffffffff) / elapsed / after["sampleRate"]
        target = config["tempo"] / 1000 if config["enabled"] else 1
        assert abs(submitted - target) < target * .04, (label, submitted, target)
        wet = config["enabled"] and any(config[k] for k in ("reverb", "distortion", "muffle"))
        delta = (after["fxFrames"] - before["fxFrames"]) & 0xffffffff
        assert (delta > 0) == bool(wet), (label, delta)
        if not wet and after["dspLatency"] == 0 and target == 1:
            assert after["bypassConfirmed"], after
        record = dict(check=label, config=config, immediatePending=pending, measuredSubmittedRate=submitted, fxFrameDelta=delta, status=after)
        results.append(record)
        print(json.dumps(dict(check=label, passed=True, immediatePending=pending, submittedRate=round(submitted, 4), fxFrameDelta=delta)), flush=True)
    good = urllib.parse.urlencode(defaults).encode()
    invalid = [
        ("missing-fx-fields", b"tempo=1000&pitch=0&linked=1&enabled=1", None, 400),
        ("amount-too-high", good.replace(b"reverb=0", b"reverb=101"), None, 400),
        ("negative-amount", good.replace(b"muffle=0", b"muffle=-1"), None, 400),
        ("fractional-amount", good.replace(b"distortion=0", b"distortion=3.5"), None, 400),
        ("duplicate-amount", good + b"&reverb=50", None, 400),
        ("unknown-field", good + b"&other=1", None, 400),
        ("missing-token", None, {}, 403),
        ("foreign-origin", None, {**headers, "Origin": "https://example.com"}, 403),
    ]
    for label, raw, custom, code in invalid:
        try:
            request(extra=custom, raw=raw)
        except urllib.error.HTTPError as exc:
            assert exc.code == code, (label, exc.code)
            results.append(dict(check=label, passed=True, status=exc.code))
        else:
            raise AssertionError(f"Invalid request accepted: {label}")
finally:
    try:
        request(defaults)
        final = wait(defaults)
        assert final["bypassConfirmed"] and final["fxLatency"] == 0, final
        results.append(dict(check="final-bypass", status=final))
    finally:
        args.output.write_text(json.dumps(results, indent=2))
