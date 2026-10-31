
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "EditorExtraTimecycle.h"
#include "Config.h"
#include "Logging.h"
#include <Windows.h>
#include <TlHelp32.h>
#include <cstdint>
#include <cstring>
#include <cstddef>

namespace
{
    using NativeHandler = void (__cdecl*)(void*);
    using NativeResolver = NativeHandler (__cdecl*)(std::uint64_t);
    using ActiveThreadFn = void* (__cdecl*)();

    NativeResolver s_resolver = nullptr;
    ActiveThreadFn s_thread = nullptr;
    NativeHandler s_setExtra = nullptr;
    NativeHandler s_clearExtra = nullptr;
    NativeHandler s_getExtra = nullptr;
    NativeHandler s_force = nullptr;
    bool s_resolved = false;
    bool s_enabled = false;
    bool s_active = false;
    bool s_owned = false;
    bool s_forced = false;
    bool s_missingThreadLogged = false;
    bool s_conflictLogged = false;
    int s_ownIndex = -1;
    unsigned s_pulses = 0;

    // Matches published FiveM scrNativeCallContext field offsets.
    // Scratch space is oversized; these calls don't use vector return values.
    struct alignas(16) Context
    {
        std::uint64_t* result = nullptr;
        std::uint32_t argCount = 0;
        std::uint32_t pad0 = 0;
        std::uint64_t* args = nullptr;
        std::uint32_t dataCount = 0;
        std::uint32_t pad1 = 0;
        std::uint8_t scratch[256]{};
    };
    static_assert(offsetof(Context, result) == 0 && offsetof(Context, argCount) == 8 &&
                  offsetof(Context, args) == 16 && offsetof(Context, dataCount) == 24);

    // Contain invalid native handler SEH faults; don't take the encoder down.
    bool Call(NativeHandler handler, std::uint64_t arg, bool withArg, std::uint64_t& out)
    {
        if (!handler) return false;
        std::uint64_t args[4]{};
        std::uint64_t returns[4]{};
        args[0] = arg;
        Context ctx{};
        ctx.result = returns;
        ctx.args = args;
        ctx.argCount = withArg ? 1 : 0;
        __try { handler(&ctx); }
        __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
        out = returns[0];
        return true;
    }

    FARPROC FindExport(HMODULE module, const char* symbolPart)
    {
        if (!module) return nullptr;
        const auto* base = reinterpret_cast<const std::uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0 ||
            dos->e_lfanew > 4096) return nullptr;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) return nullptr;
        const DWORD size = nt->OptionalHeader.SizeOfImage;
        const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!directory.VirtualAddress || directory.VirtualAddress >= size ||
            directory.Size > size - directory.VirtualAddress) return nullptr;
        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            base + directory.VirtualAddress);
        if (!exports->AddressOfNames || exports->AddressOfNames >= size ||
            exports->NumberOfNames > (size - exports->AddressOfNames) / 4) return nullptr;
        const DWORD* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        for (DWORD i = 0; i < exports->NumberOfNames; ++i)
        {
            const DWORD rva = names[i];
            if (!rva || rva >= size) continue;
            const char* name = reinterpret_cast<const char*>(base + rva);
            if (!std::memchr(name, 0, size - rva)) continue;
            if (std::strstr(name, symbolPart))
                return GetProcAddress(module, name);
        }
        return nullptr;
    }

    bool Resolve()
    {
        if (s_resolved) return s_resolver && s_setExtra && s_getExtra && s_clearExtra && s_force;
        s_resolved = true;
        HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE |
            TH32CS_SNAPMODULE32, GetCurrentProcessId());
        if (snapshot == INVALID_HANDLE_VALUE) return false;
        MODULEENTRY32W m{}; m.dwSize = sizeof(m);
        if (Module32FirstW(snapshot, &m))
        {
            do {
                FARPROC proc = FindExport(m.hModule, "GetNativeHandler@scrEngine@rage@@");
                if (proc)
                {
                    s_resolver = reinterpret_cast<NativeResolver>(proc);
                    s_thread = reinterpret_cast<ActiveThreadFn>(
                        FindExport(m.hModule, "GetActiveThread@scrEngine@rage@@"));
                    logging::InfoF("[ExtraTC] Found FiveM native bridge; active script-thread resolver={}",
                        s_thread ? "yes" : "no");
                    break;
                }
            } while (Module32NextW(snapshot, &m));
        }
        CloseHandle(snapshot);
        if (!s_resolver)
        {
            logging::Warning("[ExtraTC] FiveM scripting native exports unavailable; no calls made");
            return false;
        }
        // Original GTA hashes: FiveM scrEngine maps to current registrations.
        s_setExtra = s_resolver(0x5096FD9CCB49056DULL);
        s_clearExtra = s_resolver(0x92CCC17A7A2285DAULL);
        s_getExtra = s_resolver(0xBB0527EC6341496DULL);
        s_force = s_resolver(0x6A51F78772175A51ULL);
        if (!s_setExtra || !s_clearExtra || !s_getExtra || !s_force)
        {
            logging::Warning("[ExtraTC] GTA graphics native handlers missing; no calls made");
            return false;
        }
        logging::Info("[ExtraTC] All 4 graphics native handlers resolved");
        return true;
    }

    void Pulse()
    {
        if (!s_active) return;
        if (s_thread && !s_thread() && !s_missingThreadLogged)
        {
            s_missingThreadLogged = true;
            logging::Warning("[ExtraTC] No script TLS on export callback; testing global graphics setters");
        }
        std::uint64_t out = 0;
        if (!Call(s_getExtra, 0, false, out))
        {
            logging::Error("[ExtraTC] Failed reading extra timecycle state; stopping experiment");
            s_active = false;
            return;
        }
        const int current = static_cast<int>(out);
        if (!s_owned && current != -1)
        {
            if (!s_conflictLogged)
            {
                s_conflictLogged = true;
                logging::WarningF("[ExtraTC] Existing extra modifier {} detected; won't overwrite it", current);
            }
            return;
        }
        if (s_owned && current != -1 && current != s_ownIndex)
        {
            logging::Warning("[ExtraTC] Another component changed extra modifier; yielding");
            s_active = false;
            s_owned = false;
            return;
        }
        if (current == -1)
        {
            if (!Call(s_setExtra, reinterpret_cast<std::uint64_t>("nextgen"), true, out))
            {
                logging::Error("[ExtraTC] SET_EXTRA_TCMODIFIER failed");
                s_active = false;
                return;
            }
            s_owned = true;
            if (!Call(s_getExtra, 0, false, out))
            {
                logging::Error("[ExtraTC] Could not verify extra modifier after setting");
                s_active = false;
                return;
            }
            s_ownIndex = static_cast<int>(out);
            logging::InfoF("[ExtraTC] Activated nextgen extra timecycle index={}", s_ownIndex);
        }
        if (!Call(s_force, 1, true, out))
        {
            logging::Error("[ExtraTC] SET_FORCE_MOTIONBLUR failed");
            s_active = false;
            return;
        }
        s_forced = true;
        if (++s_pulses == 1)
            logging::Info("[ExtraTC] Native nextgen modifier and force-motionblur applied");
    }
}

namespace EditorExtraTimecycle
{
    void Begin()
    {
        s_enabled = Config::GetUInt(L"MotionBlur", L"EditorExtraTimecycle", 0) != 0;
        if (!s_enabled || !Resolve()) return;
        s_active = true;
        s_pulses = 0;
        s_conflictLogged = false;
        logging::Info("[ExtraTC] Starting export-only extra nextgen timecycle experiment");
        Pulse();
    }
    void NextFrame()
    {
        if (s_enabled) Pulse();
    }
    void End()
    {
        if (!s_enabled) return;
        std::uint64_t out = 0;
        if (s_forced)
        {
            Call(s_force, 0, true, out);
            s_forced = false;
        }
        if (s_owned)
        {
            std::uint64_t current = 0;
            if (Call(s_getExtra, 0, false, current) &&
                static_cast<int>(current) == s_ownIndex)
            {
                Call(s_clearExtra, 0, false, out);
                logging::Info("[ExtraTC] Cleared own extra timecycle modifier");
            }
            else
                logging::Warning("[ExtraTC] Extra slot changed; leaving other modifier intact");
        }
        logging::InfoF("[ExtraTC] End of export: {} native calls/pulses", s_pulses);
        s_enabled = false;
        s_active = false;
        s_owned = false;
        s_ownIndex = -1;
    }
}
