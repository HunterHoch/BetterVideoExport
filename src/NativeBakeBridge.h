#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdint>

// Identical POD layout in BetterVideoExport.asi and VYNE_BakeFX.addon64.
// The ASI owns the mapping. Add-on uses OpenFileMappingW, never resets it.
namespace nativebake {
    inline constexpr DWORD Magic = 0x58464256; // "VBFX"
    inline constexpr DWORD Version = 3;
    inline constexpr wchar_t MappingName[] = L"Local\\VYNE_BakeFX_3";
    inline constexpr unsigned MaxCandidates = 32;

    struct Candidate {
        std::uint64_t resource = 0;
        volatile LONG width = 0;
        volatile LONG height = 0;
        volatile LONG format = 0;
        volatile LONG bindCount = 0;
        volatile LONG appliedCount = 0;
        char debugName[64]{};
        volatile LONG snapshotStatus = 0; // 0=not attempted, 1=saved, -1=failed
    };

    struct Shared {
        DWORD magic = Magic;
        DWORD version = Version;
        volatile LONG active = 0;         // One only between BeginWriting and Finalize
        volatile LONG firstSampleSeen = 0;// Gates out pre-export menus and blue frames
        volatile LONG width = 0;
        volatile LONG height = 0;
        volatile LONG applyEffects = 0;   // Default diagnostic only
        volatile LONG captureSnapshots = 0; // opt-in single-frame GPU evidence
        volatile LONG opticalBlur = 0; // custom motion estimation + directional shutter blur
        volatile LONG targetCandidate = 0;// 1-based candidate, zero = auto only if name=VideoEncode
        volatile LONG submittedSamples = 0;
        volatile LONG reshadeAttached = 0;
        volatile LONG candidateCount = 0;
        volatile LONG eligibleBinds = 0;
        volatile LONG effectsApplied = 0;
        volatile LONG effectsErrors = 0;
        volatile LONG presentCount = 0;
        volatile LONG skippedBackbuffer = 0;
        volatile LONG autoTarget = 0;     // 1-based if debug name proves VideoEncode
        volatile LONG lastAppliedSample = -1;
        Candidate candidates[MaxCandidates]{};
    };
    static_assert(sizeof(Candidate) == 96, "ASI/addon Candidate ABI mismatch");
}
