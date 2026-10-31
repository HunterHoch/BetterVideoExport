#pragma once
#include <Windows.h>

namespace MotionBlurPreset
{
    // Install an embedded, verified RPF for FiveM on next launch.
    // Failures do not interfere with the native video exporter.
    void InstallBundledPreset(HMODULE self);
}
