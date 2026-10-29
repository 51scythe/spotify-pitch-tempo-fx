#pragma once
#include <windows.h>
#include <cstdint>
#include <cmath>

namespace spt {
constexpr uint32_t kMagic = 0x53505431;
constexpr uint32_t kVersion = 3;
struct Settings {
    LONG tempo = 1000; // permille, 500..1500
    LONG pitch = 0;    // cents, -1200..1200
    LONG linked = 1;
    LONG enabled = 0;
    LONG reverb = 0, distortion = 0, muffle = 0; // amounts 0..100 percent
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
};
inline bool validSettings(const Settings& c) {
    return c.tempo >= 500 && c.tempo <= 1500 && c.pitch >= -1200 && c.pitch <= 1200 &&
        (c.linked == 0 || c.linked == 1) && (c.enabled == 0 || c.enabled == 1) &&
        c.reverb >= 0 && c.reverb <= 100 && c.distortion >= 0 && c.distortion <= 100 && c.muffle >= 0 && c.muffle <= 100;
}
inline uint32_t effectAmounts(const Settings& c) {
    return c.enabled ? uint32_t(c.reverb | (c.distortion << 8) | (c.muffle << 16)) : 0;
}
inline Settings readSettings(SharedControl* s, Settings previous = Settings{}, LONG* generation = nullptr) {
    Settings c;
    if (!s || s->magic != kMagic || s->version != kVersion) return c;
    for (int i = 0; i < 8; ++i) {
        LONG seq = InterlockedCompareExchange(&s->sequence, 0, 0);
        if (seq & 1) continue;
        c = {s->tempo, s->pitch, s->linked, s->enabled, s->reverb, s->distortion, s->muffle};
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
    InterlockedIncrement(&s->sequence);
}
inline double pitchFactor(Settings c, double appliedTempo) {
    return (!c.enabled || c.linked) ? 1.0 : std::pow(2.0, c.pitch / 1200.0) / appliedTempo;
}
inline void mappingName(wchar_t* name, size_t count, DWORD pid) {
    swprintf_s(name, count, L"Local\\SpotifyPitchTempo-%lu", pid);
}
}
