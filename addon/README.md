# VYNE_BakeFX native Rockstar Editor rendering integration

This is an **experimental, opt-in ReShade add-on** built from ReShade's official
"effects_during_frame" approach. It aims to apply an existing ReShade preset
to GTA V's offscreen render target during the **native Rockstar Editor bake**,
while BetterVideoExport keeps the proven Media Foundation -> FFmpeg/NVENC path.

## Important limitations

This initial build performs **GPU candidate discovery** automatically but
**does not invent which unnamed GPU resource is GTA's `VideoEncode` target**.
Width and height alone are not enough: we already demonstrated that selecting
an arbitrary output-sized target produces Export UI, cyan initialization frames,
and stale frames. The add-on does not modify any resource until explicitly
authorized by a verified resource name or by a manually selected candidate.

EVE's old `DXGI_PRESENT_TEST` trick does **not** execute modern ReShade
effects; this add-on instead uses the supported `effect_runtime::render_effects`
API on the GPU render pass itself.

## Installation

1. Build both the ASI and the add-on from this **same branch**.
2. Back up the existing `BetterVideoExport.asi` in FiveM.app/plugins;
   replace it with this branch's ASI. Keep `ffmpeg.exe` and existing encoding
   settings unchanged.
3. Install the compiled `VYNE_BakeFX.addon64` into **the folder searched by
   your current full-add-on-support ReShade installation**. Usually this is
   the directory containing the ReShade `dxgi.dll` or its configured AddonPath.
   Check ReShade.log for the add-on load; the standard no-addons build will
   not load it.
4. Add to the existing FiveM.app/plugins/BetterVideoExport.ini:

   ```ini
   [ReShadeBake]
   Enable=1
   ApplyEffects=0
   TargetCandidate=0

   [Capture]
   EveDx11=0

   [MotionBlur]
   EditorTimecycleBlur=0
   EditorExtraTimecycle=0
   ```

5. Restart FiveM. Export a short clip with the highly recognizable white
   wireframe ReShade preset enabled.
6. Send `BetterVideoExport.log` and ReShade's own log. Look for:

   ```
   [BakeFX] Export armed ..., addon=1
   [BakeFX] Candidate 1: GPU=..., 2560x1440 ...
   [BakeFX] Export: ... eligible binds=..., ...
   ```

If `addon=0`, the ReShade module was not loaded, the wrong ReShade build
was installed, or the mapping connection failed.

## Controlled effect test after resource identification

Once the candidate list is known, select **one verified scene-only candidate**:

```ini
[ReShadeBake]
Enable=1
ApplyEffects=1
TargetCandidate=3
```

`TargetCandidate=0` automatically injects only when the D3D11 debug object
name contains `VideoEncode`, which many retail drivers/builds may omit.
The injected preset is rendered once at the end of the selected GPU pass
between successive native Media Foundation video samples. The add-on
restores D3D11 state and never triggers extra scene renders.

Do **not** use `[Capture] EveDx11=2`; that legacy substitution mode is known
to capture the wrong frames.

## Current caveats

- Each ReShade effect runtime is attached to a swapchain. Whether its
  `render_effects` can target GTA 3788's offscreen `VideoEncode` resource
  without an intermediate copy needs runtime verification.
- The first native video frame is intentionally not affected because frame
  collection is armed only after the first actual `WriteSample`.
- Some effects rely on depth information; they may need the correct export
  depth texture separately.
- `TargetCandidate` numbers are diagnostics for one process lifetime and
  can differ after a restart; never hardcode them as a permanent solution.
- Motion blur is not automatically restored by ReShade capture. First
  prove that a recognizable preset is actually injected. The blur pass
  can then use camera/depth reprojection in the same GPU processing stage.

Research: https://github.com/crosire/reshade/tree/main/examples/13-effects_during_frame
and https://github.com/ali-alidoust/gta5-extended-video-export

No REPlus replay scrubbing, scene accumulation, or extra scene renders.
