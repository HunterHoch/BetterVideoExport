#pragma once
#include <cstdint>
#include <vector>
namespace NativeBakeBridge {
    void Initialize();
    void Begin(unsigned width, unsigned height);
    void OnVideoSample();
    void End();
    bool ConsumeNV12(std::vector<std::uint8_t>& output, unsigned width, unsigned height);
}