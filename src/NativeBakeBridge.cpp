#include "NativeBakeBridgeApi.h"
#include "NativeBakeBridge.h"
#include "Config.h"
#include "Logging.h"
#include <Windows.h>
#include <cstring>
#include <string>

namespace {
    HANDLE s_map = nullptr;
    nativebake::Shared* s_shared = nullptr;
    bool s_initialized = false;
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
        s_shared->targetCandidate = static_cast<LONG>(
            Config::GetUInt(L"ReShadeBake", L"TargetCandidate", 0));
        s_shared->lastAppliedSample = -1;
        InterlockedExchange(&s_shared->active, 1);
        logging::InfoF("[BakeFX] Export armed {}x{}: addon={}, apply={}, snapshots={}, targetCandidate={} (0=only verified VideoEncode name)",
            w, h, attached, s_shared->applyEffects,
            s_shared->captureSnapshots, s_shared->targetCandidate);
    }

    void OnVideoSample() {
        if (!s_shared || !s_shared->active) return;
        InterlockedIncrement(&s_shared->submittedSamples);
        InterlockedExchange(&s_shared->firstSampleSeen, 1);
    }

    void End() {
        if (!s_shared || !s_shared->active) return;
        InterlockedExchange(&s_shared->active, 0);
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
