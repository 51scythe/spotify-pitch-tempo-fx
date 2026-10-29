# Spotify Pitch, Tempo & FX

Spotify-only pitch, tempo, reverb, distortion and muffle controls for **desktop
Spotify 1.3.6.1921 on Windows 10/11 x64 (Intel/AMD)**.

This repository contains the native helper, process-local audio backend, Spicetify
extension, self-contained installer source, and pinned third-party dependencies.

## Compatibility

**This release supports one exact Spotify build: 1.3.6.1921.** Setup verifies both
Spotify.exe and Spotify.dll hashes before installation. Microsoft Store, ARM,
32-bit and newer/different Spotify builds are unsupported. A Spotify update needs
an updated and revalidated audio backend; rebuilding this source alone does not
add support for a different build.

Spotify binaries, Spotify music, account data and private connection tokens are not included.

## Controls

- Tempo: **0.50x–1.50x**.
- Pitch: **−12 to +12 semitones**.
- Linked mode changes pitch with tempo; unlinked mode adjusts them independently.
- Reverb, Distortion and Muffle each have a separate **0–100% Amount** slider.
- **Reset all & bypass** returns to the original audio. Every helper launch starts
  bypassed; the panel distinguishes requested settings from backend application.

After installation, launch **Spotify + Pitch & Tempo** from its desktop or
Start-menu shortcut, play music, and click the sliders icon in the playback bar.
The panel is named **Pitch, Tempo & FX**. Ordinary Spotify launch does not start
the helper automatically.

## Build the setup

Requirements: Windows x64 and the Visual Studio 2022 C++ toolchain, including a
Windows SDK. `scripts/build.cmd` currently expects the Community edition under
`C:\Program Files\Microsoft Visual Studio\2022\Community`; adjust its
`vcvars64.bat` path if your toolchain is elsewhere.

From the repository root:

```powershell
cmd /c scripts\build.cmd
.\build\dsp-test.exe
.\build\fx-test.exe
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package.ps1
```

Output: **`dist/1.0.0-fx3/Spotify-Pitch-Tempo-FX-Setup.exe`**, instructions,
checksums, and a source/rebuild ZIP. Packaging uses Windows PowerShell and the
built-in .NET Framework C# compiler. Recipients do not need a compiler, Python,
Node, administrator rights or separate Spicetify installation.

The pinned native dependency sources and unmodified Spicetify 2.46.4 distribution
are included under `vendor/`; third-party licenses and Spicetify's full source
archive are included as well. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

## Install, repair or remove

1. Open the supported desktop Spotify build once and sign in.
2. Quit Spotify completely, including its tray icon.
3. Double-click the generated Setup.exe normally; do not run as administrator.
4. Choose **Install / Repair**. Then use **Open Spotify + FX** or the shortcut.

Setup also provides **Check compatibility** and **Remove**. Instructions are in
[packaging/START-HERE.txt](packaging/START-HERE.txt).

Setup installs per user under `%LOCALAPPDATA%\SpotifyPitchTempo` and uses the
standard `%APPDATA%\spicetify` configuration. Each fresh installation receives a
random private connection token; repair retains it. UI, helper, configuration and
shortcut snapshots are created before changes, with rollback on caught failures.

Share the clean generated Setup.exe or this repository. Keep installed folders,
`bridge-token.txt`, local backups and diagnostic logs private.

## Implementation

- `src/controller.cpp`: helper, launcher, local control UI and backend status.
- `src/hook.cpp`: Spotify-process WASAPI hooks and audio processing.
- `src/pitch_processor.h`: Signalsmith Stretch independent pitch compensation.
- `src/audio_effects.h`: oversampled distortion, stereo FDN reverb, low-pass muffle.
- `src/bridge.h`, `src/control.h`: authenticated loopback HTTP and shared protocol v3.
- `src/pitch-tempo.js`: Spicetify controls.
- `packaging/`: installer GUI, compatibility checks, backup/rollback and removal.
- `scripts/`: build, packaging, diagnostics and validation tools.

Tempo uses WASAPI `RATEADJUST`. The processing order is **pitch → 4x oversampled
distortion → stereo reverb → fourth-order low-pass muffle**. Amounts ramp over
about 40 ms, and the UI reports backend acceptance after audio submission.

Processing is confined to Spotify. Installed Spotify native EXE/DLL files remain
original; Spicetify modifies UI assets. There are no audio-device rerouting,
drivers, services or default-device changes.

## Validation and limitations

The backend and controls were tested with live playback on the development PC,
including 18 FX cases, combined independent pitch/tempo, and bypass. Offline
tests covered bypass, formats, filter response, distortion aliasing, reverb decay,
transitions and processing cost. These are not cross-PC compatibility claims.

Installer scratch-copy checks covered fresh install, repair, removal, token
generation/preservation, unavailable-upstream fallback, unrelated extensions,
payload corruption, and failure-injected rollback. The GUI compatibility check
was also exercised. Scratch UI-file tests are not live Spotify playback tests.

Additional developer checks:

```powershell
node --check src\pitch-tempo.js
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\test-package.ps1 -SetupExe dist\1.0.0-fx3\Spotify-Pitch-Tempo-FX-Setup.exe
```

The package integration test requires a local supported Spotify installation,
stock Spicetify backup, and the developer normal-user launch harness. The
`audit-package.py` script additionally compares the package against a local
installed helper and checks for that installation's private token. These are
development-PC checks, not prerequisites for using the setup.

Known limits:

- Independent pitch adds roughly **150 ms / tempo** latency; enabling/bypassing
  can affect transients. Distortion adds 32 source frames.
- Client3 tempo is unsupported. Other drivers and device/hotplug soak need testing.
- At most eight render streams are retained per Spotify process; restart when
  requested by the panel.
- Silent-packet reverb tails were verified offline, not exercised with live music.
- Spotify Connect playback on another device is not processed.
- Power loss or forced setup termination is outside caught-error rollback;
  local snapshots remain available for recovery.
