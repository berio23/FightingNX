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

## PC bench: Switch shortcut config + custom gear textures fixed (2026-11-21)

1. **Switch SD shortcuts re-enabled (`E:\switch\fnc-nx\fnc-nx.toml` and `out\nx\sdcard\switch\fnc-nx\fnc-nx.toml`):**
   deleted the 7 shortcut overrides (`sa2_renut_colour_handover`, `sa2_renut_lazy_resolve_mips`,
   `sa2_renut_direct_resolve_mips`, `sa2_renut_clear_repeat`, `banjo_deko3d_depth_handover`,
   `banjo_deko3d_draw_resolves`, `banjo_renut_resolve_skip`), keeping `sa2_renut_setter_marks = false` and
   `sa2_renut_setter_marks_check = false`. Ready for console testing (not yet tested on Switch).
2. **Issue 1 root cause (fighters' shorts transparent / gloves black):**
   the earlier lead (`player_body_damage_gp` / `A8AA59AFFAB9C9F0` / `BD247D80` at `EEE3B054`) was the body/skin/damage
   pass; the shorts and gloves are `player_shorts_coarsecotton` (`vs 747EC94F3B1B2E90`, `ps 2F29D47F180948C0`) and
   `player_glove_leather` (`vs 1C131265EAD45A5E`, `ps 71486B610EEC293D`). FNC's `TextureCompositor` (`sub_827E50C0`)
   draws gear layers into EDRAM, resolves level 0 (`sub_827D3450`) and generates mips `1..N` via `texturemip.fx`
   (`sub_827D5510`) into `dest_level = 1..N`, then locks the resolved texture on the CPU (`sub_82CD95F8`, the central
   subresource lock) to copy (`sub_827CC358`) or tile-to-linear + DXT-compress (`sub_827DC238`) into the gear textures.
   Renut previously ignored `dest_level > 0` on colour resolve targets (allocating 1 guest level and overwriting
   level 0) and never read GPU resolves back to guest RAM on CPU lock, so `DxtCompression` compressed zeros.
3. **Fix:**
   - `app/src/renut_engine/gpu/render/textures.h`, `textures.cpp`: allocate guest mip levels up to
     `min(fetch.MaxMipLevel() + 1, size_levels)` (capped to `host_levels_max`, `extra_levels = 0` when
     `guest_levels > 1`) on colour resolve targets; track `HostTexture::readback_dirty` and resolve-readback
     aliases (`FindResolveAliasLocked`, `NoteResolvePendingReadback`, `EnsureResolvePendingReadback`,
     `HasPendingReadback`, `CurrentPendingReadbackSeq`, `ClearPendingReadback`).
   - `app/src/renut_engine/gpu/render/draw.h`, `draw.cpp`: pass `level` into `CopyColourResolveLocked` and
     `ColourHandOverResolveLocked`, clamp resolve rects to `dest_level` host dimensions, stage MSAA resolves for
     `dest_level > 0 || slice > 0 || z > 0`, mark `readback_dirty[slice] |= (1u << dest_level)`, and add
     `ReadbackResolvedTextureLocked` (batched `ReadbackBuffer` copy + `SubmitAndWaitLocked` + channel
     unswizzle/scale-average/endian-swap/retile to guest RAM + `rex::memory::MarkStoreCardsAt` + `++g_flush_serial`).
   - `app/src/renut_engine/gpu/backend/queued.cpp`: call `NoteResolvePendingReadback` on colour resolves in
     `QueueResolve`.
   - `app/src/renut_engine/gpu/d3d/hooks/resource.cpp`: add `BANJO_RENUT_HOOK_RAW(sub_82CD95F8)` to flush deferred
     vertices, run `ReadbackResolvedTextureLocked`, clear pending readback, and `render_thread::Drain()` when
     `HasPendingReadback` is true; regenerated direct calls (`python tools/direct_calls.py --redo`, 161717 calls in 3
     files, 1392 hooked addresses respected, 3 hooked callers, 0 `__imp__sub_82CD95F8(`).
4. **PC verification (`out\pc-renut\sa2.exe`, `--sa2_renderer=renut --resolution_scale=1 --fnc_frame_rate=30`):**
   - 16 `[fnc readback]` hits in `out\pc-renut\logs\fnc-nx_065.log` (`fmt 6 host 20 endian 2 tiled 1 swizzle 60A`,
     `128x128` mips `1` through `1024x1024` mips `4`), 0 `out of range`, 0 `unsupported format`, 0 `flash`,
     0 `Device lost`, `plume_stderr.log` size `0`.
   - Compared 300 captured frames (`video_105s.rgba`) against `C:\Games\fnc-ref-recorder\build\captures\video_105s.rgba`:
     overall MAD `27.10` (before, `owned2_105s.rgba`) -> `15.10` (`video_105s.rgba`); active frames (272, +-15 aligned)
     `25.40` -> `11.98`. Walkaway `ref[2]` shorts+wraps crop `(170, 170, 245, 250)` MAD `41.46` (`gear.rgba[76]`) ->
     `29.95` (`video_105s.rgba[1]`, mean luma `100.4` -> `113.9` vs ref `138.2`); walkaway end `ref[14]` full MAD
     `6.66` (`12.57` crop) -> `0.10` (`0.00` crop); knockdown `aft[238]` vs `ref[239]` shorts+wraps `(250, 140, 370, 280)`
     MAD `13.19` (mean luma `66.4` vs ref `68.8`), `aft[255]` vs `ref[256]` full MAD `1.93` (crop `1.47`). Visual
     inspection confirms white/grey patterned shorts with waistband stripe and white taped hand wraps match the
     reference (both were previously black).
   - **Not yet verified:** Switch NRO rebuild and console run with this fix (or the SD-card shortcut change), PC
     `--resolution_scale > 1`, and coloured gloves in non-prison arenas. Shadows and crowd remain open.

## Fourth & fifth console runs (2026-11-21): shorts and gloves fixed on Switch

1. **Run 4** (`out/console_runs/run4/fnc-nx_006.log`, commit `136a4bc` + 7 renderer shortcuts re-enabled,
   `sa2_renut_setter_marks* = false` kept):
   - **Gloves fixed on hardware:** `16` `[fnc readback]` info hits during fighter load (matching the 16-log cap), `114`
     total deko3d copy readbacks, `0` readback errors, and `0` per-frame fight readbacks.
   - **Shorts still missing/transparent in Run 4** because `224` fight shaders (`shader_capture/*.bin`, `0` overlap
     with the `879` boot/title/intro cache) were not yet in the local Switch shader package (`player_shorts_finecotton*`,
     `player_shorts_satin_gp`, `player_satin*`, `crowd_instancingshader`, `farcrowd_instancingshader`, hair, mouthguards,
     ring girls).
   - **Performance with the 7 shortcuts back on:** fight GPU time dropped from `25.7-31.5 ms` (run 3) to `22.7-26.2 ms`,
     while fight FPS stayed `12.9-17.2 FPS` (`game thread` `50.3-69.2 ms`, `render thread` `25.0-26.8 ms`) because the
     Switch is CPU-bound across 6 guest threads + render thread on 3 cores. Top `rex_profile.log` hotspots:
     `FGGameSim` (`sub_8283AEA0` 65.9% incl., `sub_82C60688` 17.1%), `Job Thread 0/2/3` (`sub_826D8178` 11.8-12.6%,
     `sub_8283B828` 7.7-9.9%), `Job Thread 4` (`sub_82A8F798` 15.5%, `sub_82A8B590` 8.7%, XMA `decode_frame` 7.9%),
     `FGGameRender` (`CaptureDelta` 6.6%, `sub_82CE5670` 65.7% incl.), render thread (`XXH3_hashLong_64b_withSeed` 17.1%,
     `ExpandElement<2u>` 16.4%, `ConvertVertices` 7.2%).
2. **Run 5** (after rebuilding the local Switch shader package with all `1,103` shaders — `879 + 224` — and passing
   `tools/check_switch_build.py --classic` on `56,939,015`-byte `fnc-nx.nro`):
   - **Shorts and gloves both render and look good on Switch** (verified by user on console).
   - **Remaining on Switch:** fights are playable but still very laggy (CPU-bound); **sporadic black artifacts**
     sometimes appear on fighters' bodies; **shadows are still black boxes** (still open).


## Shadows and CPU round (2026-11-21, evening)

### Root causes
1. **No enhancement switch was ever active on FNC.** `FncApp` never called `sa2::enh::Resolve()`, so every
   `SA2_ENHANCEMENT` (`banjo_renut_capture_pool`, `banjo_renut_depth_words_stencil`, store cards, NEON paths, ...) had
   `.active == false`. Symptoms: `new`/`delete` of a `DrawCapture` per draw (Run 5 profile: `_malloc_r` 15.1 % of
   `FGGameRender`, `_free_r` 5.2 % of the render thread) and the depth-words resolve never given the stencil view.
   Fix: `fnc_app.h` calls `sa2::enh::Resolve()` (OnPostInitLogging) and `StartReport()` (OnPostSetup).
2. **Stencil never reached `depth_resolve.hlsli` on D3D12.** `SurfaceStencilSlotLocked` was Vulkan/deko3d-only and plume's
   D3D12 back end had no stencil SRV. Fix: `RenderFormat::S8_UINT` view (`X32_TYPELESS_G8X24_UINT` /
   `X24_TYPELESS_G8_UINT`, `PlaneSlice = 1`) in `plume_d3d12.cpp`; the gate removed in `surfaces.cpp`.
3. **`flatShadowTextureSampler` (BD1EF840) froze on its first frame.** It is both a depth-words consumer and a colour
   resolve target on the same base page; `BindTextureLocked` skipped `DepthWordsSource` once `gpu_written` was set, and
   `WordsViewSlot` applied the colour target's red/blue swap to the raw words. Fix in `textures.cpp`.
4. `draw.cpp` `sa2_renut_chain_reuse` compared SA2's device offsets (12816..12835, 12920..13023); now FNC's
   `kDeviceOffsetRenderTargets..DepthStencil+3` and `kDeviceOffsetTextures..+kTextureFetchSlots*4-1`.

### CPU / render thread
- `src/fnc/fnc_hot_guest.cpp`: `g_banjo_store_cards = true` (the generated pch defines `REX_CARD`; was hard-coded
  false in `fnc_stage1_stubs.cpp`), and guarded native replacements (first 256 calls + 1 in 4096 checked against the
  original): `sub_8229B520` (CRT memcpy), `sub_82A8B590` (8-wide IIR filter loop, no loop barrier; declines
  `r7 % 8 != 0`), `sub_82A8C308` (vmaddfp array accumulate, no loop barrier).
  `sub_8283B828` was tried and **removed**: the guard found `r3` 0x1f5 vs 0x1f4 (fnc-nx_073.log) on its first fight call.
- `xma_decoder.cpp`: no spurious worker wake-up in mode 0. `vertex_format.cpp`: NEON half floats, compile-time
  normalised decode, identity-swizzle fast path.

### PC verification (fnc-nx_073.log, same input script, fresh profile, `--banjo_renut_store_cards_verify=true`)
- Fresh `BD1EF840.z` in the fight: 88.6 % = 255 (lit), 7.2 % = 0 (shadow), 4.1 % soft edge — a fighter silhouette, no box.
- Store cards: ~86k copies a 10 s window proven by cards (~3.0-3.3 GB not hashed), `0 changed under clean cards`,
  `0 textures changed under clean cards`. Render thread 0.65-7.7 ms, GPU 3-8 ms a frame (D3D12, GTX 1070).
- 300 frames vs `fnc-ref-recorder` `video_105s.rgba` (640x360): ±15-aligned MAD **2.75** (active frames 2.99), was
  15.10 / 11.98 after the gear fix. Fight frames 130/150/180/210: MAD 2.8-3.9, ring floor ROI 3.9-4.5; by eye the
  fighter shadows on the canvas match the reference. Knockdown 238/252: MAD 7.7/9.6 — the remaining gap is the thinner
  crowd (issue 3), not shadows. Frames 0-1 differ only because the reference is 12 frames ahead there.
- **Not verified:** Switch console run of this build; sporadic black body artifacts (not reproduced on PC).

### Switch regression of the 20:02 build (Switch log fnc-nx_008.log) and the rollback
- On the console the `Resolve()` build was much worse: a ~1 minute fight load, black cutscenes, glitching fighters,
  slower frames. The log: `uam 139 compiles (avg 972 ms)` of run-time shader variants (was 35 in fnc-nx_007), windows
  of 0.05-2.8 fps while they compiled. Cause: `Resolve()` with both masters on (their default) switched on every
  SA2 renderer enhancement gated by `banjo::EnhancementActive` (depth-only draws, output trim, early-Z, bool spec, ...)
  and 22 SA2 `kExternal` keys, none proven on FNC/deko3d; the PC run could not show it (no deko3d, no uam).
- Fix: `fnc_app.h` sets `sa2_cpu_enhancements`, `sa2_noncpu_enhancements` and `banjo_renut_store_cards` to false
  unless the configuration sets them (the SD toml states them too). That is the enhancement state of every earlier
  build. The shadow fix (`textures.cpp`) does not depend on them: PC run fnc-nx_075 with them off renders the fight,
  shorts and crowd correctly with no shadow boxes (its fight diverged from the reference's timeline, so no MAD).
- Each enhancement must now be turned on singly and proven on the console before it stays on.

## Performance round (2026-11-21, night) and the console test set

Switch run fnc-nx_009 (masters off, shadows fixed): fights 12-18 fps; game thread 44-71 ms, render thread 21-33 ms, GPU
25-27 ms at the stock handheld clocks (CPU 1020 / GPU 307.2 / memory 1331.2 MHz), 3 cores, ~305 % of 300 % busy.
The fighters flicker (heads, arms) since the shadows work.

### Done
- **GPU clock** (`fnc_gpu_boost`, default on): Horizon's official handheld 0x92220008 (GPU 460.8) through the SDK's
  checked `switch_apm` path; **`fnc_ram_1600`** (default on) prefers its sibling 0x92220007 (GPU 460.8 + memory 1600,
  also official): `RexSwitchApmPreferRam1600`. The result is in `logs/rex/rex_stderr.log` (`[apm] ...`).
- **Hot-function order that applies**: `app/hot_functions.ld` was SA2's list (none of its functions exist in FNC), and
  `tools/hot_order.py` named `.text.sub_X` where GCC puts a recompiled body in `.text.__imp__sub_X` (sub_X is a weak
  alias): no game function was ever moved, in SA2 either. Fixed; regenerated from fnc-nx_009 (symbolised with a
  rebuild of that NRO: 44 bytes differ, build IDs and a timestamp). The hottest game functions are now in the first
  ~400 KB of .text. `build_switch.ps1` keeps every ELF as `out/nx/elf/<NRO sha256[:16]>.elf` for later profiles.
- **Heap first-fit hints** (`heap_alloc_hint`, default on; `sdk/src/system/xmemory.cpp`): per heap, the lowest page
  that may be free and the highest; `AllocRange` starts its scan there (Release/Reset/Restore/Initialize move them
  back), so it returns the same address as the full scan. PC fnc-nx_081 with `heap_alloc_hint_verify=true`: 2,048+
  allocations through boot, menus and a fight, 0 differences. Kept outside the class: xmemory.h unchanged.
- **Ruled out: depth bias.** The renderer stores SetRenderState 51/52 but never applies a host bias; a PC fight
  (fnc-nx_079, diagnostic since removed) showed FNC never sets a non-zero one.

### On the SD card for the next console run (toml only, no code)
- `banjo_deko3d_depth24 = false`, `banjo_deko3d_depth_handover = false`: the PC's depth path (32-bit float targets,
  every depth resolve drawn). The flicker is console-only; on the console the 1280x720 scene depth (whose stencil the
  shadow pass reads) was 24-bit unorm. If the flicker is gone, turn one back on at a time.
- `banjo_renut_store_cards = true` + `banjo_renut_store_cards_verify = true`: same pixels as off (everything is still
  hashed), and `[cards] ... changed under clean cards` lines say whether the cards hold on the console. None -> verify
  off next: XXH3 was ~25-35 % of the render thread's fight samples in fnc-nx_009.

### Profile of fnc-nx_009 (fight intervals, symbolised)
- Loading into the fight: `renut::gpu::ReadbackResolvedTextureLocked` ~78 % of the render thread (the gear
  compositor's synchronous GPU->CPU readbacks, commit 136a4bc). During the fight: XXH3 hashing on the render thread;
  `BaseHeap::AllocRange` 9.5 % of FGGameSim (a page-by-page first-fit scan); game code `sub_8283B828` (cloth),
  `sub_826D8178`, `sub_82A8F798`, `sub_82E4CC40`, `sub_82789120`, `sub_82A8B590`; `ExpandElementNormalized` (vertex
  expansion, render thread).

### Open
- PC only, intermittent (2 of the last 5 runs, never before nor on the console): `[FATAL] Call to invalid or
  unregistered function at guest address 0x00000000` on the audio thread ~2-3 s after start, right after the game
  opens `sound_crowds.big` / `sound_fesfx.big`. The audio system checks the callback address, so a null pointer
  inside the game's own callback: a start-up race in the sound engine that the renderer's timing exposes.

## Console run `fnc-nx_010` diagnosis, flicker fixes, CPU/NEON speedups (`2026-11-22`)

- **Log & profile findings (`E:\switch\fnc-nx\logs\fnc-nx_010.log`, `rex\rex_profile.log`)**:
  - Fight ran at 11.9–14.7 fps (`game thread` 59–75 ms, `render thread` 23.8–28.0 ms, `GPU` 14.6–20.7 ms; avg 13.67 fps across 152 fight blocks, CPU 304.8% at 1020 MHz CPU / 460.8 MHz GPU / 1600 MHz RAM).
  - Turning off `banjo_deko3d_depth24` and `banjo_deko3d_depth_handover` did not stop the console flicker; `store_cards_verify` reported `0` changed pages across the fight.
  - Because `app/src/fnc/fnc_app.h` forced `sa2_cpu_enhancements = false` alongside `sa2_noncpu_enhancements = false`, all CPU enhancements and `banjo_renut_store_cards` were disabled by default, leaving XXH3 hashing (`21.9%` of the render thread), scalar `ExpandElementNormalized` (`17.4%`), `ConvertVertices` (`9.2%`), and heavy un-hooked guest loops active.
- **Flicker & hazard fixes**:
  - `app/src/renut_engine/gpu/render/textures.h`, `textures.cpp`, `surfaces.cpp`: unified transitions into `TransitionRenderTextureLocked` and added `IsCopyOrResolveLayout` (`COPY_SOURCE`, `COPY_DEST`, `RESOLVE_SOURCE`, `RESOLVE_DEST`) checked on **both** current and target layouts so transitions out of copy/resolve layouts always emit `RenderBarrierStage::COPY` (`DkBarrier_Full`) instead of fragment-only barriers and are never skipped by `banjo_deko3d_barrier_lean`.
  - `app/thirdparty/plume/plume_deko3d_commands.cpp`: added missing `{}` around the `if` body in `invalidateZcullAfterCopy`.
  - `app/src/renut_engine/gpu/render/draw.cpp`:
    - Added `IsHandoverTarget` checking `g_handover_targets->depth` and all `colors[0..color_count-1]` in `FillTextures` memo invalidation.
    - Added `color_count` / `colors` comparison to `TexturesReuse` (`sa2_renut_chain_reuse`).
    - Added `depth_control_bits` (`DepthControl & 0x7u`) comparison to `TargetsReuse` to match `GatherTargets`' smaller-depth drop condition.
    - Added `SHADER_READ` transition for `dest->words.get()` at the end of `ResolveDepth`.
  - `app/src/fnc/fnc_app.h`: defaulted `sa2_cpu_enhancements = true` and `banjo_renut_store_cards = true`, while keeping `sa2_noncpu_enhancements = false`, `banjo_deko3d_zcull = false`, and `banjo_deko3d_depth_compression = false`.
- **CPU & Render-Thread Speedups**:
  - `sdk/src/system/util/object_table.cpp`: updated `ObjectTable::FindFreeSlot` to advance `last_free_entry_` past allocated slots (skipping slot 0) and updated `RemoveHandle`/`PurgeAllObjects` so handle creation avoids repeated O(n) linear scans.
  - `app/src/renut_engine/gpu/render/vertex_format.cpp`: vectorized `ExpandElementNormalized<kEndian, kSigned, false>` on AArch64 (`NeonExpandPackedNormalized`) across `k8_8_8_8`, `k2_10_10_10`, `k10_11_11`, `k11_11_10`, `k16_16`, and `k16_16_16_16` including arbitrary swizzle permutations.
  - `app/src/fnc/fnc_hot_guest.cpp`: added native/localized replacements for `sub_8283B828` (lock-free job queue tag-min search, `SA2_HOT_HOOK_COPY` with a 12-case deterministic self-test against `__imp__sub_8283B828`), `sub_826D6D18` (register-localized quad packer with bit-exact pack helpers, `Cmp::kAll`), `sub_82789120` (barrier-free SIMD skinning loop, `Cmp::kAll`), and `sub_82A87020` (barrier-free audio mix/resample loop, `Cmp::kAll`); switched concurrent `sub_8229B520` (`memcpy`) to `SA2_HOT_HOOK_COPY` after observing worker-thread buffer mutation during `Check()`.
  - Re-ran `python tools/direct_calls.py --redo` (`0` remaining `__imp__sub_*` direct calls for all 7 hooks) and rebuilt `app/hot_functions.ld` via `tools/hot_order.py` from `fnc-nx_010`'s `rex_profile.log`.
- **Verification & Deployment**:
  - PC verify Fight Now (`verify_fix2`, `out\pc-renut\logs\fnc-nx_090.log`): `sub_8283B828` self-test `12/12 passed`, `9,784,727` guarded hook checks (`>4096` each on `sub_82A8B590`, `sub_82789120`, `sub_82A87020`, `>1,048,576` each on `sub_826D6D18`, `sub_82A8C308`) with `0` differences, `0` flush/codegen self-check mismatches, `91,810` `snapshot_deltas` checks (`0` differ), `99,240` vertex/index + `68,096` texture card checks (`0` changed), `0` `chain_reuse_verify` diffs, and clean fight frames (`verify_f0/29/59.png` from `verify_fix`).
  - Clean PC Fight Now (`clean_fix`, `out\pc-renut\logs\fnc-nx_091.log`): locked `30.00 fps` with `3.47 ms` render-thread frame time.
  - Switch release build (`out\nx\sa2.nro`, `57,139,719` bytes, SHA-256 `6A009E98C08155FF90B2E163392DCF69A310522735437E3638D17BE9EF136071`, ELF `out\nx\elf\6a009e98c08155ff.elf`): `tools/check_switch_build.py --classic` passed all checks; `nm` confirmed all 7 hook symbols override `__imp__sub_*` near the front of `.text`. Deployed to `E:\switch\fnc-nx\fnc-nx.nro` (SHA-256 verified) with `fnc-nx.toml` tuned for Switch (`deko3d_shaders.bin` preserved).
