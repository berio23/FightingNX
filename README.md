<div align=center>

<img src="app/icon.jpg" alt="FightingNX" width="25%">

# FightingNX

A static recompilation of **Fight Night Champion** (Xbox 360) for the **Nintendo Switch** (homebrew) and Windows.

</div>

> **This repository contains no game data.** No code, graphics, sound, shaders or other files from Fight Night
> Champion are included, and none are distributed. To build and play you need **your own legally obtained copy of the
> game** (an Xbox 360 disc you own, dumped yourself). The game's code is translated from your copy on your own
> machine during the build, and its data files are read from your copy at run time.

## Status

Work in progress. Not playable yet.

| | Windows (development bench) | Switch |
|---|---|---|
| Boots, menus, video cinematics | yes | yes |
| Fights | 3D geometry broken (renderer) | 3D geometry broken (green, shards) |
| Frame rate in fights | full speed | 15-17 FPS (CPU-bound; target 30) |
| Audio | clean | clean in menus, crackles in fights |

The current state of each problem, with measurements and evidence, is in [`docs/fnc/stage3.md`](docs/fnc/stage3.md).

## How it works

- [ReXGlue](https://github.com/rexglue/rexglue-sdk) translates every PowerPC function of the game's executable to
  C++ ahead of time; the C++ is compiled for x86-64 (Windows) or ARM64 (Switch). Nothing is emulated at run time.
- The ReXGlue runtime stands in for the Xbox 360's kernel services (files, threads, memory, input, audio).
- **Renut**, a Direct3D-level renderer, draws through deko3d on the Switch and Vulkan / D3D12 on Windows, instead of
  emulating the Xbox 360's GPU.
- The game-specific fixes (the function list, the SDK patch, the 60 FPS unlock on PC) come from
  [FNC Reborn](https://github.com/whompay/FNCReborn), the Windows recompilation of the game; the Switch work builds on
  [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx).

## Building

You need your own dump of the game (the extracted disc contents, with `default.xex` and the data folders).

- **Windows bench and toolchain:** [`docs/fnc/stage1.md`](docs/fnc/stage1.md) (portable LLVM and an xwin sysroot,
  `tools/win/env.ps1`).
- **Renderer and shaders:** [`docs/fnc/stage2.md`](docs/fnc/stage2.md) (the shader capture and translation loop; the
  shader packages are built from your copy and never committed).
- **Switch:** [`docs/fnc/stage3.md`](docs/fnc/stage3.md): devkitPro (devkitA64, libnx, deko3d) through pacman, then
  `tools\win\build_switch.ps1 -NoNvk`. The NRO goes to `sdmc:/switch/fnc-nx/` with your game files in `game_root/`
  next to it. Start it from hbmenu through a title override (hold R while launching a game) for full memory.

The general sa2-nx documents in [`docs/`](docs) describe the shared tools; where they name Sonic Adventure 2 files,
use your Fight Night Champion dump instead.

## Credits and licences

- [sa2-nx](https://github.com/ChanseyIsTheBest/sa2-nx) by ChanseyIsTheBest: the Switch SDK fork, tools and the Renut
  integration this port is built on (GPL-3.0, see [`LICENSE`](LICENSE)).
- [FNC Reborn](https://github.com/whompay/FNCReborn): the game's function list, SDK patch and hooks (Apache-2.0, see
  [`NOTICE.fnc-reborn`](NOTICE.fnc-reborn)).
- [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), reNut by masterspike52 (through banjo-nx), plume,
  XenosRecomp and the other libraries under `app/thirdparty` and `sdk/thirdparty`, each under its own licence.

Fight Night Champion is a trademark of Electronic Arts. This project is not affiliated with or endorsed by Electronic
Arts, Microsoft or Nintendo.
