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
        MEMORY_BASIC_INFORMATION memory{};
        if (!str || !VirtualQuery(str, &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
            return false;
        const auto start = reinterpret_cast<std::uintptr_t>(str);
        const auto end = reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize;
        const std::size_t limit = std::min<std::size_t>(96, end - start);
        for (std::size_t i = 0; i < limit; ++i)
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

    // Conservative and general across GTA builds: the first two timecycle
    // variables have stable IDs/names, but default float values may change.
    // A signature using 0.890/0.675 was identified on VYNE's older build;
    // requiring the same float values on FiveM 3788 gave zero candidates.
    bool LooksLikeTimecycleTable(const std::uint8_t* p)
    {
        return Read32(p) == 0 && Read32(p + 32) == 1;
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
        const auto* sections = IMAGE_FIRST_SECTION(nt);
        std::uint8_t* foundFlag = nullptr;
        unsigned tables = 0;
        unsigned validTables = 0;
        unsigned tablesWithTarget = 0;
        unsigned namePairs = 0;
        unsigned blurNames = 0;

        for (unsigned s = 0; s < nt->FileHeader.NumberOfSections; ++s)
        {
            // Scan initialized, readable non-code sections of the active GTA PE.
            if (!(sections[s].Characteristics & IMAGE_SCN_MEM_READ) ||
                (sections[s].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
            const std::size_t rva = sections[s].VirtualAddress;
            if (rva >= imageSize) continue;
            const std::size_t length = std::min<std::size_t>(
                std::max(sections[s].Misc.VirtualSize, sections[s].SizeOfRawData), imageSize - rva);
            if (length < 72) continue;

            std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(module + rva);
            const std::uintptr_t end = cursor + length;
            while (cursor < end)
            {
                MEMORY_BASIC_INFORMATION memory{};
                if (!VirtualQuery(reinterpret_cast<const void*>(cursor), &memory, sizeof(memory)) ||
                    !memory.RegionSize) break;
                const std::uintptr_t regionEnd = std::min<std::uintptr_t>(
                    end, reinterpret_cast<std::uintptr_t>(memory.BaseAddress) + memory.RegionSize);
                if (regionEnd <= cursor) break;
                const DWORD access = memory.Protect & 0xff;
                const bool readable = (access == PAGE_READONLY || access == PAGE_READWRITE ||
                    access == PAGE_WRITECOPY || access == PAGE_EXECUTE_READ ||
                    access == PAGE_EXECUTE_READWRITE || access == PAGE_EXECUTE_WRITECOPY);
                if (memory.State == MEM_COMMIT && readable &&
                    !(memory.Protect & (PAGE_GUARD | PAGE_NOACCESS)))
                {
                    // Each entry contains 64-bit pointers; only consider aligned
                    // candidates. Avoid expensive pointer checks on every byte.
                    auto address = (cursor + 7) & ~std::uintptr_t(7);
                    for (; address + 72 <= regionEnd; address += 8)
                    {
                        auto* p = reinterpret_cast<std::uint8_t*>(address);
                        if (!LooksLikeTimecycleTable(p)) continue;
                        std::string name0, name1;
                        if (!ReadName(p, name0) || name0 != "light_dir_col_r" ||
                            !ReadName(p + 32, name1) || name1 != "light_dir_col_g")
                            continue;
                        ++namePairs;

                        unsigned n = 0;
                        for (; n < 1024 && address + (n + 1) * 32 <= regionEnd; ++n)
                            if (Read32(p + n * 32) != n) break;
                        if (n < 64 || n >= 1024) continue;
                        ++validTables;
                        int matches = 0;
                        std::uint8_t* target = nullptr;
                        for (unsigned i = 0; i < n; ++i)
                        {
                            std::string name;
                            if (!ReadName(p + i * 32, name)) continue;
                            if (name.find("motionblur") != std::string::npos)
                            {
                                ++blurNames;
                                if (blurNames <= 12)
                                    logging::InfoF("[TimecycleBlur] Registry blur variable [{}] {}", i, name);
                            }
                            if (name == "postfx_motionblurlength")
                            {
                                target = p + i * 32 + 0x18;
                                ++matches;
                            }
                        }
                        ++tables;
                        if (matches == 1)
                        {
                            ++tablesWithTarget;
                            foundFlag = target;
                            logging::InfoF("[TimecycleBlur] Table found, {} entries; motion blur replay flag value {}",
                                           n, (unsigned)*target);
                        }
                        else
                            logging::WarningF("[TimecycleBlur] Found {}-entry timecycle table but target variable count={}",
                                              n, matches);
                    }
                }
                cursor = regionEnd;
            }
        }

        logging::InfoF("[TimecycleBlur] Scan results: name pairs={}, valid tables={}, registry blur variables={}, target tables={}",
                       namePairs, validTables, blurNames, tablesWithTarget);
        if (tables != 1 || tablesWithTarget != 1 ||
            !foundFlag || !IsReadable(foundFlag, 1))
        {
            logging::Warning("[TimecycleBlur] No unique verified variable. No memory changes applied.");
            return false;
        }
        s_overrideFlag = foundFlag;
        logging::InfoF("[TimecycleBlur] Verified postfx_motionblurlength replay flag; original={}",
                       (unsigned)*foundFlag);
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
        // This thread lives for the process lifetime. The table can be absent
        // during plugin load, so resolve only after the Editor/export starts.
        unsigned retries = 0;
        unsigned seenEpoch = 0;
        bool previousActive = false;
        ULONGLONG nextRetry = 0;

        for (;;)
        {
            bool editor = false;
            if (s_replayMode && IsReadable(s_replayMode, sizeof(std::int32_t)))
            {
                const int mode = *s_replayMode;
                editor = (mode == 2 || mode == 3); // EDIT / transient LOADCLIP
            }
            const bool active = editor || s_exportActive.load();
            if (active && !previousActive)
            {
                ++seenEpoch;
                retries = 0;
                nextRetry = 0;
                logging::InfoF("[TimecycleBlur] Editor/export active (epoch {}): trying live registry lookup",
                                seenEpoch);
            }

            // Attempt once immediately and up to 4 more times, two seconds
            // apart, if the table isn't populated or differs on this build.
            if (active && !s_overrideFlag && retries < 5 && GetTickCount64() >= nextRetry)
            {
                ++retries;
                logging::InfoF("[TimecycleBlur] Runtime timecycle scan {}/5", retries);
                if (!ScanTimecycleTable())
                {
                    nextRetry = GetTickCount64() + 2000;
                    if (retries == 5)
                        logging::Warning("[TimecycleBlur] No verified table after 5 attempts; leaving export unmodified");
                }
            }

            if (active && !s_changed && s_overrideFlag)
            {
                s_originalFlag = *s_overrideFlag;
                if (SetFlag(0))
                {
                    s_changed = true;
                    logging::InfoF("[TimecycleBlur] ACTIVE: live postfx_motionblurlength kept in Editor, saved flag={}",
                                   (unsigned)s_originalFlag);
                }
            }
            else if (!active && s_changed)
            {
                if (SetFlag(s_originalFlag))
                {
                    s_changed = false;
                    logging::Info("[TimecycleBlur] Restored original replay flag outside Editor");
                }
            }
            else if (active && s_changed && s_overrideFlag && *s_overrideFlag != 0)
            {
                SetFlag(0);
            }
            previousActive = active;
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
        FindReplayMode();
        // Do NOT scan the timecycle table at ASI startup. GTA may not have
        // initialized it; the monitor resolves it in Editor/export instead.
        HANDLE worker = CreateThread(nullptr, 0, &Monitor, nullptr, 0, nullptr);
        if (!worker)
        {
            logging::ErrorF("[TimecycleBlur] Could not start monitor: {}", GetLastError());
            return;
        }
        CloseHandle(worker);
        logging::Info("[TimecycleBlur] Deferred resolver armed; lookup will run only in Editor/export");
    }
}

static HookFunction hookEditorTimecycle([]()
{
    EditorTimecycleBlur::Initialize();
});
