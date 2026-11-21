# FNC Reborn's SDK patch on the sa2-nx SDK fork

Source: `FNCReborn/patches/rexglue-sdk.patch` (FNC Reborn commit 7f68ee3, against upstream ReXGlue v0.10.0
`c94f5eb`). Target: `sdk/` of this tree (sa2-nx `baf523b`, the same v0.10.0 base plus the sa2-nx/nfsmw-nx/masseffect-nx
changes). FNC Reborn is Apache-2.0 (its NOTICE is kept in `NOTICE.fnc-reborn`).

Method: the patch was split per file. Each file was dry-run with `git apply --check --directory=sdk` (and `-R` to look
for hunks that were already applied). Every hunk was then checked for a fork change that does the same thing somewhere
else. "Applies" alone does not prove the fork lacks the fix, so the third column gives the evidence for each one.

| # | File | Result | Evidence |
|---|---|---|---|
| 01 | include/rex/graphics/command_processor.h | ported | `ReadbackResolveMode` in the fork has no `kSmart` (clean apply, the enum is untouched) |
| 02 | include/rex/graphics/d3d12/command_processor.h | ported (PC only) | No readback tracker in the fork. D3D12 is the PC dev bench only |
| 03 | include/rex/graphics/flags.h | ported | Cvars `readback_resolve_smart_*` absent |
| 04 | include/rex/ppc/context.h | ported | `simde/x86/fma.h` not included by the fork. It is needed by 07 |
| 05 | include/rex/ppc/function.h | ported | The fork's `GetIntegerArgumentValue` still returns `ctx.rN.u32` (u64 XUID truncation, FNC save enumeration) |
| 06 | include/rex/thread.h | ported | No `ShortSleepUs` / `BackoffYield` in the fork |
| 07 | src/codegen/builders/vector.cpp | ported | The fork's `build_vmaddfp` is still `add(mul())` (vector.cpp:93-97), `build_vexptefp` has no clamp, and `build_vpkuhus`/`build_vpkuwus` still use the aliasing per-element loop. **Switch note:** fused vmaddfp emits vector `fmla`, which `tools/check_switch_build.py` flags unless it is listed. Stage 3 must teach that check that VMX `vmaddfp`/`vnmsubfp` are fused on purpose (Xenon is fused, and FNC's characters vanish without it) |
| 08 | src/core/threading_posix.cpp | ported | Also used by the Switch build (`sdk/src/core/CMakeLists.txt:72`), so BackoffYield exists on Horizon too |
| 09 | src/core/threading_win.cpp | ported | — |
| 10 | src/core/timer_queue.cpp | **dropped, the fork covers it** | The fork replaced the disruptorplus spin-wait queue entirely (`timer_queue.cpp:30`, "This used to be a disruptorplus ring buffer with a spin-wait strategy"), and its wait is already capped at now()+1 h (`:75-77`). That is FNC's two fixes (spinning, and the `max()` overflow) |
| 11 | src/graphics/CMakeLists.txt | ported | Adds `fnc_swap_counter.cpp` |
| 12 | src/graphics/command_processor.cpp | ported, one hunk merged | The `CallInThread` wake was already in the fork, but only under `REX_PLATFORM_SWITCH` (`command_processor.cpp:165-168`). It is now unconditional and the Switch copy is removed. Ring capacity +4, unknown-opcode skip, BackoffYield in WAIT_REG_MEM: not in the fork (the old lines were still there) |
| 13 | src/graphics/d3d12/command_processor.cpp | ported (PC only) | — |
| 14 | src/graphics/fnc_swap_counter.cpp | ported | New file. The dllexport is `_WIN32` only |
| 15 | src/graphics/pipeline/render_target/cache.cpp | ported | `fnc_rt_no_edram_wrap` absent. It only matters for the emulated GPU (PC bench) |
| 16 | src/input/input_system.cpp | ported, hand-checked (fuzz 1) | The fork rewrote the file (read hook, device assignment), so the context moved 6-196 lines. Every hunk landed in the same function and branch as in upstream (reviewed in `git diff`) |
| 17 | src/kernel/CMakeLists.txt | ported | usbcam was still commented out in the fork (`:53`) |
| 18 | src/kernel/xam/xam_input.cpp | ported | Back-press counter for player_camera.cpp |
| 19 | src/kernel/xboxkrnl/xboxkrnl_crypt.cpp | ported | The fork's XeKeys stubs still return 0 |
| 20 | src/kernel/xboxkrnl/xboxkrnl_threading.cpp | ported | — |
| 21 | src/system/kernel_state.cpp | ported (fuzz 2-3, include order only) | The `RegisterNotifyListener` body is the same as upstream (`kernel_state.cpp:1104` before the patch) |
| 22 | src/system/xmemory.cpp | ported | The fork's SystemHeapAlloc still says "TODO(benvanik): lightweight pool" (`xmemory.cpp:845`) |
| 23 | src/system/xthread.cpp | ported | — |
| 24 | src/ui/rex_app.cpp | ported | The fork's `LoadGpuPlugin(name, backend)` already takes the backend (`gpu_plugin.h:53`). Only the cvar was missing |
| 25 | thirdparty/disruptorplus/.../blocking_wait_strategy.hpp | **dropped, the fork covers it** | Only FNC's timer queue (10) used the blocking strategy, and the fork's queue does not use disruptorplus at all (the only references are the comment at `timer_queue.cpp:30` and the CMake link list) |
