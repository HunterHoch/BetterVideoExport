#include "Hooking.h"
#include "EditorEncoder.h"
#include "EditorExport.h"
#include "Logging.h"
#include "GameVersion.h"

#include <Windows.h>
#include <exception>

// Avoid doing logging, pattern scanning, or hook installation under
// the Windows loader lock. Especially important when injected by FiveM.
static DWORD WINAPI InitializePlugin(LPVOID)
{
    try
    {
        logging::Initialize("BetterVideoExport", "BetterVideoExport.log");
        logging::LogStartupBanner(gameversion::GetGameBuild());
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
        DisableThreadLibraryCalls(module);
        HANDLE worker = CreateThread(nullptr, 0, InitializePlugin, nullptr, 0, nullptr);
        if (worker)
            CloseHandle(worker);
        else
            OutputDebugStringA("[BetterVideoExport] Failed to create initialization thread.\n");
    }
    return TRUE;
}
