# Stage 1: FNC Reborn on the sa2-nx base (PC dev bench, emulated GPU)

Status (2026-11-20): **builds and boots to the title screen** (PC, emulated GPU, clang 20.1.8 + xwin sysroot).
Not yet verified: identical behaviour to FNC Reborn past the title screen (no reference recording, no scripted input).

Verified so far:
- Codegen (classic): the same 69,717 functions as FNC Reborn's tree (sorted name lists, 0 diff lines); 0 "target not
  in any function". The two audit entries (below) add 2.
- Boot test (45 s, muted, minimized): no crash, `[FNC-60FPS] frame timer 30 Hz -> 60 Hz`, boot movie and audio play;
  the only errors are `ResolvePath(cache:\)`, which FNC Reborn's own log has too.
- Video (300 frames from 20 s): the title screen with PRESS START, 59.5 fps (out/pc/captures, not committed).
- Frame comparison against FNC Reborn (tools/compare_video.py, 640x360 captures of both, same settings, scale 1):
  - title screen, 5 points: mean abs difference 0.6 / 255 at every point;
  - scripted input `20:START,23:START,26:START,29:START,32:A,36:A` (title -> autosave notice -> intro cinematic and
    EA titles), 1 frame in 4, 14 points: 0.5-2.2 at 11 points; 9.7, 7.8 and 14.0 at three points of the fast-cut
    cinematic, where the pairs show the same shot a few frames apart (lens flare phase, camera move), no rendering
    difference visible. Not yet confirmed with a full-rate capture of those cuts.

Reference recorder (outside the repository, C:\Games\fnc-ref-recorder): FNC Reborn's unchanged project/ sources and
generated code, built with its own LLVM 19.1.5 and its own patched SDK, plus sa2_capture.cpp and, in a scratch copy
of that SDK, the fork's input_script (test input only, inactive unless set). Paths go on its command line (that SDK
reads them before its toml), so it never writes to Documents\FNCReborn (checked before/after every run).

## Base and inputs

- Tree: clone of sa2-nx `baf523b` (remote `sa2-nx`), branch `fnc-port`.
- FNC Reborn `7f68ee3`: manifest -> `app/fnc_manifest.toml` + `app/overrides.toml` (715 `[functions]` entries,
  unchanged), SDK patch -> `sdk/` (per-hunk evidence in [sdk-patch-port.md](sdk-patch-port.md)), hooks ->
  `app/src/fnc/` (unchanged copies; Apache-2.0, `NOTICE.fnc-reborn`).
- Game files: `assets/game_root` is a junction to `C:\Games\FNCReborn\dist\game` (git-ignored, nothing copied).

## Toolchain (this PC)

Neither VS 2022 Build Tools nor clang 20 was installed (only VS 2017/2019; FNC Reborn has LLVM 19.1.5). Chosen
(2026-11-20): portable LLVM 20.1.8 in `C:\Games\nx-ref\_tools\llvm`, and FNC Reborn's xwin sysroot (MSVC 14.44.17.14
+ Windows SDK 10.0.26100, the libraries of VS 2022 Build Tools 17.14) through the new `SA2_WINSYSROOT` mode of
`tools/win/env.ps1` (sets INCLUDE/LIB/VCToolsInstallDir as vcvars64 does, links with lld). Local paths live in the
git-ignored `tools/win/env.local.ps1`. devkitPro: postponed to stage 3.

## What changed in the app

- The generated-file prefix is `fnc_` (`fnc_init.h`, `fnc_pch.h`, `fnc_recomp`, ...): 72 references in 24 tool and
  CMake files renamed mechanically.
- **SA2 guest-address code is out of the build.** A recompiled function is a weak symbol, so any `sub_XXXXXXXX`
  defined by sa2-nx's hooks would replace the FNC function with the same address. Excluded until stage 2 re-derives
  each address for FNC: `src/me/*` (renderer glue + D3D hooks), `src/renut_engine` (`SA2_RENUT_RENDERER` OFF),
  `src/cpu/*` (natives at SA2 addresses, enhancements), `sa2_controls`, `sa2_dlc`, `sa2_language`, `sa2_d3d_trace`.
- Kept (address-free): video capture, crash log, PGO collection, PC sampling profiler, the debug-hooks pattern.
- The capture's frame counter came from SA2's flip hook. Stage 1 feeds it from the emulated GPU's XE_SWAP count
  (`fnc_gpu_swap_count`) instead, until FNC's flip function is identified in stage 2.
- `REX_DIET_CENSUS_ON` disabled (its counters lived in `sa2_enhancements.cpp`).

## Known stage-3 items found on the way

- FNC's 60 FPS hook (`fnc_hooks.cpp`) compares `ctx.lr`: it needs the classic codegen flavour, or a different
  trigger before `elide_lr_stores` (the optimised flavour) can be used.
- Fused `vmaddfp`/`vnmsubfp` (FNC patch, required: characters vanish without it) produce vector `fmla` on AArch64,
  which `tools/check_switch_build.py` rejects unless taught that these are intentional.
- `fast_fma.h` is an x86-64-v2 workaround. AArch64 has a native fused multiply-add: drop it on the Switch.
- sa2-nx's post-codegen rounds (`SA2_GEN_*`) are validated on SA2 only. They must be A/B-checked against FNC Reborn
  before being kept on.

## Sweeps

- setjmp 0x82C753D0 / longjmp 0x82C750B0 by shape (tools/setjmp_shape.py); evidence in app/overrides.toml.
- Crash-class audit (subagent, in a copy of the tree; report C:\Games\fnc-nx-audit\out\crash_audit.md): FNC Reborn's
  2.7 h log has 0 crash-class lines; pointer_fixpoint / pointer_audit / switch_check: two real missed entries,
  0x82AD3D30 (vtable slot) and 0x827BA428 (callback after the longjmp call), checked by hand and added; the rest
  classified as SEH markers, padding, jump-table bases. switch_check: 0 truncated / wrong / missed tables. Two
  function-pointer tables with null entries (0x82E8ECB0, 0x82E901F8) would call 0 if the game ever picked one.

## Still to do in stage 1

1. Scripted input deeper into the game: main menu, Fight Now (a fight), Legacy/Champion, Create a Fighter.
2. Full-rate captures (VideoEvery 1) of the cuts where the 1-in-4 comparison is ambiguous.
3. compare_video.py against the reference for each scene; a second fixpoint pass.
