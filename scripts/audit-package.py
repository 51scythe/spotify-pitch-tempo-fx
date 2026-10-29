"""Audit clean release artifacts without printing private connection tokens."""
import argparse
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import tempfile
import zipfile


parser = argparse.ArgumentParser()
parser.add_argument("release", type=Path)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
release = args.release.resolve()
root = Path(__file__).resolve().parent.parent
checks = []


def check(name, passed):
    checks.append({"check": name, "passed": bool(passed)})
    if not passed:
        raise RuntimeError(name)


def sha(data):
    return hashlib.sha256(data).hexdigest().upper()


def scan(name, data):
    parts = name.replace("\\", "/").lower().split("/")
    forbidden = {"spotify.exe", "spotify.dll", "bridge-token.txt", "prefs",
                 ".git", "originals", "reports", "checkpoints", "__pycache__"}
    check("clean path: " + name, not forbidden.intersection(parts)
          and not parts[-1].endswith((".spa", ".pdb", ".obj", ".pyc")))
    for secret in private_patterns:
        check("no private data: " + name, secret not in data)
    if name.lower().endswith(".zip"):
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for entry in archive.infolist():
                if not entry.is_dir():
                    scan(name + "/" + entry.filename, archive.read(entry))


if args.output.exists():
    raise FileExistsError(args.output)
token = (Path(os.environ["LOCALAPPDATA"]) / "SpotifyPitchTempo" / "bridge-token.txt").read_text().strip()
private_patterns = [token.encode(), token.encode("utf-16le")]
for text in (os.environ["USERNAME"], os.environ["USERPROFILE"], str(root)):
    private_patterns.extend((text.encode(), text.encode("utf-16le")))

try:
    for line in (release / "SHA256SUMS.txt").read_text().splitlines():
        expected, name = line.split("  ", 1)
        check("release checksum: " + name, sha((release / name).read_bytes()) == expected)
    scratch_root = root / "build"
    with tempfile.TemporaryDirectory(prefix="release-audit-", dir=scratch_root) as temp:
        payload = Path(temp) / "payload"
        subprocess.run([str(release / "Spotify-Pitch-Tempo-FX-Setup.exe"), "--extract-only", str(payload)], check=True)
        manifest = json.loads((payload / "manifest.json").read_text(encoding="utf-8-sig"))
        check("protocol v3", manifest["protocol"] == 3)
        expected_paths = {entry["path"].replace("\\", "/") for entry in manifest["files"]} | {"manifest.json"}
        actual_paths = {p.relative_to(payload).as_posix() for p in payload.rglob("*") if p.is_file()}
        check("payload has exactly manifest files", actual_paths == expected_paths)
        for entry in manifest["files"]:
            check("manifest hash: " + entry["path"], sha((payload / entry["path"]).read_bytes()) == entry["sha256"])
        for path in payload.rglob("*"):
            if path.is_file():
                scan(path.relative_to(payload).as_posix(), path.read_bytes())
        for name in ("SpotifyPitchTempo.exe", "spotify-pitch-tempo.dll"):
            data = (payload / "bin" / name).read_bytes()
            check("delivered live native binary: " + name, data == (Path(os.environ["LOCALAPPDATA"]) / "SpotifyPitchTempo" / name).read_bytes())
        check("token-free extension is current source", (payload / "extension/pitch-tempo.js").read_bytes() == (root / "src/pitch-tempo.js").read_bytes())
        spice = root / "vendor/spicetify-2.46.4"
        for path in spice.rglob("*"):
            if path.is_file():
                check("unmodified Spicetify: " + path.relative_to(spice).as_posix(), path.read_bytes() == (payload / "spicetify" / path.relative_to(spice)).read_bytes())
    scan("Spotify-Pitch-Tempo-FX-Source.zip", (release / "Spotify-Pitch-Tempo-FX-Source.zip").read_bytes())
    for folder in ("src", "scripts", "packaging"):
        with zipfile.ZipFile(release / "Spotify-Pitch-Tempo-FX-Source.zip") as archive:
            for path in (root / folder).rglob("*"):
                if path.is_file():
                    check("current source: " + path.relative_to(root).as_posix(), archive.read(path.relative_to(root).as_posix()) == path.read_bytes())
finally:
    args.output.write_text(json.dumps({"release": str(release), "checks": checks,
                                     "passed": all(c["passed"] for c in checks)}, indent=2), encoding="utf-8")
print(f"PASS: {len(checks)} artifact integrity, source, dependency and privacy checks")
