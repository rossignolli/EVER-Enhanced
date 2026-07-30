# Installing and using EVER Enhanced

## Before installing

EVER Enhanced modifies Rockstar Editor's export path inside the running game.
Game updates can invalidate version-specific hooks, so confirm that the release
notes list your GTA V Enhanced build before using it.

You need:

- GTA V Enhanced on Windows;
- a [ScriptHookV build](https://www.dev-c.com/gtav/scripthookv/) that supports
  GTA V Enhanced;
- [ReShade](https://reshade.me/) for DirectX 12 with full add-on support; and
- sufficient disk and GPU memory for the selected export.

## Installation

1. Back up any existing `zEVER.asi` and `EVER` directory from the GTA V
   Enhanced installation folder.
2. Download [ScriptHookV from its official page](https://www.dev-c.com/gtav/scripthookv/)
   and install it according to its author's instructions. Verify that the page
   lists support for the installed Enhanced build.
3. Download [ReShade from its official website](https://reshade.me/), run the
   installer, select `GTA5_Enhanced.exe`, select DirectX 12, and install the
   version with full add-on support.
4. Extract the EVER Enhanced release ZIP directly into the directory containing
   `GTA5_Enhanced.exe`.

Expected layout:

```text
GTA V Enhanced/
|-- GTA5_Enhanced.exe
|-- zEVER.asi
`-- EVER/
    |-- EVER.ini
    |-- preset.json
    |-- EncoderConfig.exe
    |-- dlls/
    `-- presets/
```

Do not copy `zEVER.asi` into the `EVER` directory and do not rename it.

## First launch check

1. Launch Story Mode.
2. Press `Home` to open ReShade.
3. Open the add-ons tab.
4. Confirm that EVER identifies `GTA V Enhanced` and says that Enhanced
   compatibility mode is active.

If EVER is absent, stop here and use the troubleshooting section. Starting an
export with a partial installation makes diagnosis harder.

## Configure an export

Close the game before editing configuration files manually, or keep
`auto_reload_config = true` so EVER reloads them when a new export starts.

Edit `EVER/EVER.ini`:

```ini
enable_mod = true
auto_reload_config = true
output_folder =
log_level = error

[EXPORT]
fps = 30
motion_blur_samples = 0
motion_blur_strength = 0.5
export_openexr = false
disable_watermark = false
```

- Leave `output_folder` blank to use the Windows Videos directory.
- Start with 30 FPS and no motion blur for the first test.
- `motion_blur_samples = 3` means four rendered temporal samples per final
  frame. At 120 FPS output that is an effective 480 renders per second.
- Set `log_level = debug` only while diagnosing a problem; debug logs are much
  larger.

Use `EVER/EncoderConfig.exe` or copy one of the JSON files in `EVER/presets`
over `EVER/preset.json`. Hardware presets only work when the matching GPU,
driver, and FFmpeg encoder are available.

## Export

1. Open Rockstar Editor and load the project.
2. Start Rockstar Editor's normal export flow.
3. Keep the game focused and wait for EVER to finish.

When the effective render rate exceeds 60, EVER performs two passes. The first
captures audio and the second renders video. The handoff is automatic. The
screen can remain on an export or loading state while the file is finalized;
do not close the game until completion is reported.

The output filename and path are written to `EVER/EVER.log`. With a blank
`output_folder`, check the current Windows user's Videos directory.

## Recommended first tests

Run a short project in this order:

1. 1920x1080, 30 FPS, no motion blur, H.264 hardware preset if available.
2. Your target resolution and frame rate, still without motion blur.
3. Add one motion-blur sample.
4. Increase samples or switch to AV1 only after the earlier test succeeds.

This sequence makes encoder, memory, capture, and dual-pass failures easy to
distinguish.

## Troubleshooting

### EVER does not appear in ReShade

- Confirm ReShade was installed for `GTA5_Enhanced.exe`, not the Legacy
  executable.
- Confirm the DirectX 12 renderer and full add-on support were selected.
- Confirm `zEVER.asi` is next to the game executable.
- Confirm ScriptHookV explicitly supports the installed Enhanced version.

### The game reports `ERR_GFX_STATE`

Close the game completely before retrying. Test without unrelated overlays or
graphics mods, confirm the current ReShade installation, and start with the
baseline 30 FPS/no-blur configuration. Do not repeatedly force-kill the game
while an encoder is finalizing a file.

### Export stops or appears frozen

Check `EVER/EVER.log`. A high-FPS or multi-sample export can be in its automatic
two-pass handoff or asynchronous finalization. If the log no longer advances,
record the last 100 lines before closing the game.

### Encoder cannot reserve memory

Reduce resolution, frame rate, motion-blur samples, encoder surfaces, or other
GPU-heavy mods. Use the supplied 8-bit AV1 NVENC preset before trying a custom
10-bit P010 configuration. Closing browsers and GPU-accelerated applications
can also free VRAM.

### The Rockstar export UI appears in the video

Confirm the log says `Enhanced frame capture active via ReShade`. If it uses a
swap-chain fallback instead, include the complete debug log in a bug report.

### No audio or audio is out of sync

Do not interrupt the automatic second pass. Report the configured FPS, sample
count, output video/audio durations, and the complete debug log.

## Uninstall

Close the game, then remove only `zEVER.asi` and the `EVER` directory installed
by this release. ReShade and ScriptHookV are separate products and should be
removed using their own instructions if they are no longer wanted.
