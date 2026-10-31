#pragma once
#include <Windows.h>
#include <string>
#include <cstdlib>

namespace Config
{
    // BetterVideoExport.ini lives next to the GTA executable (same place as ffmpeg.exe).
    inline const std::wstring& IniPath()
    {
        static const std::wstring path = []
        {
            wchar_t exe[MAX_PATH];
            DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
            std::wstring p(exe, n);
            size_t slash = p.find_last_of(L"\\/");
            if (slash != std::wstring::npos) p.resize(slash + 1);
            return p + L"BetterVideoExport.ini";
        }();
        return path;
    }

    inline uint32_t GetUInt(const wchar_t* section, const wchar_t* key, uint32_t def)
    {
        return (uint32_t)GetPrivateProfileIntW(section, key, (INT)def, IniPath().c_str());
    }

    inline float GetFloat(const wchar_t* section, const wchar_t* key, float def)
    {
        wchar_t buf[64], defStr[64];
        swprintf(defStr, 64, L"%g", def);
        GetPrivateProfileStringW(section, key, defStr, buf, 64, IniPath().c_str());
        return (float)_wtof(buf);
    }

    inline std::wstring GetStr(const wchar_t* section, const wchar_t* key, const wchar_t* def)
    {
        wchar_t buf[1024];
        GetPrivateProfileStringW(section, key, def, buf, 1024, IniPath().c_str());
        return buf;
    }

    // Write a documented default ini the first time the plugin runs, so users
    // have something to edit. No-op if the file already exists.
    inline void EnsureDefaultFile()
    {
        if (GetFileAttributesW(IniPath().c_str()) != INVALID_FILE_ATTRIBUTES)
            return;

        static const char* kDefault =
            "[Export]\r\n"
            "; Average bitrate in bits/sec (Win7 path). 100000000 = 100 Mbit/s.\r\n"
            "Bitrate=100000000\r\n"
            "; VBR quality percent (Win8+ path). 100 = max.\r\n"
            "QualityPercent=100\r\n"
            "; Keep the 60 FPS toggle at actual 60 FPS.\r\n"
            "HighFps=60\r\n"
            "; Max export resolution. 3840x2160 = 4K UHD (stock is 1920x1080).\r\n"
            "MaxWidth=3840\r\n"
            "MaxHeight=2160\r\n"
            "\r\n"
            "[Encoder]\r\n"
            "; ffmpeg args used when re-encoding the editor output (requires ffmpeg.exe).\r\n"
            "; Recommended for FiveM: supply full path to ffmpeg.exe if detection fails.\\r\\n"
            "; FFmpegPath=C:\\\\ffmpeg\\\\bin\\\\ffmpeg.exe\\r\\n"
            "VideoArgs=-c:v libx264 -preset medium -crf 16 -pix_fmt yuv420p\r\n"
            "AudioArgs=-c:a aac -b:a 320k\r\n"
            "\r\n"
            "[MotionBlur]\r\n"
            "; Automatically install Normal preset from this bundled ASI into FiveM.app/mods.\r\n"
            "; Set to 0 to stop future installations (remove installed RPF manually to disable blur).\r\n"
            "AutoInstallNormalPreset=1\r\n";

        HANDLE h = CreateFileW(IniPath().c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE)
        {
            DWORD w = 0;
            WriteFile(h, kDefault, (DWORD)strlen(kDefault), &w, nullptr);
            CloseHandle(h);
        }
    }
}
