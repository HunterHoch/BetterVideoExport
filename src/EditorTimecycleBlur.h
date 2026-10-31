#pragma once
namespace EditorTimecycleBlur
{
    // One-render-per-output-frame solution. Suppresses only the motion-blur
    // variable's replay-keyframe override, leaving recorded lighting untouched.
    void Initialize();
    void SetExportActive(bool active);
}
