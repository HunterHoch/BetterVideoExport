#pragma once
namespace NativeBakeBridge {
    void Initialize();
    void Begin(unsigned width, unsigned height);
    void OnVideoSample();
    void End();
}