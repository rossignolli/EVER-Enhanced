# Publishing EVER Enhanced

This checklist is for maintainers. Public releases are created from tags that
match `v*`; the GitHub Actions workflow builds and uploads the ZIP and its
SHA-256 checksum.

## Release checklist

1. Confirm the supported GTA V Enhanced, ScriptHookV, and ReShade versions in
   `README.md` and `ENHANCED_PORT.md`.
2. Confirm the FFmpeg feature set and binary-license strategy. Do not publish a
   GPL-enabled FFmpeg build as an Apache-only binary distribution.
3. Build Release and run `ctest --test-dir build-enhanced -C Release`.
4. Inspect `x64/Release`: it must contain `zEVER.asi`, `EVER/EVER.ini`, presets,
   `EncoderConfig.exe`, and all required runtime DLLs.
5. Test a short baseline export and one dual-pass export in the supported game
   build. Probe and fully decode the result.
6. Ensure no logs, videos, screenshots, local paths, game files, ScriptHookV
   binaries, or credentials are tracked.
7. Review `LICENSE`, `NOTICE`, and every file generated under
   `THIRD_PARTY_LICENSES` in the release ZIP.
8. Push the release commit, tag a prerelease such as `v2.0.0-beta.1`, and verify
   the GitHub Actions artifact before announcing it.

## Suggested release notes

State clearly:

- that the build targets GTA V Enhanced and is version-sensitive;
- the exact tested game and ScriptHookV versions;
- that ReShade with full add-on support is required;
- which codecs are included in that specific binary;
- validated resolution/FPS/motion-blur/audio combinations;
- known limitations and rollback/uninstall instructions; and
- that Legacy users should use upstream EVER releases.

Do not promise compatibility with untested FiveM or future GTA updates.

## Compatibility monitor

`.github/workflows/compatibility-monitor.yml` checks Steam app `3240220` every
six hours. It compares the public branch build ID with `compatibility.json`.

When Steam changes the build ID, the workflow:

- changes the README compatibility block to a prominent warning;
- creates or updates an issue labeled `compatibility-warning`; and
- changes the short GitHub repository description when the optional
  `REPOSITORY_ADMIN_TOKEN` Actions secret is configured.

The normal `GITHUB_TOKEN` can update repository contents and issues, but GitHub
requires repository Administration permission to change the short description.
Use a fine-grained token restricted to this repository with **Administration:
read and write**, save it as `REPOSITORY_ADMIN_TOKEN`, and do not grant unrelated
permissions.

After a GTA update, do not clear the warning until the end-to-end export matrix
passes. Then update all version fields in `compatibility.json`, update the
validation record, and manually dispatch the workflow. A matching build restores
the supported README block, closes the warning issue, and restores the normal
short description.
