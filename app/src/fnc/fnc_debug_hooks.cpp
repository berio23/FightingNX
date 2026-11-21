// Temporary boot-debugging hooks for FNC: log who calls a function and with which arguments, then call the original.
// Each entry says what it is chasing; remove it once the question is answered. Empty while nothing needs one.
//
// Pattern (sa2-nx's sa2_debug_hooks.cpp):
//   extern "C" REX_FUNC(__imp__sub_XXXXXXXX);
//   REX_HOOK_RAW(sub_XXXXXXXX) {
//     REXLOG_WARN("[dbg] sub_XXXXXXXX(r3={:08X}) from lr={:08X}", ctx.r3.u32, uint32_t(ctx.lr));
//     __imp__sub_XXXXXXXX(ctx, base);
//   }
//
// History: the first Direct3D census (2026-11-20, ten hand-written hooks on the draw candidates) lived here; it is
// now the generated census build (fnc_d3d_census.cpp, tools/fnc_d3d_census_build.py). Results: docs/fnc/stage2.md.

#include <rex/hook.h>
#include <rex/logging.h>
