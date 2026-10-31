#pragma once
#include <Windows.h>
#include <string>

namespace ImportHook
{
    // Replace an import pointer without suspending threads. Applicable only when
    // the executable imports the given API (otherwise use a different hook).
    bool Patch(HMODULE imageModule,
               const char* dllName,
               const char* functionName,
               void* expectedAddress,
               void* replacement,
               void** originalAddress,
               std::string& detail);
}
