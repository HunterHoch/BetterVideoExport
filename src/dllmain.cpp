#include "Hooking.h"
#include "EditorEncoder.h"
#include "EditorExport.h"
#include "Logging.h"
#include "Config.h"
#include "GameVersion.h"
#include "MotionBlurPreset.h"
#include "NativeBakeBridgeApi.h"

#include <Windows.h>
#include <exception>

// Avoid doing logging, pattern scanning, or hook installation under
// the Windows loader lock. Especially important when injected by FiveM.
static HMODULE g_self = nullptr;

static DWORD WINAPI InitializePlugin(LPVOID)
{
    try
    {
        logging::Initialize("BetterVideoExport", "BetterVideoExport.log");
        logging::LogStartupBanner(gameversion::GetGameBuild());
        logging::InfoF("[Settings] Active INI: {}", std::string(Config::IniPath().begin(), Config::IniPath().end()));
        logging::InfoF("[Settings] Effective overrides: EditorTimecycleBlur={}, EditorExtraTimecycle={}, EveDx11={}",
            Config::GetUInt(L"MotionBlur", L"EditorTimecycleBlur", 0),
            Config::GetUInt(L"MotionBlur", L"EditorExtraTimecycle", 0),
            Config::GetUInt(L"Capture", L"EveDx11", 0));
        MotionBlurPreset::InstallBundledPreset(g_self);
        NativeBakeBridge::Initialize();
        HookFunctionBase::RunAll();
        OutputDebugStringA("[BetterVideoExport] Initialization finished.\n");
    }
    catch (const std::exception& ex)
    {
        OutputDebugStringA("[BetterVideoExport] Initialization failed: ");
        OutputDebugStringA(ex.what());
        OutputDebugStringA("\n");
    }
    catch (...)
    {
        OutputDebugStringA("[BetterVideoExport] Initialization failed with an unknown exception.\n");
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_self = module;
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(nullptr, 0, InitializePlugin, nullptr, 0, nullptr);
        if (worker)
            CloseHandle(worker);
        else
            OutputDebugStringA("[BetterVideoExport] Failed to create initialization thread.\n");
    }
    return TRUE;
}
