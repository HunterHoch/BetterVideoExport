#include "MotionBlurPreset.h"
#include "Logging.h"
#include "Config.h"

#include <Windows.h>
#include <cstdint>
#include <cwctype>
#include <cstring>
#include <iterator>
#include <string>
#include <vector>

namespace
{
    constexpr char kPayloadMagic[8] = { 'B','V','E','M','B','0','1','!' };
    constexpr DWORD kMaxPayloadSize = 2 * 1024 * 1024;

    uint32_t ReadLE32(const unsigned char* p)
    {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
            | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    }

    uint32_t CRC32(const unsigned char* p, size_t n)
    {
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < n; ++i)
        {
            crc ^= p[i];
            for (int b = 0; b < 8; ++b)
                crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
        return crc ^ 0xffffffffu;
    }

    bool ReadExact(HANDLE file, void* buffer, DWORD size)
    {
        DWORD read = 0;
        return ReadFile(file, buffer, size, &read, nullptr) && read == size;
    }

    std::wstring Lower(std::wstring s)
    {
        for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
        return s;
    }

    bool LoadAppendedRpf(HMODULE self, std::vector<unsigned char>& payload)
    {
        wchar_t modulePath[32768]{};
        DWORD len = GetModuleFileNameW(self, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (!len || len == std::size(modulePath)) return false;

        HANDLE file = CreateFileW(modulePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        LARGE_INTEGER size{};
        bool ok = false;
        do
        {
            if (!GetFileSizeEx(file, &size) || size.QuadPart < 16) break;
            LARGE_INTEGER footerPos{};
            footerPos.QuadPart = size.QuadPart - 16;
            if (!SetFilePointerEx(file, footerPos, nullptr, FILE_BEGIN)) break;
            unsigned char footer[16]{};
            if (!ReadExact(file, footer, sizeof(footer))) break;
            if (memcmp(footer, kPayloadMagic, sizeof(kPayloadMagic)) != 0) break;
            DWORD count = ReadLE32(footer + 8);
            uint32_t checksum = ReadLE32(footer + 12);
            if (count < 16 || count > kMaxPayloadSize || size.QuadPart < static_cast<LONGLONG>(count) + 16) break;
            LARGE_INTEGER start{};
            start.QuadPart = size.QuadPart - count - 16;
            if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) break;
            payload.resize(count);
            if (!ReadExact(file, payload.data(), count)) break;
            if (memcmp(payload.data(), "RPF7", 4) != 0 || CRC32(payload.data(), payload.size()) != checksum) break;
            ok = true;
        } while (false);
        CloseHandle(file);
        if (!ok) payload.clear();
        return ok;
    }

    bool SameFile(const std::wstring& path, const std::vector<unsigned char>& expected)
    {
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                  nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER size{};
        bool equal = false;
        if (GetFileSizeEx(file, &size) && size.QuadPart == static_cast<LONGLONG>(expected.size()))
        {
            std::vector<unsigned char> data(expected.size());
            equal = ReadExact(file, data.data(), static_cast<DWORD>(data.size())) && data == expected;
        }
        CloseHandle(file);
        return equal;
    }

    bool WritePreset(const std::wstring& path, const std::vector<unsigned char>& payload)
    {
        const std::wstring temp = path + L".new";
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        DWORD written = 0;
        bool ok = WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)
                  && written == payload.size() && FlushFileBuffers(file);
        CloseHandle(file);
        if (!ok) { DeleteFileW(temp.c_str()); return false; }
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            CopyFileW(path.c_str(), (path + L".bak").c_str(), TRUE);
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            DeleteFileW(temp.c_str());
            return false;
        }
        return true;
    }
}

namespace MotionBlurPreset
{
    void InstallBundledPreset(HMODULE self)
    {
        if (GetPrivateProfileIntW(L"MotionBlur", L"AutoInstallNormalPreset", 1,
                                  Config::IniPath().c_str()) == 0)
        {
            logging::Info("[MotionBlur] Auto-install disabled in INI.");
            return;
        }
        wchar_t modulePath[32768]{};
        DWORD length = GetModuleFileNameW(self, modulePath, static_cast<DWORD>(std::size(modulePath)));
        if (!length || length >= std::size(modulePath)) return;
        const std::wstring lowPath = Lower(modulePath);
        auto at = lowPath.rfind(L"\\plugins\\");
        if (at == std::wstring::npos || lowPath.find(L"fivem.app") == std::wstring::npos)
        {
            logging::Info("[MotionBlur] Not in FiveM.app/plugins; skipping preset installer.");
            return;
        }
        std::vector<unsigned char> payload;
        if (!LoadAppendedRpf(self, payload))
        {
            logging::Warning("[MotionBlur] No valid bundled RPF payload found. Exporter unaffected.");
            return;
        }
        const std::wstring root(modulePath, at);
        // FiveM 'mods' expects an OIV package with assembly.xml + content/.
        // This bundled RPF is a raw common/ file overlay and belongs in 'addons'.
        const std::wstring addons = root + L"\\addons";
        const std::wstring target = addons + L"\\zz_VYNE_MotionBlur.rpf";
        const std::wstring oldTarget = root + L"\\mods\\zz_VYNE_MotionBlur.rpf";

        if (!CreateDirectoryW(addons.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            logging::Warning("[MotionBlur] Failed to create FiveM addons directory.");
            return;
        }

        bool installed = SameFile(target, payload);
        if (!installed)
        {
            if (!WritePreset(target, payload))
            {
                logging::Warning("[MotionBlur] Could not install raw graphics RPF in FiveM addons. Exporter remains usable.");
                return;
            }
            installed = true;
            logging::Info("[MotionBlur] Normal preset raw RPF installed in FiveM addons; restart FiveM before testing.");
        }
        else
        {
            logging::Info("[MotionBlur] Normal preset RPF present in FiveM addons (mounting not independently verified).");
        }

        // Remove only an exact byte-for-byte copy of our former, inert 'mods' package.
        // Never delete arbitrary user-owned graphics packs.
        if (installed && SameFile(oldTarget, payload))
        {
            if (DeleteFileW(oldTarget.c_str()))
                logging::Info("[MotionBlur] Removed obsolete raw RPF from FiveM mods.");
            else
                logging::Warning("[MotionBlur] Could not remove obsolete mods copy (delete it manually if necessary).");
        }
    }
}
