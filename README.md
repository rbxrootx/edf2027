# EDF2027: EDF 3 recompilation from the 360 version (v0.3.38-beta)

![The in-game console types "spawn ant 1000 80" and a thousand giant ants pour down the Mission 1 street](docs/media/horde-1000-ants.gif)

*The in-game console (`` ` ``) spawning 1,000 ants in Mission 1.*

A native Windows port of the Xbox 360 game *Earth Defense Force 2017*
(USA/Europe), made by static recompilation with the
[ReXGlue](https://github.com/rexglue/rexglue-sdk) SDK. The game's PowerPC code
is translated to C++ ahead of time, and the port's own Direct3D 12 renderer
draws the frame.

![Mission 1 on the native renderer, rendered at 2560x1440 with FSR Native AA](docs/media/gameplay-1440p.jpg)

*Mission 1 at 2560x1440 with FSR Native AA (shown at 1920x1080).*

**No game data is included.** You need your own dump of the disc as an `.iso`
(title id `445007D3`). The tested dump is 7,835,492,352 bytes, SHA-1
`3F5DADC9BD11399E3EF260018A058F420EAFB1F6`; other dumps of the same release
should work too.

## Features

- **Native Direct3D 12 renderer**, with the original renderer (*Classic*) and
  Direct3D 11 as fallbacks.
- **High frame rates:** 60 (default), 120, 144, 165, 240 or uncapped, with the
  simulation kept at 60 Hz and the frames in between interpolated.
- **Faster recompiled code:** a simulation step takes about 25% less time than
  with the stock translation, with identical results.
- **In-game console:** spawn hundreds of enemies, collapse buildings, run
  scripts ([docs/console.md](docs/console.md)).
- **Self-hosted multiplayer foundation (experimental):** a headless ENet
  server, LAN browser, direct IP join, optional self-hosted directory,
  Docker Compose server setup, signed server identities, encrypted/replay-checked
  ENet packets, server-side Luau hooks, experimental host ant state/removal sync,
  and a guest-host bridge. F5 local hosts preserve EDF movement and collision;
  the host pose comes from the local guest and remote client poses are relayed
  with a server-side speed bound.
  Shared mission combat and its headless Mission 1 route remain unverified.
  ([docs/multiplayer.md](docs/multiplayer.md)).
- **Low input latency:** median input-to-photon 18 ms instead of 42 ms.
- **AMD FSR 3.1** anti-aliasing, **any resolution and aspect ratio** (21:9 and
  32:9 Hor+, HUD kept in 16:9).
- **Live settings menu (F1)**, controllers via SDL3 or XInput, native keyboard
  and mouse with rebindable keys, optional manual reload.
- **Built-in disc extraction** and quick loading.

## Requirements

- Windows 10 or 11, 64-bit, and a Direct3D 12 GPU.
- A CPU with AVX2 and FMA (Intel Haswell / AMD Zen or newer). For older CPUs,
  see [building without x86-64-v3](docs/building.md#older-cpus).
- About 6 GB of free disk space.

## Quick start

1. Unzip the release and run `edf2027.exe`. Keep the DLLs next to it.
2. On the setup screen, pick your `.iso` (or an extracted folder containing
   `default.xex`). It is extracted to `%APPDATA%\edf2027\game\`.
3. Press **Start** (Enter) twice at the title screen.
4. Press **F1** at any time for the settings.

Settings are saved to `%APPDATA%\edf2027\edf2027.toml`; delete it to run setup
again.

## Settings and keys

![The F1 menu, Graphics section](docs/media/menu-graphics.jpg)

| Key | |
|---|---|
| **F1** (pad: Back + Start) | Settings menu. Pauses the game; most settings apply live. |
| **F2** | Performance overlay |
| **`** | Console ([docs/console.md](docs/console.md)) |
| **F4** / **F3** / **F9** | ReXGlue settings / debug overlay / log console |
| **Esc** | Quit |

![Performance menu](docs/media/menu-performance.jpg)

![Performance overlay (F2) with the frame rate uncapped](docs/media/overlay-uncapped.jpg)

*The performance overlay (F2), uncapped at 2560x1440 with FSR Native AA.*

![Mission 1 at 32:9](docs/media/gameplay-32x9.jpg)

*A 32:9 window at 5120x1440: a wider view, with the HUD in the centred 16:9
area.*

Every setting, the frame-rate and latency options, and the command line are
described in [docs/settings.md](docs/settings.md).

## Controls

Any SDL3 gamepad works out of the box (XInput is an option in F1). Keyboard and
mouse are native, with raw mouse aim, and use the game's **Technical** control
type.

| Key | Action | Key | Action |
|---|---|---|---|
| W A S D | Move | Mouse | Aim |
| Left mouse / X | Fire | Right mouse / Z | Weapon zoom |
| Space | Jump / roll, confirm | Middle mouse / Q | Next weapon |
| E | Enter vehicle | Backspace | Cancel in menus |
| Arrows | Menu navigation | R | Pad X |
| Enter | Start / pause | Tab | Back: retire |
| F / C | L3 / R3 | G | Reload (optional) |
| F5 | Multiplayer server browser | | |

![Controls section with the key bindings](docs/media/menu-controls.jpg)

Everything can be rebound in **F1 > Controls**.

## Troubleshooting

- **Black window or GPU error:** try *Graphics > Renderer > Classic*, then the
  Direct3D 11 fallback.
- **Missing DLL:** `rexruntime.dll`, `d3dcompiler_47.dll` and the bundled
  Visual C++ DLLs must be next to `edf2027.exe`. The native FSR option also
  needs `amd_fidelityfx_dx12.dll`; without it, the game still runs and FSR is
  unavailable in the graphics settings.
- **"Not Earth Defense Force 2017":** only the USA/Europe release is supported.
- **Bug reports:** each run writes a log to `logs\` next to the executable (or
  `%APPDATA%\edf2027\logs`), and a crash writes a `.dmp` there. Attach both.

## Building from source

Windows only, with clang from Visual Studio 2026 and the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) 0.10.0:

```cmd
set REXGLUE_SDK=C:\path\to\sdk\win-amd64
build.cmd
```

Prerequisites, presets, codegen, tests and packaging are in
[docs/building.md](docs/building.md).

## Licenses

`LICENSES/` contains the licenses of the bundled components (ReXGlue SDK, SDL3,
Dear ImGui, AMD FidelityFX SDK, SDL_GameControllerDB, Inter and Font Awesome).
The port's own code is provided as-is. The game and its data remain the
property of D3 Publisher and Sandlot.
