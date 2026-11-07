#pragma once
#include <windows.h>
#include <cstdint>
#include <cmath>

namespace spt {
constexpr uint32_t kMagic = 0x53505431;
constexpr uint32_t kVersion = 4;
constexpr LONG kDefaultPanSpeed = 40;
struct Settings {
    LONG tempo = 1000; // permille, 500..1500
    LONG pitch = 0;    // cents, -1200..1200
    LONG linked = 1;
    LONG enabled = 0;
    LONG reverb = 0, distortion = 0, muffle = 0; // amounts 0..100 percent
    LONG bass = 0, pan = 0, panSpeed = kDefaultPanSpeed, spatial = 0; // 0..100; panSpeed is a rate, not an amount
};
// Fixed-width layout, shared by the native hook and controller. The controller is
// the sole config writer; even sequence values indicate a complete transaction.
struct SharedControl {
    uint32_t magic, version;
    volatile LONG sequence;
    volatile LONG tempo, pitch, linked, enabled;
    volatile LONG attached, streams, active, rateSupported, formatSupported;
    volatile LONG sampleRate, channels, bits, appliedTempo, appliedPitch;
    volatile LONG frames, buffers, lastError, heartbeat;
    volatile LONG initCalls, initFlags, stopCalls, resetCalls;
    volatile LONG dspLatency, processingErrors;
    volatile LONG telemetrySequence, appliedSequence, transitioning, allApplied;
    volatile LONG bypassConfirmed, faulted, capacityReached, untracked;
    volatile LONG init3Calls, silentBuffers;
    volatile LONG reverb, distortion, muffle;
    volatile LONG appliedReverb, appliedDistortion, appliedMuffle, fxLatency, fxFrames;
    volatile LONG bass, pan, panSpeed, spatial;
    volatile LONG appliedBass, appliedPan, appliedPanSpeed, appliedSpatial;
};
inline bool validSettings(const Settings& c) {
    auto percent = [](LONG v) { return v >= 0 && v <= 100; };
    return c.tempo >= 500 && c.tempo <= 1500 && c.pitch >= -1200 && c.pitch <= 1200 &&
        (c.linked == 0 || c.linked == 1) && (c.enabled == 0 || c.enabled == 1) &&
        percent(c.reverb) && percent(c.distortion) && percent(c.muffle) &&
        percent(c.bass) && percent(c.pan) && percent(c.panSpeed) && percent(c.spatial);
}
// Byte n of the packed word: reverb, distortion, muffle, bass, 8D pan, 8D speed, 3D.
// The 8D speed is zero while 8D is off, so all-zero still means exact bypass.
inline uint64_t effectAmounts(const Settings& c) {
    if (!c.enabled) return 0;
    uint64_t speed = c.pan ? uint64_t(c.panSpeed) : 0;
    return uint64_t(c.reverb) | uint64_t(c.distortion) << 8 | uint64_t(c.muffle) << 16 |
        uint64_t(c.bass) << 24 | uint64_t(c.pan) << 32 | speed << 40 | uint64_t(c.spatial) << 48;
}
inline int effectByte(uint64_t amounts, int index) { return int((amounts >> (8*index)) & 255); }
inline Settings readSettings(SharedControl* s, Settings previous = Settings{}, LONG* generation = nullptr) {
    Settings c;
    if (!s || s->magic != kMagic || s->version != kVersion) return c;
    for (int i = 0; i < 8; ++i) {
        LONG seq = InterlockedCompareExchange(&s->sequence, 0, 0);
        if (seq & 1) continue;
        c = {s->tempo, s->pitch, s->linked, s->enabled, s->reverb, s->distortion, s->muffle,
            s->bass, s->pan, s->panSpeed, s->spatial};
        MemoryBarrier();
        if (seq == InterlockedCompareExchange(&s->sequence, 0, 0)) {
            if (!validSettings(c))
                return Settings{};
            c.linked = !!c.linked; c.enabled = !!c.enabled;
            if (generation) *generation = seq;
            return c;
        }
    }
    return previous;
}
// Telemetry is published by one native worker. Retain the last complete snapshot
// if it is momentarily busy. The command sequence has a separate single writer.
inline bool readTelemetry(SharedControl* source, SharedControl& out) {
    for (int i = 0; i < 8; ++i) {
        LONG seq = InterlockedCompareExchange(&source->telemetrySequence, 0, 0);
        if (seq & 1) continue;
        SharedControl copy = *source;
        MemoryBarrier();
        if (seq == InterlockedCompareExchange(&source->telemetrySequence, 0, 0)) {
            out = copy; return true;
        }
    }
    return false;
}
inline void writeSettings(SharedControl* s, Settings c) {
    InterlockedIncrement(&s->sequence);
    s->tempo = c.tempo; s->pitch = c.pitch;
    s->linked = c.linked; s->enabled = c.enabled;
    s->reverb = c.reverb; s->distortion = c.distortion; s->muffle = c.muffle;
    s->bass = c.bass; s->pan = c.pan; s->panSpeed = c.panSpeed; s->spatial = c.spatial;
    InterlockedIncrement(&s->sequence);
}
// Linked mode compensates when the clock is held below the requested tempo.
inline double pitchFactor(Settings c, double appliedTempo) {
    if (!c.enabled) return 1.0;
    return (c.linked ? c.tempo / 1000.0 : std::pow(2.0, c.pitch / 1200.0)) / appliedTempo;
}
inline void mappingName(wchar_t* name, size_t count, DWORD pid) {
    swprintf_s(name, count, L"Local\\SpotifyPitchTempo-%lu", pid);
}
}
