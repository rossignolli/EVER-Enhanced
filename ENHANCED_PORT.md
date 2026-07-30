# GTA V Enhanced port

This branch contains the active port of EVER to **Grand Theft Auto V Enhanced**.
The Enhanced executable is detected at runtime, so the same binary keeps the original Legacy path
while selecting a separate, safer Enhanced path.

## Port contract

The Enhanced port must preserve EVER's original purpose rather than merely produce a playable
Rockstar Editor export. In particular:

- The output resolution follows the actual ReShade/game render resolution, up to the user's
  hardware limit. The port must not silently fall back to Rockstar Editor's standard Media
  Foundation resolution or downscale a higher-resolution back buffer.
- The configured frame rate and encoding settings must control the final output.
- Audio must remain synchronized at high/custom frame rates; multi-pass rendering is part of the
  required solution when a single pass cannot preserve synchronization.
- The Legacy threshold is the effective render rate, `output FPS * (motion blur samples + 1)`.
  Configurations above 60 effective FPS require the audio-only/video-only dual-pass workflow until
  an equally strong replacement is proven end to end.
- ReShade post-processing, configurable containers/codecs, motion blur, watermark removal, and
  OpenEXR sequences are parity features, not unrelated extras.

A build can be described as an experimental compatibility stage while some parity features are
still missing, but it is not a complete EVER port until the applicable promises above work in an
end-to-end Rockstar Editor export.

## Current Enhanced support

- Loads as a ScriptHookV/ReShade add-on in `GTA5_Enhanced.exe`.
- Avoids all Legacy D3D11 hooks and uses edition-specific validation before installing any
  game-address hook.
- Tracks Rockstar Editor's named `VideoEncode` DX12 render target through ReShade add-on events.
- Selects the full-resolution BGRA8 render target used before Rockstar's export-progress UI and
  reads it through an asynchronous three-slot ReShade/DX12 ring. The swap-chain backbuffer remains
  a fallback. This both removes the progress HUD and avoids the device loss caused by synchronous
  screenshot capture.
- Includes an edition-specific custom-FPS hook for Enhanced 1.0.1158.13. The target is accepted
  only after its complete 21-byte wrapper and virtual slot are validated. End-to-end exports have
  passed at 24, 30, 60, and 120 FPS.
- Uses Rockstar Editor's native Media Foundation video samples to pace frame capture.
- Reuses Rockstar Editor PCM audio and EVER's FFmpeg encoding pipeline.
- Accumulates Enhanced multi-sample motion blur directly on the DX12 GPU in an RGBA16F render
  target, then reads back only the divided output frame. This preserves the original
  shutter-strength rule and `samples + 1` grouping while reducing color readbacks from every
  subframe to one per output frame. The parallel CPU implementation remains a fallback for
  unsupported formats and OpenEXR, where synchronized depth readback is still required.
  H.264/H.265/AV1 encoding can use NVENC independently on supported NVIDIA GPUs.
- Resolves unique Enhanced 1.0.1158.13 entry points for start-bake, replay cleanup, pending-bake,
  and kill/cancel transitions. A first 61 FPS run captured 15.4347 seconds of PCM audio in the
  audio-only pass, proving the pass-one override and memory buffer. That run also exposed that
  PolyHook could not decode the cleanup or kill/cancel entry points, so both now use validated
  12-byte manual hooks with temporary byte restoration when their original functions are called.
  The cleanup patch covers its complete prologue and RIP-relative flag test; the kill/cancel patch
  preserves its original `bool` calling convention. Runtime tracing also proved that Enhanced calls
  the non-user-cancel kill transition immediately before Media Foundation `Finalize`. Suppressing
  the whole function also suppresses finalization, so EVER resolves the first recording-index reset
  from its instruction signature, temporarily exits through a matching x64 epilogue immediately
  before that destructive store, and still allows the earlier finalization trigger to run. Duplicate
  post-finalize kills are suppressed, while an explicit user cancellation still executes normally.
  Enhanced pauses the ScriptHook script fiber while its pending-bake screen is active, but continues
  polling the pending-bake function on the game main thread. The pending-bake hook now performs the
  pass-one cleanup and advances the delayed pass-two startup from that main-thread polling path,
  with a thread-local re-entry guard around cleanup.
  The corrected two-pass handoff has completed end-to-end 61 and 120 effective-FPS exports. A follow-up build now
  applies the same pre-destructive-store exit to the normal pass-two kill transition, preserving
  Enhanced's pending-bake polling until the worker-thread finalization marks the pass complete. The
  next main-thread poll then performs final replay cleanup and dismisses the stale export screen.
  For the pass-one-to-pass-two handoff, Enhanced's
  pending-bake polling is too bursty to provide a reliable delay. The hook therefore releases the
  loading gate after cleanup so ScriptMain resumes, waits the normal frame-counted interval, and
  starts pass two from a settled game frame.
- Tracks a full-resolution single-sample D32/S8 depth-stencil candidate and exports synchronized
  color/depth D3D12 readbacks to OpenEXR. The raw EXR writer has
  an automated round-trip test that verifies the 2x2 data window, R/G/B/SubsurfaceScatter/depth.Z
  channels, representative color/depth values, and padded D3D12-style color/depth row pitches
  through OpenEXR's own reader. The first 4K runtime sequence exposed that a combined D32/S8
  readback footprint zeroed the D3D12 copy batch. The readback now copies plane 0 into its native
  R32_FLOAT footprint while retaining the source format separately. A complete 371-frame 4K game
  sequence has passed channel, color, and varying-depth inspection. EXR compression runs on a
  bounded background worker so it no longer blocks each Rockstar render frame.
- Supports configurable FFmpeg containers/codecs and ships presets covering MP4, MKV, AVI, MOV,
  and WebM plus AAC, MP3, FLAC, PCM, and Opus audio. Runtime exports have passed with H.264 NVENC,
  H.265 NVENC, AV1 NVENC, FLAC, and MP3. Encoder sample formats are negotiated automatically for
  custom presets. FFmpeg is built against NVIDIA codec headers 12.1.14 so AV1 NVENC remains
  compatible with the FFmpeg 6.1 API used by the mod.
- Final FFmpeg trailer writing runs asynchronously in both single- and dual-pass Enhanced exports,
  preventing long OpenEXR or high-bitrate finalization from stalling the GTA main thread.

## Not yet ported or validated

- Fractional frame-rate presets still need an explicit Enhanced runtime test.
- Additional native G-buffer planes beyond color, SubsurfaceScatter compatibility, and depth have
  not been identified in Enhanced.
- FiveM uses a different host path and has not been validated by this Enhanced GTA executable test suite.

Those features depend on internal GTA V functions and rendering resources. Their Legacy byte
patterns do not match the current Enhanced executable, so Enhanced compatibility mode deliberately
does not install those hooks without an edition-specific signature and entry validation.

## Requirements

- GTA V Enhanced.
- A ScriptHookV build that explicitly supports the installed Enhanced game version.
- ReShade with full add-on support and DX12 enabled.

Copy `zEVER.asi` and the `EVER` directory into the GTA V Enhanced installation directory. The
`z` prefix ensures ReShade loads first. Open the ReShade overlay to verify that EVER reports
`GTA V Enhanced` and `Enhanced compatibility mode`.

## Validation status

The evidence below was collected on July 30, 2026 with GTA V Enhanced
1.0.1158.13, ScriptHookV 3889.0.1158.13, and ReShade 6.7.3. See
[docs/VALIDATION.md](docs/VALIDATION.md) for executable hashes and decoded
output frames.

- Release build: passing with MSVC 19.44 and CMake 3.30.
- Binary packaging and runtime dependency inspection: passing.
- In-game load and ScriptHook registration: passing on game 1.0.1158.13 with ScriptHookV 3889.0.
- End-to-end high-resolution Rockstar Editor export: passing at 3840x2160 on game 1.0.1158.13.
- Baseline verified output: H.264/yuv420p, 463 frames at 30 FPS, AAC stereo 48 kHz, 15.434 s.
- A/V synchronization telemetry: 1.33 ms delta, zero dropped video frames in the current 15.43 s test project.
- Independent FFprobe validation and decoded-frame visual check: passing.
- Clean pre-UI frame capture: passing. Frames decoded at 5 s and 10 s contain no Rockstar export
  controls or progress HUD.
- Custom 60 FPS export: passing at 3840x2160. The Enhanced render-time hook resolved uniquely and
  installed successfully; the output contains 926 frames with 16.667 ms packet cadence, zero drops,
  AAC stereo audio, and 1.33 ms encoder-side A/V delta. Frames decoded at 5 s and 10 s remain free
  of Rockstar export controls and progress HUD.
- Custom 24 FPS export with OpenEXR: passing at 3840x2160. The output contains 371 H.264 frames and
  AAC stereo audio. All 371 EXRs were written, and the inspected payload contains R/G/B,
  SubsurfaceScatter, and varying `depth.Z` channels at 3840x2160.
- Custom 120 FPS export: passing at 3840x2160 through the audio-first dual-pass path. The output
  contains 1,852 H.264 frames and AAC stereo audio; encoder telemetry reports a 1.33 ms A/V delta.
- Custom 240 FPS export: passing at 3840x2160 through the audio-first dual-pass path. The output
  contains 3,704 H.264 frames at an exact declared 240/1 rate, AAC stereo audio, zero dropped video
  frames, and a 1.33 ms encoder-side A/V delta.
- Motion blur at the single-pass limit: passing at 3840x2160, 30 FPS output, one additional sample
  (60 effective FPS), strength 1.0. The output contains 463 frames with zero drops and visible
  two-subframe accumulation, plus AAC stereo audio. Encoder telemetry reports 15.4333 s of video,
  15.4347 s of audio, and a 1.33 ms A/V delta. Decoded frames at 5 s and 10 s remain free of the
  Rockstar export UI and watermark.
- Motion blur above the single-pass limit: passing at 3840x2160, 30 FPS output, three additional
  samples (120 effective FPS), strength 1.0. The audio-first/video-second handoff completed
  automatically, produced 463 frames with zero drops, preserved AAC audio at a 1.33 ms telemetry
  delta, and visibly accumulated four subframes without capturing the Rockstar UI.
- GPU motion-blur stress test: passing with AV1 NVENC at 3840x2160, exact 120 FPS output, three
  additional samples (480 effective FPS), and strength 1.0. The DX12 RGBA16F path produced 1,852
  frames with zero drops, decoded end-to-end without errors, and made camera/car motion visibly
  blurred while the Rockstar UI stayed absent. Video duration is 15.433333 s and AAC stereo audio
  is 15.434000 s, a 0.667 ms difference. Only one color readback is issued per output frame instead
  of four subframe readbacks.
- GPU encoding: H.264 NVENC, H.265 NVENC, and AV1 NVENC pass at 3840x2160. The AV1 run produced
  274 Main-profile YUV420 frames at an exact 30 FPS, AAC stereo 48 kHz audio, zero dropped frames,
  and a 2.67 ms A/V duration delta. Its `p6`, VBR, constant-quality target, spatial AQ, and temporal
  AQ options were accepted by FFmpeg. The shipped AV1 preset limits the encoder to four 8-bit
  surfaces because P010 10-bit initialization ran out of allocatable GPU memory beside the active
  4K GTA/ReShade workload.
- Container/audio matrix: MP4/AAC, MKV/FLAC, AVI/MP3, and MOV/PCM pass in full Rockstar Editor
  exports. The ProRes 422 HQ MOV preset is intentionally CPU-heavy at 4K.
- Watermark/progress UI removal: passing through clean pre-UI capture. Decoded evidence frames from
  baseline, motion-blur, 120 FPS, and OpenEXR runs contain neither Rockstar export controls nor a watermark.

For an initial test, set `log_level = debug` in `EVER/EVER.ini`, run the game at a resolution above
Rockstar Editor's standard export limit, export a short project, and inspect `EVER/EVER.log`. The
log must show the actual runtime resolution as `Enhanced Video Export Resolution` even when the
Rockstar Media Foundation stream reports a lower resolution. A successful capture also logs
`Enhanced frame capture active via ReShade`.
