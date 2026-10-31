#include "NativeBakeBridgeApi.h"
#include "NativeBakeBridge.h"
#include "Config.h"
#include "Logging.h"
#include <Windows.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <cstdint>
#include <vector>

namespace {
    HANDLE s_map = nullptr;
    nativebake::Shared* s_shared = nullptr;
    bool s_initialized = false;
    HANDLE s_gpuMap = nullptr;
    nativebake::FrameShared* s_gpu = nullptr;
    LONG s_lastSequence = 0;
    LONG s_consumedFrames = 0;
    LONG s_fallbackFrames = 0;
    unsigned char ClampByte(int v) {
        return static_cast<unsigned char>(v < 0 ? 0 : (v > 255 ? 255 : v));
    }
    // Convert ReShade's published BGRA8 output into the same tightly-packed
    // BT.709 limited NV12 layout that the stock FFmpeg shim already expects.
    // Prior frame values are never composited here.
    void ToNV12(const std::vector<uint8_t>& bgra, unsigned w, unsigned h,
                std::vector<uint8_t>& out) {
        out.resize(static_cast<size_t>(w) * h * 3 / 2);
        uint8_t* yPlane = out.data();
        uint8_t* uvPlane = out.data() + static_cast<size_t>(w) * h;
        for (unsigned y = 0; y < h; y += 2) {
            for (unsigned x = 0; x < w; x += 2) {
                int r = 0, g = 0, b = 0;
                for (unsigned dy = 0; dy < 2; ++dy) {
                    for (unsigned dx = 0; dx < 2; ++dx) {
                        const size_t i = (static_cast<size_t>(y + dy) * w + x + dx) * 4;
                        const int pb = bgra[i], pg = bgra[i + 1], pr = bgra[i + 2];
                        yPlane[static_cast<size_t>(y + dy) * w + x + dx] =
                            ClampByte(16 + ((47*pr + 157*pg + 16*pb + 128) >> 8));
                        r += pr; g += pg; b += pb;
                    }
                }
                r = (r + 2) >> 2; g = (g + 2) >> 2; b = (b + 2) >> 2;
                const size_t pos = static_cast<size_t>(y/2) * w + x;
                uvPlane[pos]     = ClampByte(128 + ((-26*r - 87*g + 113*b + 128) >> 8));
                uvPlane[pos + 1] = ClampByte(128 + ((113*r - 103*g - 10*b + 128) >> 8));
            }
        }
    }
}
namespace NativeBakeBridge {
    void Initialize() {
        if (s_initialized) return;
        s_initialized = true;
        if (Config::GetUInt(L"ReShadeBake", L"Enable", 0) == 0) {
            logging::Info("[BakeFX] Disabled; opt in with [ReShadeBake] Enable=1");
            return;
        }
        s_map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                   sizeof(nativebake::Shared), nativebake::MappingName);
        if (!s_map) {
            logging::ErrorF("[BakeFX] CreateFileMapping failed: {}", GetLastError());
            return;
        }
        const DWORD already = GetLastError();
        s_shared = static_cast<nativebake::Shared*>(
            MapViewOfFile(s_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(nativebake::Shared)));
        if (!s_shared) {
            logging::ErrorF("[BakeFX] MapViewOfFile failed: {}", GetLastError());
            CloseHandle(s_map); s_map = nullptr;
            return;
        }
        if (already == ERROR_ALREADY_EXISTS &&
            (s_shared->magic != nativebake::Magic || s_shared->version != nativebake::Version)) {
            logging::Error("[BakeFX] A different bridge version exists; disabling");
            UnmapViewOfFile(s_shared); s_shared = nullptr;
            CloseHandle(s_map); s_map = nullptr;
            return;
        }
        // Only this ASI creates the mapping. Reset any stale local-process state.
        std::memset(s_shared, 0, sizeof(*s_shared));
        s_shared->magic = nativebake::Magic;
        s_shared->version = nativebake::Version;
        s_shared->lastAppliedSample = -1;
        if (Config::GetUInt(L"ReShadeBake", L"ReplaceVideoSamples", 1) != 0) {
            s_gpuMap = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
                PAGE_READWRITE, static_cast<DWORD>(
                    (sizeof(nativebake::FrameShared) >> 32) & 0xFFFFFFFFu),
                static_cast<DWORD>(sizeof(nativebake::FrameShared) & 0xFFFFFFFFu),
                nativebake::FrameMappingName);
            if (s_gpuMap) {
                s_gpu = static_cast<nativebake::FrameShared*>(
                    MapViewOfFile(s_gpuMap, FILE_MAP_ALL_ACCESS, 0, 0,
                                  sizeof(nativebake::FrameShared)));
                if (s_gpu) {
                    s_gpu->magic = nativebake::FrameMagic;
                    s_gpu->version = nativebake::FrameVersion;
                    s_gpu->busy = 0;
                    s_gpu->ready = 0;
                    s_gpu->sequence = 0;
                    logging::Info("[BakeFX] 4K GPU frame transport initialized: effect output can REPLACE original NV12");
                }
                else {
                    CloseHandle(s_gpuMap); s_gpuMap = nullptr;
                    logging::Warning("[BakeFX] GPU frame transport mapping failed; normal NV12 fallback");
                }
            } else logging::Warning("[BakeFX] GPU frame mapping allocation failed; normal NV12 fallback");
        }
        logging::Info("[BakeFX] Native bake bridge created; awaiting ReShade add-on attachment");
    }

    void Begin(unsigned w, unsigned h) {
        if (!s_shared) return;
        InterlockedExchange(&s_shared->active, 0);
        const LONG attached = s_shared->reshadeAttached;
        std::memset(s_shared, 0, sizeof(*s_shared));
        s_shared->magic = nativebake::Magic;
        s_shared->version = nativebake::Version;
        s_shared->reshadeAttached = attached;
        s_shared->width = static_cast<LONG>(w);
        s_shared->height = static_cast<LONG>(h);
        s_shared->applyEffects = static_cast<LONG>(
            Config::GetUInt(L"ReShadeBake", L"ApplyEffects", 0));
        s_shared->captureSnapshots = static_cast<LONG>(
            Config::GetUInt(L"ReShadeBake", L"CaptureSnapshots", 0));
        s_shared->opticalBlur = static_cast<LONG>(
            Config::GetUInt(L"ReShadeBake", L"OpticalBlur", 0));
        s_shared->targetCandidate = static_cast<LONG>(
            Config::GetUInt(L"ReShadeBake", L"TargetCandidate", 0));
        s_shared->lastAppliedSample = -1;
        s_lastSequence = 0; s_consumedFrames = 0; s_fallbackFrames = 0;
        if (s_gpu) {
            InterlockedExchange(&s_gpu->ready, 0);
            InterlockedExchange(&s_gpu->sequence, 0);
            InterlockedExchange(&s_gpu->published, 0);
            InterlockedExchange(&s_gpu->consumed, 0);
            InterlockedExchange(&s_gpu->skipped, 0);
        }
        InterlockedExchange(&s_shared->active, 1);
        logging::InfoF("[BakeFX] Export armed {}x{}: addon={}, apply={}, snapshots={}, opticalBlur={}, targetCandidate={}",
            w, h, attached, s_shared->applyEffects,
            s_shared->captureSnapshots, s_shared->opticalBlur, s_shared->targetCandidate);
    }

    void OnVideoSample() {
        if (!s_shared || !s_shared->active) return;
        InterlockedIncrement(&s_shared->submittedSamples);
        InterlockedExchange(&s_shared->firstSampleSeen, 1);
    }


    bool ConsumeNV12(std::vector<std::uint8_t>& out, unsigned w, unsigned h) {
        if (!s_shared || !s_shared->active || !s_gpu ||
            !s_gpu->ready || !s_shared->applyEffects ||
            w == 0 || h == 0 || (w & 1) || (h & 1)) return false;
        if (InterlockedCompareExchange(&s_gpu->busy, 1, 0) != 0) {
            ++s_fallbackFrames;
            return false;
        }
        const LONG seq = s_gpu->sequence;
        const LONG frameSample = s_gpu->sampleNumber;
        const LONG thisSample = s_shared->submittedSamples;
        const size_t byteCount = static_cast<size_t>(w) * h * 4;
        bool good = seq > s_lastSequence && s_gpu->ready &&
                    s_gpu->magic == nativebake::FrameMagic &&
                    s_gpu->version == nativebake::FrameVersion &&
                    s_gpu->width == static_cast<LONG>(w) &&
                    s_gpu->height == static_cast<LONG>(h) &&
                    s_gpu->bytes == static_cast<LONG>(byteCount) &&
                    frameSample > 0 && frameSample <= thisSample &&
                    (thisSample - frameSample) <= 2;
        static thread_local std::vector<std::uint8_t> scratch;
        if (good) {
            scratch.resize(byteCount);
            std::memcpy(scratch.data(), s_gpu->bgra, byteCount);
            s_lastSequence = seq;
            InterlockedIncrement(&s_gpu->consumed);
        }
        InterlockedExchange(&s_gpu->busy, 0);
        if (!good) {
            ++s_fallbackFrames;
            return false;
        }
        ToNV12(scratch, w, h, out);
        const LONG count = ++s_consumedFrames;
        if (count == 1 || count % 120 == 0)
            logging::InfoF("[BakeFX] ACTUAL post-ReShade GPU frame sent to NVENC: #{} (GPU seq={}, captured after sample={}, writer sample={})",
                count, seq, frameSample, thisSample);
        return true;
    }

    void End() {
        if (!s_shared || !s_shared->active) return;
        InterlockedExchange(&s_shared->active, 0);
        logging::InfoF("[BakeFX] GPU-to-encoder: processed NV12 frames={}, native fallbacks={}, GPU publications={}, dropped GPU frames={}",
            s_consumedFrames, s_fallbackFrames,
            s_gpu ? s_gpu->published : 0, s_gpu ? s_gpu->skipped : 0);
        logging::InfoF("[BakeFX] Export: samples={}, addon={}, presents={}, candidate resources={}, eligible binds={}, effect passes={}, errors={}, autoTarget={}",
            s_shared->submittedSamples, s_shared->reshadeAttached,
            s_shared->presentCount, s_shared->candidateCount,
            s_shared->eligibleBinds, s_shared->effectsApplied,
            s_shared->effectsErrors, s_shared->autoTarget);
        const unsigned count = static_cast<unsigned>(
            s_shared->candidateCount < 0 ? 0 :
            s_shared->candidateCount > static_cast<LONG>(nativebake::MaxCandidates)
            ? nativebake::MaxCandidates : s_shared->candidateCount);
        for (unsigned i = 0; i < count; ++i) {
            const auto& item = s_shared->candidates[i];
            logging::InfoF("[BakeFX] Candidate {}: GPU=0x{:016X}, {}x{}, fmt={}, binds={}, effects={}, snapshot={}, name={}",
                i+1, item.resource, item.width, item.height, item.format,
                item.bindCount, item.appliedCount, item.snapshotStatus, item.debugName[0] ? item.debugName : "<unnamed>");
        }
    }
}
