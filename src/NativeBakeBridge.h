#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <cstdint>
#include <cstddef>

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
    // Independent high-bandwidth GPU-frame transport; metadata bridge stays
    // small. At most one 4K BGRA8 frame (33.2 MB) is shared between ReShade
    // and the FFmpeg sink writer. Protected by nonblocking Interlocked lock.
    inline constexpr DWORD FrameMagic = 0x4D524656; // "VFRM"
    inline constexpr DWORD FrameVersion = 1;
    inline constexpr wchar_t FrameMappingName[] = L"Local\\VYNE_BakeFX_Frame_1";
    inline constexpr DWORD MaxFrameWidth = 3840;
    inline constexpr DWORD MaxFrameHeight = 2160;
    inline constexpr size_t MaxBgraBytes =
        static_cast<size_t>(MaxFrameWidth) * MaxFrameHeight * 4;

    struct FrameShared {
        DWORD magic = FrameMagic;
        DWORD version = FrameVersion;
        volatile LONG busy = 0;
        volatile LONG ready = 0;
        volatile LONG sequence = 0;
        volatile LONG sampleNumber = 0;
        volatile LONG width = 0;
        volatile LONG height = 0;
        volatile LONG bytes = 0;
        volatile LONG published = 0;
        volatile LONG consumed = 0;
        volatile LONG skipped = 0;
        uint8_t bgra[MaxBgraBytes]{};
    };
    static_assert(offsetof(FrameShared, bgra) == 48, "GPU frame ABI alignment mismatch");
        static_assert(sizeof(Candidate) == 96, "ASI/addon Candidate ABI mismatch");
}
