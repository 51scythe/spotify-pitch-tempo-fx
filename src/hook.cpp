#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <ksmedia.h>
#include <wrl/client.h>
#include <MinHook.h>
#include <intrin.h>
#include <atomic>
#include <array>
#include <memory>
#include <mutex>
#include "control.h"
#include "pitch_processor.h"
#include "audio_effects.h"

using Microsoft::WRL::ComPtr;
using namespace spt;
namespace {
using InitFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID);
using Init3Fn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient3*, DWORD, UINT32, const WAVEFORMATEX*, LPCGUID);
using ServiceFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*, REFIID, void**);
using SimpleFn = HRESULT(STDMETHODCALLTYPE*)(IAudioClient*);
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, BYTE**);
using ReleaseBufferFn = HRESULT(STDMETHODCALLTYPE*)(IAudioRenderClient*, UINT32, DWORD);
InitFn originalInit;
Init3Fn originalInit3;
ServiceFn originalService;
SimpleFn originalStart, originalStop, originalReset;
GetBufferFn originalGetBuffer;
ReleaseBufferFn originalReleaseBuffer;
SharedControl* shared = nullptr;
HANDLE mapping = nullptr;
uintptr_t spotifyBase = 0;

// The launcher verifies Spotify.dll's SHA-256 before injection. These are the
// traced native playback renderer's initialization bounds for 1.3.6.1921 only.
// Capture, loopback, CEF and exclusive clients are never retained or modified.
bool nativeRenderer(void* caller) {
    uintptr_t rva = uintptr_t(caller) - spotifyBase;
    return spotifyBase && rva >= 0xdc1fa8 && rva < 0xdc3cac;
}
struct Counters {
    std::atomic<LONG> frames{0}, buffers{0}, silentBuffers{0}, errors{0};
    std::atomic<LONG> initCalls{0}, init3Calls{0}, initFlags{0}, stopCalls{0}, resetCalls{0};
    std::atomic<LONG> lastError{0}, capacityReached{0}, untracked{0};
} counters;

struct Stream {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioClockAdjustment> clock; // worker-thread acquisition and ownership
    std::array<std::atomic<IAudioRenderClient*>, 4> renderers{};
    PitchProcessor dsp;
    AudioEffects effects;
    std::vector<BYTE> scratch;
    DWORD rate = 0;
    WORD channels = 0, bits = 0, blockAlign = 0;
    int latency = 0;
    bool formatOK = false, rateFlag = false, clockAttempted = false;
    std::atomic<bool> active{false}, reset{false}, fault{false};
    // Factor (float bit representation) and command generation published together.
    std::atomic<uint64_t> command{0};
    std::atomic<uint64_t> effectsCommand{0};
    std::atomic<uint32_t> commandSequence{0};
    uint64_t audioCommand=0,audioEffectsCommand=0; // audio-thread last coherent mailbox
    std::atomic<LONG> dspSequence{-1};
    std::atomic<double> acceptedFactor{1};
    std::atomic<bool> transitioning{false}, wet{false};
    std::atomic<bool> effectsWet{false};
    std::atomic<uint32_t> acceptedEffects{0};
    std::atomic<LONG> effectsLatency{0},effectsFrames{0};
    std::atomic<uint32_t> packetSequence{0};
    struct PacketSnapshot {
        LONG generation=-1,latency=0,frames=0;
        double factor=1;
        bool transitioning=false,wet=false,effectsWet=false;
        uint32_t effects=0;
    } acknowledged; // worker-owned last complete, successfully submitted packet
    std::atomic<LONG> packetError{0};
    double appliedTempo = 1; // worker-owned, only updated on successful API return
    LONG clockError = 0;
    BYTE* buffer = nullptr; // GetBuffer/ReleaseBuffer pair is owned by one audio thread
    UINT32 requestedFrames = 0;
    bool wasSilent = false;
};
// Bounded process-lifetime ownership. No real-time locks, allocations or reclaim
// races. A ninth distinct render client stays untouched and reports restart needed.
// Proper COM-wrapper retirement is a future improvement; Stop alone is NOT teardown.
constexpr size_t kMaxStreams = 8;
std::array<std::atomic<Stream*>, kMaxStreams> streams{};
struct Pending {
    IAudioClient* client = nullptr; // identity only; never dereferenced by worker
    WAVEFORMATEXTENSIBLE format{};
    bool rateFlag = false;
};
std::array<Pending, 32> pending{};
size_t pendingNext = 0;
std::mutex registrationMutex; // only initialization / GetService; never audio lookup

Stream* findStream(IAudioClient* client) noexcept {
    for (auto& slot : streams) { auto* s = slot.load(); if (s && s->client.Get() == client) return s; }
    return nullptr;
}
Stream* findRenderer(IAudioRenderClient* renderer) noexcept {
    for (auto& slot : streams) {
        auto* s = slot.load();
        if (s) for (auto& alias : s->renderers) if (alias.load() == renderer) return s;
    }
    return nullptr;
}
void remember(IAudioClient* client, const WAVEFORMATEX* format, bool rateFlag) noexcept {
    if (!format) return;
    try {
        std::lock_guard<std::mutex> lock(registrationMutex);
        Pending* entry = nullptr;
        for (auto& p : pending) if (p.client == client) { entry = &p; break; }
        if (!entry) entry = &pending[pendingNext++ % pending.size()];
        *entry = Pending{}; entry->client = client; entry->rateFlag = rateFlag;
        size_t bytes = sizeof(WAVEFORMATEX);
        if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) bytes = sizeof(WAVEFORMATEXTENSIBLE);
        memcpy(&entry->format, format, bytes);
    } catch (...) { counters.errors++; counters.lastError = E_OUTOFMEMORY; }
}
bool parseFormat(const WAVEFORMATEXTENSIBLE& ext, bool& floating, int& validBits) {
    const auto& f = ext.Format;
    if (f.nChannels < 1 || f.nChannels > 8 || f.nSamplesPerSec < 8000 || f.nSamplesPerSec > 192000) return false;
    WORD tag = f.wFormatTag; validBits = f.wBitsPerSample;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        if (f.cbSize < 22) return false;
        if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (ext.SubFormat == KSDATAFORMAT_SUBTYPE_PCM) tag = WAVE_FORMAT_PCM;
        else return false;
        validBits = ext.Samples.wValidBitsPerSample;
    }
    floating = tag == WAVE_FORMAT_IEEE_FLOAT;
    return validBits >= 1 && validBits <= f.wBitsPerSample &&
        ((floating && f.wBitsPerSample == 32 && validBits == 32) ||
        (tag == WAVE_FORMAT_PCM && (f.wBitsPerSample == 16 || f.wBitsPerSample == 24 || f.wBitsPerSample == 32))) &&
        f.nBlockAlign == f.nChannels*(f.wBitsPerSample/8);
}
HRESULT STDMETHODCALLTYPE initHook(IAudioClient* client, AUDCLNT_SHAREMODE mode, DWORD flags,
        REFERENCE_TIME duration, REFERENCE_TIME periodicity, const WAVEFORMATEX* format, LPCGUID session) {
    bool candidate = nativeRenderer(_ReturnAddress()) && mode == AUDCLNT_SHAREMODE_SHARED && !(flags & AUDCLNT_STREAMFLAGS_LOOPBACK);
    if (!candidate) return originalInit(client, mode, flags, duration, periodicity, format, session);
    counters.initCalls++; counters.initFlags = LONG(flags);
    DWORD newFlags = flags | AUDCLNT_STREAMFLAGS_RATEADJUST;
    HRESULT hr = originalInit(client, mode, newFlags, duration, periodicity, format, session);
    bool enabledRate = SUCCEEDED(hr);
    if (FAILED(hr) && newFlags != flags) {
        // Best effort only: WASAPI can make a failed client non-reinitializable.
        // Preserve the first failure; never claim this guarantees recovery.
        counters.lastError = hr;
        hr = originalInit(client, mode, flags, duration, periodicity, format, session);
    }
    if (FAILED(hr)) counters.lastError = hr;
    else remember(client, format, enabledRate);
    return hr;
}
HRESULT STDMETHODCALLTYPE init3Hook(IAudioClient3* client, DWORD flags, UINT32 period, const WAVEFORMATEX* format, LPCGUID session) {
    bool candidate = nativeRenderer(_ReturnAddress());
    HRESULT hr = originalInit3(client, flags, period, format, session);
    if (candidate) {
        counters.init3Calls++;
        if (SUCCEEDED(hr)) remember(client, format, false); // Client3 does not support RATEADJUST.
        else counters.lastError = hr;
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE serviceHook(IAudioClient* client, REFIID iid, void** out) {
    HRESULT hr = originalService(client, iid, out);
    if (FAILED(hr) || !out || !*out || iid != __uuidof(IAudioRenderClient)) return hr;
    try {
        std::lock_guard<std::mutex> lock(registrationMutex);
        auto* s = findStream(client);
        if (!s) {
            Pending* metadata = nullptr;
            for (auto& p : pending) if (p.client == client) { metadata = &p; break; }
            if (!metadata) return hr; // Already-initialized or out-of-scope client.
            std::atomic<Stream*>* freeSlot = nullptr;
            for (auto& slot : streams) if (!slot.load()) { freeSlot = &slot; break; }
            if (!freeSlot) { counters.capacityReached = 1; return hr; }
            auto state = std::make_unique<Stream>();
            const auto& f = metadata->format.Format;
            state->rate = f.nSamplesPerSec; state->channels = f.nChannels;
            state->bits = f.wBitsPerSample; state->blockAlign = f.nBlockAlign;
            bool floating = false; int validBits = 0;
            state->formatOK = parseFormat(metadata->format, floating, validBits);
            state->rateFlag = metadata->rateFlag;
            if (state->formatOK) {
                UINT32 bufferFrames = 0;
                HRESULT sizeResult = client->GetBufferSize(&bufferFrames);
                if (FAILED(sizeResult) || !bufferFrames || bufferFrames > f.nSamplesPerSec*2) state->formatOK = false;
                else {
                    state->dsp.configure(f.nChannels, f.nSamplesPerSec, f.wBitsPerSample, floating, validBits);
                    state->effects.configure(f.nChannels,f.wBitsPerSample,f.nSamplesPerSec);
                    state->latency = state->dsp.latency();
                    state->scratch.resize(size_t(bufferFrames)*f.nBlockAlign);
                }
            }
            state->client = client; // Retain only a confirmed shared rendering client.
            static_cast<IAudioRenderClient*>(*out)->AddRef(); // Same bounded lifetime as parent; prevents stale alias reuse.
            state->renderers[0] = static_cast<IAudioRenderClient*>(*out);
            s = state.release(); freeSlot->store(s);
            metadata->client = nullptr;
        } else {
            auto* renderer = static_cast<IAudioRenderClient*>(*out);
            for (auto& alias : s->renderers) {
                if (alias.load() == renderer) break;
                if (!alias.load()) { renderer->AddRef(); alias = renderer; break; }
            }
        }
    } catch (...) { counters.errors++; counters.lastError = E_OUTOFMEMORY; }
    return hr;
}
HRESULT STDMETHODCALLTYPE startHook(IAudioClient* client) {
    HRESULT hr = originalStart(client);
    if (auto* s = findStream(client)) { if (SUCCEEDED(hr)) s->active = true; else s->packetError = hr; }
    else if (SUCCEEDED(hr)) counters.untracked++;
    return hr;
}
HRESULT STDMETHODCALLTYPE stopHook(IAudioClient* client) {
    HRESULT hr = originalStop(client);
    if (auto* s = findStream(client)) { counters.stopCalls++; if (SUCCEEDED(hr)) s->active = false; else s->packetError = hr; }
    return hr;
}
HRESULT STDMETHODCALLTYPE resetHook(IAudioClient* client) {
    HRESULT hr = originalReset(client);
    if (auto* s = findStream(client)) { counters.resetCalls++; if (SUCCEEDED(hr)) s->reset = true; else s->packetError = hr; }
    return hr;
}
HRESULT STDMETHODCALLTYPE getBufferHook(IAudioRenderClient* client, UINT32 frames, BYTE** out) {
    HRESULT hr = originalGetBuffer(client, frames, out);
    if (auto* s = findRenderer(client)) {
        if (SUCCEEDED(hr) && frames && out) { s->buffer = *out; s->requestedFrames = frames; }
        else if (FAILED(hr)) { s->packetError = hr; s->reset = true; }
    }
    return hr;
}
HRESULT STDMETHODCALLTYPE releaseBufferHook(IAudioRenderClient* client, UINT32 frames, DWORD flags) {
    auto* s = findRenderer(client);
    if(s) for(int attempt=0;attempt<4;++attempt) {
        uint32_t seq=s->commandSequence.load(); if(seq&1) continue;
        uint64_t command=s->command.load(),effects=s->effectsCommand.load();
        if(seq==s->commandSequence.load()) { s->audioCommand=command; s->audioEffectsCommand=effects; break; }
    }
    uint64_t command = s ? s->audioCommand : 0;
    bool effectsProcessed=false;
    bool sourceSilent=(flags & AUDCLNT_BUFFERFLAGS_SILENT)!=0;
    if (s && frames) {
        try {
            uint32_t bits = uint32_t(command); float factor; memcpy(&factor, &bits, 4);
            if (!std::isfinite(factor) || factor <= 0) factor = 1;
            uint32_t amounts=uint32_t(s->audioEffectsCommand);
            uint32_t rateBits=uint32_t(s->audioEffectsCommand>>32); float effectiveRate;
            memcpy(&effectiveRate,&rateBits,4);
            if(!std::isfinite(effectiveRate) || effectiveRate<=0) effectiveRate=float(s->rate);
            if (s->formatOK && s->reset.exchange(false)) { s->dsp.reset(); s->effects.reset(); }
            if (sourceSilent && s->formatOK && !s->wasSilent) s->dsp.reset();
            if (s->formatOK && !s->fault && s->buffer && frames <= s->requestedFrames) {
                size_t bytes = size_t(frames)*s->blockAlign;
                if (bytes > s->scratch.size()) throw std::bad_alloc();
                if(sourceSilent) memset(s->scratch.data(),0,bytes);
                else {
                    memcpy(s->scratch.data(), s->buffer, bytes);
                    s->dsp.process(s->scratch.data(), frames, factor);
                }
                effectsProcessed=s->effects.process(s->scratch.data(),frames,amounts,effectiveRate,s->dsp);
                memcpy(s->buffer, s->scratch.data(), bytes); // Only commit a successful packet.
                if(sourceSilent && effectsProcessed) flags &= ~AUDCLNT_BUFFERFLAGS_SILENT; // Submit the room's tail.
            }
            s->wasSilent = sourceSilent;
        } catch (...) {
            s->fault = true; s->reset = true; counters.errors++;
        }
        s->buffer = nullptr; s->requestedFrames = 0;
    }
    HRESULT hr = originalReleaseBuffer(client, frames, flags);
    if (s) {
        if (SUCCEEDED(hr) && frames) {
            counters.frames.fetch_add(LONG(frames)); counters.buffers++;
            if (sourceSilent) counters.silentBuffers++;
            // Publish one coherent acknowledgement only AFTER WASAPI accepts it.
            s->packetSequence.fetch_add(1);
            bool processed=s->formatOK && !s->fault;
            uint32_t bits=uint32_t(command); float factor; memcpy(&factor,&bits,4);
            s->acceptedFactor=processed ? s->dsp.acceptedFactor() : 1;
            s->acceptedEffects=processed ? s->effects.accepted() : 0;
            s->transitioning=processed && (!s->dsp.settled() || !s->effects.settled() || (sourceSilent && std::abs(factor-1)>.00001));
            s->wet=processed && s->dsp.wet(); s->effectsWet=processed && s->effects.wet();
            s->effectsLatency=processed ? s->effects.latency() : 0;
            if(effectsProcessed) s->effectsFrames.fetch_add(LONG(frames));
            s->dspSequence = LONG(command >> 32); // Acknowledge only accepted packets.
            s->packetSequence.fetch_add(1);
        } else if (FAILED(hr)) { s->packetError = hr; s->reset = true; }
    }
    return hr;
}
bool queueHook(void* address, void* detour, void** original) {
    MH_STATUS status = MH_CreateHook(address, detour, original);
    if (status == MH_OK) status = MH_QueueEnableHook(address);
    if (status != MH_OK) { shared->lastError = LONG(0x80040000u | unsigned(status)); return false; }
    return true;
}
bool installHooks() {
    ComPtr<IMMDeviceEnumerator> enumerator; ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client; ComPtr<IAudioClient3> client3; ComPtr<IAudioRenderClient> render;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (SUCCEEDED(hr)) client.As(&client3);
    WAVEFORMATEX* format = nullptr;
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, format, nullptr);
    if (format) CoTaskMemFree(format);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&render));
    if (FAILED(hr)) { shared->lastError = hr; return false; }
    auto cv = *reinterpret_cast<void***>(client.Get()); auto rv = *reinterpret_cast<void***>(render.Get());
    if (MH_Initialize() != MH_OK) { shared->lastError = E_FAIL; return false; }
    bool ok = queueHook(cv[3], (void*)initHook, (void**)&originalInit)
        && queueHook(cv[14], (void*)serviceHook, (void**)&originalService)
        && queueHook(cv[10], (void*)startHook, (void**)&originalStart)
        && queueHook(cv[11], (void*)stopHook, (void**)&originalStop)
        && queueHook(cv[12], (void*)resetHook, (void**)&originalReset)
        && queueHook(rv[3], (void*)getBufferHook, (void**)&originalGetBuffer)
        && queueHook(rv[4], (void*)releaseBufferHook, (void**)&originalReleaseBuffer);
    if (ok && client3) {
        auto v3 = *reinterpret_cast<void***>(client3.Get());
        ok = queueHook(v3[20], (void*)init3Hook, (void**)&originalInit3);
    }
    if (!ok) { MH_Uninitialize(); return false; }
    MH_STATUS status = MH_ApplyQueued();
    if (status != MH_OK) { shared->lastError = LONG(0x80040000u | unsigned(status)); MH_Uninitialize(); return false; }
    return true;
}
DWORD WINAPI worker(void*) {
    wchar_t image[MAX_PATH]{}; GetModuleFileNameW(nullptr, image, MAX_PATH);
    const wchar_t* name = wcsrchr(image, L'\\');
    if (!name || _wcsicmp(name + 1, L"Spotify.exe")) return 1;
    spotifyBase = uintptr_t(GetModuleHandleW(L"Spotify.dll"));
    wchar_t key[96]; mappingName(key, 96, GetCurrentProcessId());
    mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(SharedControl), key);
    if (!mapping) return 2;
    bool fresh = GetLastError() != ERROR_ALREADY_EXISTS;
    shared = static_cast<SharedControl*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedControl)));
    if (!shared) return 3;
    if (fresh) {
        ZeroMemory(shared, sizeof(*shared)); shared->magic = kMagic; shared->version = kVersion;
        shared->tempo = 1000; shared->linked = 1; shared->appliedTempo = 1000;
    }
    if (shared->magic != kMagic || shared->version != kVersion) return 4;
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) { shared->lastError = com; return 5; }
    if (!installHooks()) { CoUninitialize(); return 6; }
    shared->attached = 1;
    Settings c; LONG generation = 0;
    for (;;) {
        c = readSettings(shared, c, &generation);
        bool alive = DWORD(GetTickCount() - DWORD(shared->heartbeat)) < 2000;
        Settings requested = c;
        if (!alive || counters.capacityReached) requested.enabled = 0;
        LONG count = 0, active = 0, allApplied = 1, bypassed = 1, transitioning = 0, faulted = 0;
        Stream* selected = nullptr;
        LONG lastError = counters.lastError.load();
        for (auto& slot : streams) {
            auto* s = slot.load(); if (!s) continue;
            ++count; if (!selected) selected = s;
            if (!s->clockAttempted) {
                s->clockAttempted = true;
                if (s->rateFlag) {
                    HRESULT hr = originalService(s->client.Get(), __uuidof(IAudioClockAdjustment), reinterpret_cast<void**>(s->clock.GetAddressOf()));
                    if (FAILED(hr)) s->clockError = hr;
                }
            }
            double rate = requested.enabled && !s->fault ? requested.tempo/1000.0 : 1.0;
            if (!requested.linked && !s->formatOK) rate = 1;
            if (std::abs(rate - s->appliedTempo) > .000001 && s->clock) {
                HRESULT hr = s->clock->SetSampleRate(float(s->rate*rate));
                s->clockError = FAILED(hr) ? hr : 0;
                if (SUCCEEDED(hr)) s->appliedTempo = rate;
            }
            double factor = s->formatOK && !s->fault ? pitchFactor(requested, s->appliedTempo) : 1;
            float f = float(factor); uint32_t bits; memcpy(&bits, &f, 4);
            uint64_t command = (uint64_t(uint32_t(generation)) << 32) | bits;
            uint32_t amounts=s->formatOK && !s->fault ? effectAmounts(requested) : 0;
            float effectiveRate=float(s->rate*s->appliedTempo); uint32_t rateBits; memcpy(&rateBits,&effectiveRate,4);
            uint64_t effects=(uint64_t(rateBits)<<32)|amounts;
            if(s->command.load()!=command || s->effectsCommand.load()!=effects) {
                s->commandSequence.fetch_add(1);
                s->effectsCommand=effects; s->command=command;
                s->commandSequence.fetch_add(1);
            }
            for(int attempt=0;attempt<4;++attempt) {
                uint32_t seq=s->packetSequence.load(); if(seq&1) continue;
                Stream::PacketSnapshot snapshot{s->dspSequence.load(),s->effectsLatency.load(),s->effectsFrames.load(),
                    s->acceptedFactor.load(),s->transitioning.load(),s->wet.load(),s->effectsWet.load(),s->acceptedEffects.load()};
                if(seq==s->packetSequence.load()) { s->acknowledged=snapshot; break; }
            }
            const auto& ack=s->acknowledged;
            bool waiting = ack.generation != generation || ack.transitioning;
            double actualFactor = ack.factor;
            // A clock rollback can change compensation without changing the UI generation.
            if (std::abs(actualFactor - factor) > .00001) waiting = true;
            if(ack.effects!=amounts) waiting=true;
            bool dry = !ack.wet && !ack.effectsWet && std::abs(s->appliedTempo - 1) < .000001;
            bypassed &= LONG(dry);
            if (s->active) {
                ++active; selected = s;
                double targetRate = requested.enabled ? requested.tempo/1000.0 : 1;
                double targetPitch = requested.enabled ? (requested.linked ? 1200*std::log2(targetRate) : requested.pitch) : 0;
                double actualPitch = 1200*std::log2(s->appliedTempo*actualFactor);
                bool applied = !waiting && !s->fault && ack.effects==effectAmounts(requested) &&
                    std::abs(targetRate-s->appliedTempo) < .000001 && std::abs(targetPitch-actualPitch) < .1;
                allApplied &= LONG(applied); transitioning |= LONG(waiting);
            }
            faulted |= s->fault.load() || s->packetError.load() != 0 || s->clockError != 0;
            if (s->packetError) lastError = s->packetError.load();
            if (s->clockError) lastError = s->clockError;
        }
        InterlockedIncrement(&shared->telemetrySequence);
        shared->streams = count; shared->active = active;
        shared->allApplied = active && allApplied && requested.enabled == c.enabled && !faulted && !counters.capacityReached;
        shared->transitioning = transitioning; shared->bypassConfirmed = count && bypassed;
        shared->faulted = faulted; shared->capacityReached = counters.capacityReached.load();
        shared->appliedSequence = generation; shared->untracked = counters.untracked.load();
        shared->frames = counters.frames.load(); shared->buffers = counters.buffers.load();
        shared->silentBuffers = counters.silentBuffers.load(); shared->processingErrors = counters.errors.load();
        shared->initCalls = counters.initCalls.load(); shared->init3Calls = counters.init3Calls.load();
        shared->initFlags = counters.initFlags.load(); shared->stopCalls = counters.stopCalls.load();
        shared->resetCalls = counters.resetCalls.load(); shared->lastError = lastError;
        if (selected) {
            shared->sampleRate = selected->rate; shared->channels = selected->channels; shared->bits = selected->bits;
            shared->formatSupported = selected->formatOK; shared->rateSupported = selected->clock ? 1 : 0;
            shared->appliedTempo = LONG(std::lround(selected->appliedTempo*1000));
            const auto& ack=selected->acknowledged;
            shared->appliedPitch = LONG(std::lround(1200*std::log2(selected->appliedTempo*ack.factor)));
            shared->dspLatency = ack.wet ? selected->latency : 0;
            uint32_t amounts=ack.effects;
            shared->appliedReverb=amounts&255; shared->appliedDistortion=(amounts>>8)&255; shared->appliedMuffle=(amounts>>16)&255;
            shared->fxLatency=ack.latency; shared->fxFrames=ack.frames;
        }
        InterlockedIncrement(&shared->telemetrySequence);
        Sleep(25);
    }
}
}
BOOL WINAPI DllMain(HINSTANCE dll, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(dll);
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    }
    return TRUE;
}
