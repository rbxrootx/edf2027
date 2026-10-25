# Private Windows test installer

This package path is intentionally separate from the public release rules. It
builds the current working tree with the official ReXGlue v0.10.0 SDK and emits
`out/package/EDF2027-private-test-0.3.38-beta.exe`.

The SDK used for this installer was built from the official
`rexglue/rexglue-sdk` v0.10.0 tag (`f5337cdc947ff6d4c4196737e2c807a48f2a1fc2`)
into `out/tools/sdki-msvc`; the codegen-3 fork was not found, so codegen falls
back to stock 0.10.0 only in the staging copy. Inno Setup 6.7.3 is installed
under `out/tools/InnoSetup6` from the official Authenticode-signed installer.

Run from the repository root after those tools are present:

```powershell
powershell -ExecutionPolicy Bypass -File packaging/private/build-private.ps1
```

The script copies the current tree to `out/package/staging/source`, disables the
private codegen-fork options only in that staging copy, points its manifest at
the sibling extracted game data, and builds the native client, two standalone
servers, and optional headless guest host there. It never regenerates `generated/default` in the
working tree. The native executable uses the repository's x86-64-v3 release
profile (AVX2-capable CPU required). The installer bundles the native client,
dedicated server, directory server, guest-backed authority host and its Mission 1
boot-input route, required runtime DLLs, controller mappings,
the Windows SDK D3D compiler redistributable, all runtime game-data directories,
and both XEX files. The game payload is checked against the extracted source by
per-file SHA-256; the isolated install check verifies every staged client/server
support file, guest-host route, and game-file contents. The current ReXGlue runtime does not
import the FidelityFX DLL at startup; if the optional `amd_fidelityfx_dx12.dll`
is absent, the game still starts and the native FSR option is unavailable.
The stock 0.10.0 code generator emits a different guest dispatch layout, so the
installer build disables optional generated dispatch-trace probes; the host and
game runtime hooks still build. The packaging script rejects identity keys,
logs, dumps, and shader-cache directories from each fresh payload tree. REA/Ghidra
sidecars and the menu-init report are also excluded. The
F5/direct-address workflow is shown in the finish-page readme and
`docs/multiplayer.md`.

The wizard images are cropped from `docs/media/gameplay-1440p.jpg` and
`docs/media/menu-graphics.jpg`. Recreate them with
`packaging/private/prepare-wizard-assets.ps1` if the screenshots or crop
dimensions change.
