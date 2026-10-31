#include "EditorExport.h"
#include <Hooking.h>
#include "GameVersion.h"
#include "Config.h"
#include "Logging.h"

namespace EditorExport
{
    // H.264 level. 0x2A=4.2 (stock, caps ~62 Mbit @1080p). 0x33=5.1 lifts that to
    // ~240 Mbit and is the highest level Microsoft's H.264 encoder MFT accepts;
    // 5.2 (0x34) is rejected → "unknown error" on export. eAVEncH264VLevel5_1.
    static constexpr uint32_t kH264Level = 0x33;

    void Initialize()
    {
        if (!gameversion::IsLegacy())
            return;

        Config::EnsureDefaultFile();

        // From BetterVideoExport.ini [Export]; defaults match stock-improved values.
        const uint32_t kExportBitrate   = Config::GetUInt(L"Export", L"Bitrate", 100000000); // 100 Mbit/s
        const uint32_t kQualityPercent  = Config::GetUInt(L"Export", L"QualityPercent", 100); // VBR, 100=max
        const float    kHighFps         = Config::GetFloat(L"Export", L"HighFps", 120.0f);
        // NOTE: the MS encoder caps at level 5.1, which only covers 4K up to ~30 fps.
        // 4K + the 120fps toggle may exceed it; use the 30fps option for 4K if so.
        const uint32_t kMaxWidth        = Config::GetUInt(L"Export", L"MaxWidth", 3840);  // 4K UHD
        const uint32_t kMaxHeight       = Config::GetUInt(L"Export", L"MaxHeight", 2160);
        logging::InfoF("[EditorExport] Requested max={}x{}, 60fps-slot={}, bitrate={}, quality={}, H264-level=5.1",
            kMaxWidth, kMaxHeight, kHighFps, kExportBitrate, kQualityPercent);
        int foundPatterns = 0;
        {
            auto p = hook::pattern("48 8D 3D ? ? ? ? 41 8B C8 8B DA");
            if (p.size() > 0)
            {
                auto* table = hook::get_address<uint32_t*>(p.get_first<uint8_t>(0), 3, 7);
                for (int i = 0; i < 12; ++i)
                    hook::put<uint32_t>(&table[i], kExportBitrate);
                logging::Info("[EditorExport] Bitrate pattern patched");
                ++foundPatterns;
            }
        }

        {
            auto p = hook::pattern("48 8D 0D ? ? ? ? 48 98 8B 04 81 EB ?");
            if (p.size() > 0)
            {
                auto* table = hook::get_address<uint32_t*>(p.get_first<uint8_t>(0), 3, 7);
                for (int i = 0; i < 12; ++i)
                    hook::put<uint32_t>(&table[i], kQualityPercent);
                logging::Info("[EditorExport] Quality pattern patched");
                ++foundPatterns;
            }
        }

        {
            auto p = hook::pattern("48 8D 0D ? ? ? ? F3 0F 10 04 81 C3 F3 0F 10 05 ? ? ? ?");
            if (p.size() > 0)
            {
                auto* table = hook::get_address<float*>(p.get_first<uint8_t>(0), 3, 7);
                hook::put<float>(&table[1], kHighFps);
                logging::Info("[EditorExport] FPS pattern patched");
                ++foundPatterns;
            }
        }

        {
            auto p = hook::pattern("41 B8 2A 00 00 00 48 8B 01");
            if (p.size() > 0)
            {
                hook::put<uint32_t>(p.get_first<uint32_t>(2), kH264Level);
                logging::Info("[EditorExport] H.264 level pattern patched");
                ++foundPatterns;
            }
        }

        {
            auto pw = hook::pattern("B8 80 07 00 00 45 8B D0");
            if (pw.size() > 0)
            {
                hook::put<uint32_t>(pw.get_first<uint32_t>(1), kMaxWidth);
                logging::Info("[EditorExport] Width pattern patched");
                ++foundPatterns;
            }

            auto ph = hook::pattern("B9 38 04 00 00 41 8B C1");
            if (ph.size() > 0)
            {
                hook::put<uint32_t>(ph.get_first<uint32_t>(1), kMaxHeight);
                logging::Info("[EditorExport] Height pattern patched");
                ++foundPatterns;
            }
        }
        logging::InfoF("[EditorExport] Supported signature patches applied: 6 expected, {} found", foundPatterns);
        if (foundPatterns != 6)
            logging::Warning("[EditorExport] One or more signatures missing on this GTA build; max resolution/FPS may not apply.");
    }
}

static HookFunction hookEditorExport([]()
{
    EditorExport::Initialize();
});
