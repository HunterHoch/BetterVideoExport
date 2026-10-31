# EVER-style one-pass GPU capture + GTA native timecycle blur experiment

This branch keeps **Rockstar Editor's own bake timing** and the working
**FFmpeg/NVENC sink writer**, while replacing the encoder's input source with
a direct readback of the finished offscreen DX11 render target.

It intentionally does **not** import EVER's frame sampling or motion-blur
accumulation: no `drawAdditive`, `divideBuffer`, extra `Present`, extra
world renders, replays scrubbing, temporal frame averaging, or REPlus capture.

## What has been implemented

- Uses the same DX11 `OMSetRenderTargets` interception point that EVER
  uses, without ReShade or a `Present(TEST)` path.
- Copies the outgoing **completed offscreen color render target** to a
  staging texture **after GTA unbinds it**.
- Tracks eligible render targets at the actual export resolution.
  The selection favors an explicit `VideoEncode` D3D11 debug name,
  otherwise the last consistently bound BGRA/RGBA 8-bit color target
  after eight native video samples (the five-target pattern observed
  on FiveM Legacy 3788).
- Captured GPU BGRA8 is converted into BT.709 limited NV12 and sent to the
  existing FFmpeg/NVENC pipe with the exact next Rockstar `WriteSample`.
- If a capture is absent, stale, mismatched, or invalid, original Rockstar
  NV12 is used for **that frame**, preserving a playable output.
- Original GTA audio/WAV/muxing stays in the working code.
- Logs candidate target dimensions, formats, bind counts, direct GPU frame
  delivery, and native fallback counts.

## Native GTA blur attempt

The existing **Normal** game timecycle preset (already proven to affect
normal gameplay) can be used in conjunction with:

```ini
[MotionBlur]
AutoInstallNormalPreset=1
EditorTimecycleBlur=1
EditorExtraTimecycle=1
```

`EditorTimecycleBlur` removes only the replay timecycle override on
`postfx_motionblurlength` (variable 134 on previously observed game 3788).
`EditorExtraTimecycle` invokes GTA's `SET_EXTRA_TCMODIFIER("nextgen")`
and `SET_FORCE_MOTIONBLUR` for the export session.

**Important: these actions were tried before and did NOT show native blur
in the NV12 video.** Directly capturing the offscreen color buffer is a
different pixel source, but it is **not proof** that the actual GTA
motion-blur postprocess pass is executing during bake. If the pass is
disabled by replay camera flags or lacks camera history/motion data, a
timecycle value alone cannot enable it. Fixing that will require a
validated engine-specific native blur-pass gate or camera eligibility
patch on the exact FiveM build; no unverified hook has been inserted.

The same applies to internal render resolution: true 1440p is **not
established** merely by capturing a 2560x1440 texture. The original
VYNE (3258) engine source identified a 1080p export clamp, but 3258
instructions cannot safely be pasted into FiveM 3788.

## Setup

Use this experimental ASI by itself, without the separate ReShade
`.addon64` or optical-flow shader from previous attempts. The add-on
is not required for this test.

Put the new `BetterVideoExport.asi` in:

`%LOCALAPPDATA%\FiveM\FiveM.app\plugins\`

Keep your existing `BetterVideoExport.ini` and `ffmpeg.exe`; merge:

```ini
[Capture]
EveDx11=0
EverOnePass=1
EverTarget=0

[MotionBlur]
AutoInstallNormalPreset=1
EditorTimecycleBlur=1
EditorExtraTimecycle=1
```

`EverTarget=0` chooses the last persistent 8-bit color target (or
an explicitly named VideoEncode target). For a deliberate A/B run
`EverTarget=4` or `5` uses the observed candidate ordinal, but
candidate ordering may differ after a restart and is logged per export.

Allow GTA to run its native 2-3-second bake. A handful of initial frames
can fall back to GTA's original NV12 while GPU targets are identified.

Log markers:
- `[EVER1] DX11 OMSetRenderTargets hook installed`
- `[EVER1] Selected GPU candidate ...`
- `[EVER1] DIRECT GPU FRAME -> NVENC ...`
- `[EVER1] Finished: native samples=..., direct GPU captures=...,
  encoded GPU frames=..., native NV12 fallbacks=...`

If no GPU frames are delivered, inspect these logs. Do not mistake
successful compression for confirmed native motion blur or true 1440p.

## Important caveats

- The fallback target heuristic is not as strong as EVER's internal
  `CreateTexture("VideoEncode")` hook and must be verified visually.
- The synchronous staging readback may be slower than the native NV12
  path. No multi-frame sampling occurs; GPU stalls aren't extra scene
  renders.
- This is a proof-of-concept capture path, not a production-quality 4K
  texture pipeline. It intentionally favors correctness/fallbacks over
  speculative game-code patching.
- To return to the known-good encoder, restore the previous ASI or set
  `[Capture] EverOnePass=0` and disable both experimental blur toggles.

Source references:
- https://github.com/MiffedLyric559/EVER
- https://github.com/HunterHoch/VYNE
