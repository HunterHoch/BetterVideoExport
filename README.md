# BetterVideoExport
Renders replays from Rockstar Editor using FFMPEG for higher quality + patches max bitrate and FPS.

# Credits
[Citizenfx](https://github.com/citizenfx) for hooking library used.

# Usage

Place ffmpeg.exe in the Grand Theft Auto V root directory or have ffmpeg in PATH.

## FiveM 3788: bundled Normal motion blur

The downloadable bundled ASI includes the **Normal** preset from GTAV_Freak's
[Motion Blur Mod](https://www.gta5-mods.com/misc/motion-blur-mod).
This is GTA's existing motion blur, not motion interpolation or temporal frame accumulation.

- Place `BetterVideoExport-3788-MotionBlur.asi` in `%LOCALAPPDATA%\\FiveM\\FiveM.app\\plugins\\` and remove the previous `BetterVideoExport.asi` so only one build is loaded. You may rename the new ASI to `BetterVideoExport.asi`.
- On its first run, the ASI verifies its bundled RPF and installs
  `%LOCALAPPDATA%\\FiveM\\FiveM.app\\addons\\zz_VYNE_MotionBlur.rpf`.
  **Restart FiveM once more** after that first launch. FiveM must mount the new RPF at boot.
- In GTA graphics settings use **PostFX High or above** and **Motion Blur Amount 0%**, as instructed by the original preset.
- The original mod describes the effect as third-person-only. In-editor behavior is not guaranteed for every camera mode.
- Check `BetterVideoExport.log` for a `[MotionBlur]` installation message.

The preset adds `postfx_motionblurlength = 1.000 1.000` under
`nextgen` in `timecycle_mods_1.xml` and includes the mod's
`visualsettings.dat` adjustments, which remove competing camera-event blur.
FiveM loads raw RPFs with a `common/` tree from `addons/`, whereas `mods/` requires an OIV-like `assembly.xml` package. Existence of a file on disk does not establish it was mounted. This uses GTA's file override, not invasive runtime timecycle memory patching.

To disable installation, set `[MotionBlur] AutoInstallNormalPreset=0` in
`BetterVideoExport.ini`. To **disable the actual effect**, also remove
`zz_VYNE_MotionBlur.rpf` from FiveM's addons folder and restart.

### Rebuilding from source

The Windows CI build produces an unbundled ASI core. After generating the
`zz_VYNE_MotionBlur.rpf` asset from your copy of the original preset,
bundle the binary using:

```shell
python tools/bundle_motion_blur.py BetterVideoExport.asi zz_VYNE_MotionBlur.rpf BetterVideoExport-3788-MotionBlur.asi
```

The bundler appends the RPF and a validated size/CRC32 footer to the PE image.
The original source archive is not redistributed in the fork.

For **FiveM/offline editor work only**; do not use this in GTA Online.


## Diagnosing 4K60 export failures (FiveM)

The GTA Rockstar Editor's "unknown error" is generic, so check which encoder is actually used.
By default this ASI searches for FFmpeg beside the running FiveM process, in
`%LOCALAPPDATA%\\FiveM\\FiveM.app\\plugins\\ffmpeg.exe`, then in the system PATH.
It **does not automatically search the original GTA V installation directory**.

You can also set an absolute path in the active `BetterVideoExport.ini`:

```ini
[Export]
HighFps=60
MaxWidth=3840
MaxHeight=2160

[Encoder]
FFmpegPath=C:\\Tools\\ffmpeg\\bin\\ffmpeg.exe
VideoArgs=-c:v h264_nvenc -preset p6 -tune hq -rc vbr -cq 16 -b:v 80M -maxrate 100M -bufsize 200M -pix_fmt yuv420p
AudioArgs=-c:a aac -b:a 320k
```

The actual `FFmpegPath` above is only an example. `BetterVideoExport.log` now logs:
* The active `BetterVideoExport.ini` path, which is fixed under `FiveM.app\\plugins\\`.
* Whether FFmpeg was found and whether the FFmpeg sink writer was selected.
* Captured video dimensions and frame rate, the size of the first raw NV12 frame, and the encoder process exit code.

FFmpeg's own errors are written next to the attempted export as `<output>.ffmpeg.log`.
Changing the INI requires restarting FiveM. An existing INI is not overwritten by new
defaults, so update `HighFps=120` to `HighFps=60` yourself if it is present.
