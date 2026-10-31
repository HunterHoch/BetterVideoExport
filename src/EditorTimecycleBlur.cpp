#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "EditorTimecycleBlur.h"
#include "Config.h"
#include "Logging.h"
#include <Hooking.h>
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace
{
    std::atomic<bool> s_exportActive{false};
    std::atomic<bool> s_started{false};
    std::uint8_t* s_overrideFlag = nullptr;
    std::int32_t* s_replayMode = nullptr;
    std::uint8_t s_originalFlag = 0;
    bool s_changed = false;

    std::uint32_t Read32(const void* address)
    {
        std::uint32_t v = 0;
        std::memcpy(&v, address, sizeof(v));
        return v;
    }

    bool IsReadable(const void* addr, std::size_t bytes)
    {
        if (!addr || !bytes) return false;
        MEMORY_BASIC_INFORMATION m{};
        if (!VirtualQuery(addr, &m, sizeof(m)) ||
            m.State != MEM_COMMIT || (m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const auto start = reinterpret_cast<std::uintptr_t>(addr);
        const auto region = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
        return start >= region && start - region <= m.RegionSize &&
               bytes <= m.RegionSize - (start - region);
    }

    bool ReadName(const std::uint8_t* entry, std::string& name)
    {
        std::uintptr_t pointer = 0;
        std::memcpy(&pointer, entry + 8, sizeof(pointer));
        const char* str = reinterpret_cast<const char*>(pointer);
        if (!IsReadable(str, 96)) return false;
        for (std::size_t i = 0; i < 96; ++i)
        {
            if (str[i] == 0)
            {
                if (i == 0) return false;
                name.assign(str, i);
                return true;
            }
            if (static_cast<unsigned char>(str[i]) < 32 ||
                static_cast<unsigned char>(str[i]) > 126)
                return false;
        }
        return false;
    }

    bool LooksLikeTimecycleTable(const std::uint8_t* p)
    {
        // Verified in VYNE's GTA Legacy 3258 research: first retail entries
        // light_dir_col_r default 0.890, light_dir_col_g default 0.675.
        // Never trust this fingerprint alone; validate names and sequential IDs.
        return Read32(p) == 0 && Read32(p + 4) == 0 &&
               Read32(p + 16) == 0x3F63D70A &&
               Read32(p + 20) == 4 && Read32(p + 24) == 1 &&
               Read32(p + 28) == 2 && Read32(p + 32) == 1 &&
               Read32(p + 36) == 2 && Read32(p + 40) == 1 &&
               Read32(p + 44) == 0 && Read32(p + 56) == 0x3F2CCCCD &&
               Read32(p + 60) == 0 && Read32(p + 64) == 1 &&
               Read32(p + 68) == 2;
    }

    bool ScanTimecycleTable()
    {
        const auto module = reinterpret_cast<std::uint8_t*>(GetModuleHandleW(nullptr));
        if (!IsReadable(module, sizeof(IMAGE_DOS_HEADER))) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            dos->e_lfanew > 4096) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(module + dos->e_lfanew);
        if (!IsReadable(nt, sizeof(*nt)) || nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return false;

        const auto imageSize = nt->OptionalHeader.SizeOfImage;
        const auto* section = IMAGE_FIRST_SECTION(nt);
        std::uint8_t* foundFlag = nullptr;
        unsigned tables = 0;

        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        {
            // The table is initialized retail data, not executable instructions.
            if (!(section[s].Characteristics & IMAGE_SCN_MEM_READ)) continue;
            const std::size_t rva = section[s].VirtualAddress;
            if (rva >= imageSize) continue;
            const std::size_t length = std::min<std::size_t>(
                std::max(section[s].Misc.VirtualSize, section[s].SizeOfRawData), imageSize - rva);
            if (length < 72) continue;

            std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(module + rva);
            const std::uintptr_t end = cursor + length;
            while (cursor < end)
            {
                MEMORY_BASIC_INFORMATION m{};
                if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &m, sizeof(m)) ||
                    m.RegionSize == 0) break;
                const std::uintptr_t regionEnd = std::min<std::uintptr_t>(
                    end, reinterpret_cast<std::uintptr_t>(m.BaseAddress) + m.RegionSize);
                if (regionEnd <= cursor) break;
                if (m.State == MEM_COMMIT && !(m.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                {
                    for (auto address = cursor; address + 72 <= regionEnd; ++address)
                    {
                        auto* p = reinterpret_cast<std::uint8_t*>(address);
                        if (!LooksLikeTimecycleTable(p)) continue;
                        std::string name0, name1;
                        if (!ReadName(p, name0) || !ReadName(p + 32, name1) ||
                            name0 != "light_dir_col_r" || name1 != "light_dir_col_g")
                            continue;

                        ++tables;
                        unsigned n = 0;
                        for (; n < 1024 && address + (n + 1) * 32 <= regionEnd; ++n)
                            if (Read32(p + n * 32) != n) break;
                        if (n < 64 || n >= 1024) continue;
                        logging::InfoF("[TimecycleBlur] Validated timecycle table with {} sequential variables", n);

                        int targetCount = 0;
                        for (unsigned i = 0; i < n; ++i)
                        {
                            std::string name;
                            if (!ReadName(p + i * 32, name)) continue;
                            if (name.find("motionblur") != std::string::npos)
                                logging::InfoF("[TimecycleBlur] Motion-blur timecycle variable {}: {}", i, name);
                            if (name == "postfx_motionblurlength")
                            {
                                foundFlag = p + i * 32 + 0x18;
                                ++targetCount;
                            }
                        }
                        if (targetCount != 1)
                            logging::WarningF("[TimecycleBlur] Target postfx_motionblurlength count={} in timecycle table; will not patch ambiguous table", targetCount);
                    }
                }
                cursor = regionEnd;
            }
        }

        if (tables != 1 || !foundFlag || !IsReadable(foundFlag, 1))
        {
            logging::WarningF("[TimecycleBlur] Table/variable identification not unique (table candidates={}); intervention disabled", tables);
            return false;
        }
        s_overrideFlag = foundFlag;
        logging::InfoF("[TimecycleBlur] Target replay-override flag identified; current value={}", (unsigned)*foundFlag);
        return true;
    }

    void FindReplayMode()
    {
        // Legacy camReplayDirector::GetMaxDistanceAllowedFromPlayer.
        // This is the independently reverse-engineered VYNE 3258 fingerprint.
        // Do NOT assume it survives FiveM 3788: reject a missing/multiple match.
        auto matches = hook::pattern(
            "83 3D ? ? ? ? 02 4C 8B 81 F0 02 00 00 F3 41 0F 10 40 38 "
            "75 1D 84 D2 74 19 8B 81 30 06 00 00 83 F8 FF 74 0E 39 05");
        if (matches.size() != 1)
        {
            logging::WarningF("[TimecycleBlur] Replay editor mode signature matches={} (need 1); export-only fallback", matches.size());
            return;
        }

        auto* instruction = matches.get(0).get<std::uint8_t>();
        std::int32_t disp = 0;
        std::memcpy(&disp, instruction + 2, sizeof(disp));
        auto* ptr = reinterpret_cast<std::int32_t*>(instruction + 7 + disp);
        if (!IsReadable(ptr, sizeof(std::int32_t)) || *ptr < 0 || *ptr > 4)
        {
            logging::Warning("[TimecycleBlur] Replay mode pointer failed validation; export-only fallback");
            return;
        }
        s_replayMode = ptr;
        logging::InfoF("[TimecycleBlur] Replay mode global resolved (mode={})", *ptr);
    }

    bool SetFlag(std::uint8_t value)
    {
        if (!s_overrideFlag || !IsReadable(s_overrideFlag, 1)) return false;
        if (*s_overrideFlag == value) return true;
        DWORD protection = 0;
        if (!VirtualProtect(s_overrideFlag, 1, PAGE_READWRITE, &protection))
        {
            logging::ErrorF("[TimecycleBlur] VirtualProtect on replay flag failed: {}", GetLastError());
            return false;
        }
        *s_overrideFlag = value;
        DWORD ignored = 0;
        VirtualProtect(s_overrideFlag, 1, protection, &ignored);
        return true;
    }

    DWORD WINAPI Monitor(void*)
    {
        for (;;)
        {
            bool editor = false;
            if (s_replayMode && IsReadable(s_replayMode, sizeof(std::int32_t)))
            {
                const int mode = *s_replayMode;
                editor = (mode == 2 || mode == 3); // EDIT / transient LOADCLIP
            }
            const bool active = editor || s_exportActive.load();
            if (active && !s_changed && s_overrideFlag)
            {
                s_originalFlag = *s_overrideFlag;
                // If the original is already 0, log that we didn't change it.
                if (SetFlag(0))
                {
                    s_changed = true;
                    logging::InfoF("[TimecycleBlur] Editor active: replay override disabled ONLY for postfx_motionblurlength (original={})",
                                   (unsigned)s_originalFlag);
                }
            }
            else if (!active && s_changed)
            {
                if (SetFlag(s_originalFlag))
                {
                    s_changed = false;
                    logging::Info("[TimecycleBlur] Editor inactive: original replay flag restored; future recordings unaffected");
                }
            }
            else if (active && s_changed && s_overrideFlag && *s_overrideFlag != 0)
            {
                SetFlag(0);
            }
            Sleep(16);
        }
    }
}

namespace EditorTimecycleBlur
{
    void SetExportActive(bool active)
    {
        s_exportActive.store(active);
        logging::InfoF("[TimecycleBlur] Export state: {}", active ? "START" : "END");
    }

    void Initialize()
    {
        if (Config::GetUInt(L"MotionBlur", L"EditorTimecycleBlur", 1) == 0)
        {
            logging::Info("[TimecycleBlur] Disabled by INI");
            return;
        }
        if (s_started.exchange(true)) return;
        if (!ScanTimecycleTable())
        {
            logging::Warning("[TimecycleBlur] No safe target found; stock timecycle behavior unchanged");
            return;
        }
        FindReplayMode();
        HANDLE worker = CreateThread(nullptr, 0, &Monitor, nullptr, 0, nullptr);
        if (!worker)
        {
            logging::ErrorF("[TimecycleBlur] Could not start monitor: {}", GetLastError());
            return;
        }
        CloseHandle(worker);
        logging::Info("[TimecycleBlur] One-variable replay timecycle override installed (no extra renders)");
    }
}

static HookFunction hookEditorTimecycle([]()
{
    EditorTimecycleBlur::Initialize();
});
