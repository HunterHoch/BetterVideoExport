#pragma once
#include <Windows.h>
#include <cstdint>
#include <vector>
#include <string>

// EVE-style native bake probe: use the present-time DX11 backbuffer rather
// than GTA's offscreen NV12 buffer. No replay seeking or additional world draws.
namespace EveDx11Capture
{
    void Initialize();
    void Start(unsigned width, unsigned height, const std::wstring& videoFile);
    bool Consume(std::vector<std::uint8_t>& nv12);
    void Stop();
}
