#pragma once
#include <cstdint>
#include <vector>
#include <string>

// Single native GTA bake frame -> direct DX11 offscreen copy -> NV12 -> NVENC.
// No swapchain Present(TEST), ReShade dependency, additional world renders,
// temporal accumulation, or replay stepping.
namespace EverOnePass {
    void Initialize();
    void Begin(unsigned width, unsigned height, const std::wstring& output);
    void OnVideoSample();
    bool Consume(std::vector<std::uint8_t>& output);
    void End();
}
