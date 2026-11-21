# Stage 3: the Switch

Status (2026-11-20): **the first NRO is built and on the SD card; it has not run on the console yet.**

## Toolchain

devkitPro through pacman in C:\devkitPro\msys2 (the msys2 route of the devkitPro wiki, approved by the user):
devkitA64 GCC 16.1.0, libnx, deko3d, switch-libexpat / libzstd / zlib. The deko3d shader tools for the PC (SPIRV-Cross
and uam 1.1.0) from `tools/deko3d_build_tools.sh`, with MSYS2's clang64 toolchain. See docs/fnc/stage2.md.

## The first build (`tools/win/build_switch.ps1 -NoNvk`)

Renut on deko3d, no Vulkan driver (sa2-nx's release configuration), classic codegen flavour, LTO.

Two fixes were needed to configure it on a PC without nfsmw-nx's Mesa build:
- `sdk/cmake/rexglue_switch.cmake` and plume required `lib/libvulkan.a` even when the driver is not linked. Now only a
  build that links it requires it; otherwise the SDK's `thirdparty/vulkan-headers` (with `vulkan_vi.h`) supplies the
  headers.
- `build_switch.ps1` passed `-DREXGLUE_SWITCH_NO_NVK` only to its second cmake call, after the first configure had
  already checked for the driver.

All 788 steps built with no compile error. The LTO link ran its 512 LTRANS partitions one after another ("using serial
compilation of 512 LTRANS jobs": GCC did not use the make the script provides), about 37 minutes. Result:
`out/nx/sa2.nro`, 75.7 MB.

## tools/check_switch_build.py

`python tools/check_switch_build.py --classic`: **all checks pass** on the first build. What each check needed for FNC:

| Check | Result | Evidence |
|---|---|---|
| `_start` at 0 | pass | |
| codegen flavour | classic, accepted by `--classic` | FNC Reborn's 60 fps hook compares `ctx.lr`, which the optimised flavour's elide_lr_stores removes (docs/fnc/stage1.md) |
| Vulkan driver | none linked | as intended (-NoNvk) |
| vector fmla/fmls | pass: 0 unexplained | 10,673 in 914 functions are the guest's own fused VMX operations: FNC Reborn's SDK patch makes vmaddfp / vnmsubfp fused as on Xenon (characters vanish without it). Rule `vmx_fused_counts`: a function's fmla / fmls (its `.part` / `.isra` / `.cold` pieces counted with it) do not outnumber the fused VMX instructions of its guest code plus its callees' up to two calls deep (inlining). By hand first: sub_82A74A68 284 of 284, sub_82785860 174 of 193, sub_82ADD568 490 of 840. The last 2 (sub_82E44B38) are scalar fmsubs paired by GCC: guest 10 fmadds + 10 fmsubs; NRO 10 fmadd, 6 fnmsub, 2 two-lane fmla (KNOWN_VECTOR_FMA) |
| branch-to-self | pass: 3 known guest halts | below (KNOWN_HALTS, which held SA2's addresses before) |

Branch-to-self (`tools/self_loops_guest.py`, `aarch64-none-elf-objdump`). None is a polling loop that lost its barrier:
neither self-loop path has a load in the guest.
- **sub_82E815C8**: the PowerPC loads r5 once (`lwz r5,632(r11)`) and loops on `cmpwi cr6,r5,1; beq .`.
- **sub_82C87E48**: a list walk `while (r10 != r28) { if (r11) { r11 = [r11+4]; r10 = [r10] } }`. With r11 == 0
  nothing changes and nothing is read, so the console loops forever on that path too; GCC's `cbz w4; b .` is that path.
- **sub_82B2CF88**: `do { r8 += 510; r9++; r7 = (r8 + r10 + 255) / r6 } while (r11 > r7)`, registers only. divw and
  cmpw use the low 32 bits as on the PowerPC, so it is endless exactly when the console's is (r6 <= 0); GCC's
  `cmp w2,#0; b.gt .` is that case. One difference: divw by 0 is undefined on the PowerPC and 0 in the recompiled code.

## SD card layout (first test)

`E:\switch\fnc-nx\` (the user's SD card through Hekate's USB mass storage):
- `fnc-nx.nro` (the build; its title and icon are still SA2's: cosmetic, to change before any release);
- `fnc-nx.toml`: `asset_pack = false` / `asset_pack_build = false` (the single-file pack would be 5.6 GB, over FAT32's
  4 GB limit), Renut on deko3d, `fnc_frame_rate = 30`, a log in `logs/`;
- `game_root\`: the user's extracted game files (5.6 GB, largest file 1.46 GB), read as plain files.

Launch from hbmenu through a title override (hold R while starting a game): the album applet does not give the game
enough memory.

## First console runs (2026-11-21, the user's Switch, handheld)

1. **No renderer** (first NRO): exited at boot, "Failed to get Vulkan global function pointers ... Graphics
   presentation setup failed: C0000001". SA2_RENUT_RENDERER defaulted OFF, and the -NoNvk build links no Vulkan
   driver. Fixed: build_switch.ps1 passes -DSA2_RENUT_RENDERER=ON; CMake stops a no-driver build without Renut.
2. **Boots, plays, takes input** (Renut on deko3d, classic flavour; log and profile in out/console_runs/run1, not in
   the repo). Per scene, from Renut's 10-second lines:

   | Scene | FPS | Game thread (Swap to Swap) | Render thread | GPU |
   |---|---|---|---|---|
   | intro video (FMV) | 29.5-30.0 | 22-27 ms | 1.1-2 ms | 7 ms |
   | in-engine cutscene | 4.9-6.8 | 137-197 ms | 18-63 ms | 16-18 ms |
   | first playable scene | 11.3-13.6 | 65-81 ms | 13 ms | 14 ms |

   CPU-bound. The profile's per-thread CPU: FGGameRender 44 %, FGGameSim 18 %, Job Thread 4 57 %, Job Threads 0/2/3
   14 % each, all with core mask 0x6 (cores 1-2: 200 % of 200 %), while FNC's Main XThread used 0.2 % of core 0.
   The user saw: the playable scene black with broken graphics over its upper three quarters (the HUD correct), and
   static ("rusty") audio throughout; the profile's audio line shows every buffer delivered, no clipping, peak 0.55.

## Changes since, all waiting for the next console run

| Change | Commit | Verified on the PC | On the console |
|---|---|---|---|
| FNC's guest threads on all three cores (mask 0x7, preferring core 0) instead of SA2's layout | 122272d | n/a (Switch only) | not run |
| `audio_dump_output_s` on the Switch driver (the console's output as a WAV) | 122272d | n/a | not run |
| make installed in devkitPro's MSYS2; build_switch.ps1 stops without it | aefae89 | the Switch link: 37 min serial -> 9 min whole build | n/a |
| Optimised codegen flavour (FNC's own keep list of 85 functions from tools/non_argument_audit.py; read_before_write.py: 0; the 60 fps hook without ctx.lr; SA2's indirect-call list emptied) | a442d39 | fighter select 215 s: CPU 94.0 s -> 74.7 s (-21 %); captures 2.88-9.66 against classic (two classic runs: 2.95-9.71); the intro cinematic shows the same shots | not run |
| check_switch_build.py: FMA rule by fused sources (GCC duplicates paths: sub_82465158 has 3 simde_mm_fmadd_ps and 6 fmla) | this commit | all checks pass on the optimised NRO (56.5 MB; 0 branch-to-self) | n/a |

The optimised NRO has no "[enh]" flavour label (the check's flavour line falls back to --classic); its code is the
optimised flavour (24 MB smaller, the loops of KNOWN_HALTS gone). Not looked into.

### Test settings staged for that run (out/nx/sdcard/switch/fnc-nx/fnc-nx.toml, not on the card yet)

- `switch_profile_stacks = true`: per-function CPU samples (tools/switch_profile_report.py).
- `audio_dump_output_s = 20`, `audio_dump_output_from_s = 40`: logs/audio_output.wav.
- `audio_xma_in_worker = 0`: the static. The Switch decodes XMA in a worker (mode 1), the PC on the requesting thread
  (mode 0). Marathon Recompiled's Switch port fixed static caused by XMA ring-buffer data handed out before it was
  ready (https://www.gamebrew.org/wiki/Marathon_Recompiled_NX_Switch). On the PC both modes measure clean (20 s dumps
  of fighter select: no sample jump above 0.25, 0.32 % of the energy above 10 kHz in each), so only the console tells.
- Nine renderer shortcuts off (setter marks and their check, colour hand-over, lazy and direct resolve mips, clear
  repeat, depth hand-over, drawn resolves, resolve skip): the broken graphics. The PC with the Switch's sa2_renut_*
  values draws fighter select correctly (2.95-9.71 against the clean run: the same range as two clean runs), so the
  deko3d-only shortcuts (depth hand-over, drawn resolves) and the deko3d back end are the suspects.

### Not done (needs the console or the user)

- The broken graphics of the playable scene: not reproduced on the PC (the PC runs reached fighter select, not that
  scene); the next run's photo and log, with the shortcuts off, decide.
- 30 FPS in fights: not measured. The game thread needs about 2-2.5 times its speed in the playable scene. Next: the
  per-function profile, then sa2-nx's CPU rounds (native replacements of the hottest functions, PGO).
- The NRO's title and icon are still SA2's.
- The PC Renut build exited once more with 0xC0000409 at 12.7 s (no message, no dump); two of the three later runs
  ran the full 215 s.

## The broken graphics reproduce on the PC (2026-11-21, 04:20)

The console's broken scene is the prison opening's knockdown (the referee's count and the get-up prompt, the HUD the
user saw drawn correctly). The PC Renut build shows the same: a fresh profile reaches it by itself.

Repro (muted, minimized; about 3 minutes):

    tools\win\boot_test.ps1 -Exe out\pc-renut\sa2.exe -Seconds 175 -VideoAt 150 -VideoFrames 40 -VideoEvery 8 -ExeArgs
      "--sa2_renderer=renut --resolution_scale=1 --fnc_frame_rate=30 --user_data_root=<an empty folder>
       --cache_root=<its cache> --input_script=20:START,23:START,26:START,29:START,40:A,55:A,70:A,85:A,100:A,115:A,130:A,145:A"

- FNC Reborn (C:\Games\fnc-ref-recorder, the emulated GPU), same input and a fresh profile: the knockdown view is a
  bright first-person picture from the canvas, the ring sideways.
- Renut: almost black, a thin strip of the scene at the top; the count ("2", "4") and the prompt correct.
- Renut with colour hand-over, lazy and direct resolve mips, clear repeat, resolve skip and setter marks off: the same.
  Not those shortcuts. So the nine shortcuts staged off for the console are no longer the lead (harmless there; the
  deko3d-only ones are still tested by it). Next: trace that scene's draws, resolves and the full-screen pass that
  makes the knockdown view, against the reference.

## Fight rendering (2026-11-21, 05:00): one cause fixed, one open

Every tiled 4x MSAA frame (the fights) is broken in Renut on the PC as on the console; menus, cinematics and fighter
select are not tiled with MSAA and render correctly. With a fresh profile the prison fight starts by itself (repro
above; the fight at about 105 s, the knockdown at 150 s). FNC Reborn, same input: the ring, the crowd, the opponent.

1. **Fixed: the targets were one tile tall.** FNC's own BeginTiling / EndTiling (sub_82CDA978 / sub_82CDAFE0) run as
   recompiled code and wrote the device's tiling words, but nothing told Renut's render thread
   (SetTilingExtentLocked / SetTilesLocked, which only Renut's N&B BeginTiling hook called), so the host target was
   the tile's 1280x256 surface and the bands at rows 256 and 512 resolved nothing. Wrappers in draw.cpp's FNC
   section keep the game's code and add the render thread's state. Checked: "[fnc tiling] BeginTiling: 3 tiles,
   extent 1280x720, first (0,0)-(1280,256)"; the fight's draws now go to 1280x720 targets (draw log: 258 of 356).
   - Trap found on the way: tools/direct_calls.py (optimised flavour) turns calls to functions without a hook into
     __imp__ calls when it runs, so **a hook added after the codegen is never called** until it runs again
     (`python tools/direct_calls.py --redo`). It had made the two new hooks dead; run it after adding any hook.
2. **Open: the fight's 3D geometry draws as stretched shards** (top right, the rest black), with the targets fixed.
   Ruled out: the tiling extent, predication (FFFFFFFF at BeginTiling: every tile; flags A1 04 30 00), the renderer
   shortcuts. Cinematics with the same boxers render, so it is the fight's own draws: next, its vertex streams,
   declarations and shaders draw by draw (banjo_renut_diag_draw_log_s, the reference's frames).

The Switch NRO staged in out/nx/sdcard/switch/fnc-nx has fix 1 (built 2026-11-21 05:00, all checks pass).

### Ruled out after the tiling fix (2026-11-21, 05:20)

- Every Renut speed-up and shortcut off at once (banjo_gpu_blend_skip, _bool_spec, _depth_only, _depth_words_view,
  _output_trim, _tess_grid; banjo_renut_cpu_memos, _depth_words_lazy, _depth_words_stencil, _device_swizzle,
  _exact_streams, _keep_gpu_written, _linked_stages, _pipeline_memo, _point_minmax_radius, _precise_barriers,
  _push_point_only, _resolve_skip, _shared_dedupe, _upload_cached; sa2_renut_colour_handover, _lazy_resolve_mips,
  _direct_resolve_mips, _clear_repeat): the same shards.
- Surface aliasing: the fight draws through BD26BC40 / BD26BC00 and EndTiling resolves through the surfaces BeginTiling
  recorded (BD1F1BC0 / BD1F1B80); Renut keys host targets by EDRAM view (TargetKey: base, pitch, MSAA, format,
  scale), so objects over the same view share a target. Not checked: that those two pairs really have the same view.

Next (needs new diagnostics): the two pairs' SurfaceInfo / ColorInfo at the fight; a dump of the fight's host targets
after the depth pre-pass and after the colour draws; then the first wrong draw, its vertex streams and declaration,
against the reference.

## The fights render (2026-11-21, afternoon)

The second console run (all three cores, XMA mode 0) showed the fight green and black with stretched shapes, the HUD
correct; the menus' audio clean, the fight's crackling. On the PC the same frames came out as shards over black. The
causes, in the order found, each checked against FNC Reborn's frames (fresh profile, the prison fight):

| Cause | Fix | Commit |
|---|---|---|
| The tiled pass's host targets one tile tall (1280x256) | the render thread told of FNC's own BeginTiling / EndTiling | b8ac70a |
| BeginTiling's clear covered rows 0-255 only | whole-target while FNC's BeginTiling runs; the tiles told before it runs | f9f4d54 |
| Texture and vertex fetch addresses in the 0xE0000000 region read 4 KB early | `TextureFetch::GpuPage`, the same + 0x1000 for vertex fetches (the bytes at the CPU address equal physical + 4 KB) | f9f4d54 |
| **GPU-owned shader constants**: the engine (sub_8245AD18 / sub_8245AD90) writes them into the command buffer (sub_82CE5AC0) or has the GPU load them (LOAD_ALU_CONSTANT, sub_82CE5B48); the device image Renut reads never had them (the camera c0-c3, the crowd's c40-c195, the pixel constants) | the same data copied into the image | f9f4d54 |
| The copies set no pending bits, so the snapshot deltas never passed them on (the crowd collapsed) | m_Pending marked; the loads copied again at the next draw | 0cbb31a |

Found on the way: tools/direct_calls.py (optimised flavour) makes calls to functions without a hook direct, so every
new hook needs `python tools/direct_calls.py --redo` before it is called at all.

PC result: the ring, both fighters, the crowd, the knockdown view; the selection screen unchanged (0.50 median against
a clean run). On the console: built, not yet run.

Also in that build: the game's mixer thread (RwAudioCore) one priority step above the other guest threads, for the
fights' crackle (the cores full; every audio buffer delivered). Not yet heard on the console.

### Still open in the fights' graphics (PC, 2026-11-21)

- **The fighters cast no shadow.** FNC Reborn draws a sharp fighter shadow on the canvas; Renut none (default
  settings), or with the depth shortcuts off (banjo_gpu_depth_only, banjo_gpu_depth_words_view,
  banjo_renut_depth_words_lazy, banjo_renut_depth_words_stencil, banjo_renut_resolve_skip) a dark diamond: the whole
  projected area shadowed. Both read as an empty shadow map. Ruled out: the caster draws (the frame's first 8, the
  fighters' depth-only vertex shader 9F1398D86E8A5F55) and their viewport and scissor (512x512, logged). Next: the
  shadow map's depth resolve (surface format 33) read back as a texture (format 34), Renut's depth-words path.
- **The crowd looks thinner** than the reference's (gaps between inmates). Not looked into.
- Shadow, also ruled out (2026-11-21): the map is sampled as its own resolve destination (texture BD26B840, guest
  format 23 = 24_8_FLOAT, depth words present), and the words pass encodes the 512x512 surface (format word 1A220197,
  format 23) as 20e4 float, as the game samples it. Left: what the caster draws write into the host depth target
  (a dump of it before the resolve), and the shadow comparison in the canvas's pixel shader.

## Third console run (2026-11-21, evening): fights playable

The fight plays on the Switch (a knockdown scored): 13-20 FPS, game thread 43-67 ms, render thread 22-27 ms, GPU
23-31 ms (the renderer shortcuts still off on the card). The user saw: the fighters' shorts transparent and gloves
black (custom gear textures), shadows as big grey-black boxes, a thinner crowd; the audio better but crackling in
fights.

- A 1024x1024 colour target drawn with a 512x512 depth bound (warned 8 times): such a depth is now left out for draws
  that neither test nor write depth nor use stencil (draw.cpp GatherTargets; past its edge D3D12 is undefined and
  Maxwell faults). It turned out to be the shadow system's target: the PC now shows the same dark shadow area as the
  console. Not yet the gear.
- The fighters' colour pass (vertex shader A8AA59AFFAB9C9F0) samples 9 textures; slot 1 (texture BD247D80, fetch
  base EEE3B054, 1024 wide) is all zeros at the CPU's address too: its contents are not in memory. Next: who fills it
  (a GPU resolve or render target, or a CPU copy from another address).
