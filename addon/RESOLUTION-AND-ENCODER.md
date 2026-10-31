# Native Resolution and GPU-to-NVENC correction (GTA Legacy 3788)

**Core observation:** Rockstar Editor exports a 2560x1440 NV12 video frame,
but that does not establish the resolution of the scene actually being
rendered. A 1920x1080 source enlarged to 2560x1440 is consistent with the
visible loss of fine detail. Missing DOF is a separate render-pass problem.

## What older VYNE reverse engineering found

`HunterHoch/VYNE/main/src/native_resolution.cpp` from GTA build 3258
contains a separately validated internal 1920x1080 native export clamp.
It identifies four callers of that clamp (capture size, encoder defaults,
setup, and reset), intercepts the actual `VideoEncode` texture allocation,
and verifies viewport and color-source dimensions. The 3258 code also
handles monitor fitting and mode restore.

**Important:** Hard-coded 3258 addresses cannot safely be reused in 3788.
Current BetterVideoExport's six signature patches change export settings,
but do *not* independently prove that its GPU scene source is 2560x1440.

## Changes in this branch

- ReShade add-on still identifies the five export-sized render targets.
- At the selected offscreen color pass, it logs pixel shader input SRV
  dimensions: 1920x1080 vs 2560x1440. This is observational, not proof
  unless the logged SRVs are known to be the scene/compositor inputs.
- Rather than applying `render_effects` then discarding its output, the add-on
  captures the post-effect BGRA8 GPU texture and writes it to a high-bandwidth
  shared mapping.
- The existing sink writer consumes that GPU frame, converts BGRA to
  BT.709 limited-range NV12, and passes it to our existing FFmpeg/NVENC.
  If no valid GPU sample is ready, it falls back to GTA's original NV12.
- The operation is sequential and synchronous, and can slow offline export;
  no additional scene renders or replay seeks are performed.
- Motion blur shader remains a best-effort ReShade optical-flow approximation.
  It is *not* GTA's built-in motion blur.
- Main remains unchanged. Do not use this as proof of true 1440p until a
  render-size source trace validates that. Merely enlarging a render target
  or using GPU-to-NVENC does not invent missing resolution detail.

## Runtime logging

- `[VYNE_BakeFX] GPU compositing inputs: export=2560x1440, full-res SRVs=...,
  1080p SRVs=...`
- `[VYNE_BakeFX] GPU frame transport connected...`
- `[BakeFX] ACTUAL post-ReShade GPU frame sent to NVENC: #...`
- `[BakeFX] GPU-to-encoder: processed NV12 frames=... native fallbacks=...`

These distinguish (a) whether GPU processing reaches NVENC, and (b)
whether the scene source appears internally downscaled. They do not
pretend to solve the native 1080p clamp on game build 3788.

## Next engine patch

Port the 3258 VYNE native resolution clamp/caller-site analysis to 3788
using signature validation of the actual `VideoEncode` render path.
Control the scene viewport and source render size BEFORE GTA upscales.
Guard by exact executable build and refuse unknown instruction bodies.
