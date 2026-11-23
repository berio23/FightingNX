// Game-specific function overrides for Fight Night Champion (default.xex v0.0.0.3,
// media 2DC50868). Addresses are only valid for that executable.
//
// Recompiled functions are weak symbols; defining one here replaces it, and
// __imp__<name> is always the original.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

// fnc-nx (2026-11-21): handheld, Nintendo's official 460.8 MHz GPU profile (FncApp::OnPreSetup).
REXCVAR_DEFINE_BOOL(fnc_gpu_boost, true, "Game",
                    "Handheld Switch: ask for the official 460.8 MHz GPU profile (memory clock unchanged, checked; "
                    "nothing when docked)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnc_ram_1600, true, "Game",
                    "Handheld Switch, with fnc_gpu_boost: prefer Nintendo's official GPU 460.8 + memory 1600 MHz profile "
                    "(0x92220007; more memory bandwidth, a little more power) over the memory-unchanged one")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(fnc_frame_rate, 60.0, "Game",
                      "Frame rate cap. The retail game runs at 30. Set 30 to restore it.");
REXCVAR_DEFINE_DOUBLE(fnc_simulation_rate, 60.0, "Game",
                      "Fight simulation ticks per second. The retail rate is 60.")
    .range(30.0, 60.0)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ---------------------------------------------------------------------------
// 60 FPS.
// sub_827C6380 builds one of the game's fixed-rate frame timers; f1 = rate in Hz,
// which sub_827C5E60 turns into a tick period (timebase / rate). The game makes
// exactly two, each once:
//   sub_822A9570 -> 30 Hz render timer (constant 0x82008430, mode 1)
//   sub_822AA558 -> 60 Hz simulation timer (constant 0x82008454, mode 0)
// The community "Unlock FPS" patch for TU2 (illusion, xenia-canary/game-patches)
// raises the same 30.0 constant (it sits at 0x82008574 in TU2).
// ---------------------------------------------------------------------------
//
// fnc-nx (2026-11-21): the caller is told by a flag set around sub_822A9570, not by ctx.lr == 0x822A9594 as in FNC
// Reborn: the optimised codegen flavour (elide_lr_stores) no longer stores the return address in ctx.lr. Same
// selection: sub_822A9570 is the only function whose call to sub_827C6380 returns to 0x822A9594, and it makes only
// that one call (it is the 30 Hz timer's constructor).
static thread_local bool g_fnc_in_30hz_timer = false;
extern "C" void __imp__sub_822A9570(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_822A9570(PPCContext& __restrict ctx, uint8_t* base) {
  g_fnc_in_30hz_timer = true;
  __imp__sub_822A9570(ctx, base);
  g_fnc_in_30hz_timer = false;
}
extern "C" void __imp__sub_827C6380(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void sub_827C6380(PPCContext& __restrict ctx, uint8_t* base) {
  if (g_fnc_in_30hz_timer && ctx.f1.f64 == 30.0) {
    double rate = REXCVAR_GET(fnc_frame_rate);
    if (rate >= 10.0 && rate <= 240.0 && rate != 30.0) {
      REXLOG_INFO("[FNC-60FPS] frame timer 30 Hz -> {:.0f} Hz (fnc_frame_rate)", rate);
      ctx.f1.f64 = rate;
    } else {
      REXLOG_INFO("[FNC-60FPS] frame timer left at 30 Hz (fnc_frame_rate = {})", rate);
    }
  } else if (!g_fnc_in_30hz_timer && ctx.r5.u32 == 0 && ctx.f1.f64 == 60.0) {
    // The fixed simulation timer supplies the tick duration to FGGameSim.
    // Apply the rate at construction so the scheduler and its tick duration agree.
    const double rate = REXCVAR_GET(fnc_simulation_rate);
    if (rate >= 30.0 && rate < 60.0) {
      REXLOG_INFO("[FNC timing] simulation timer 60 Hz -> {:.0f} Hz (fnc_simulation_rate)", rate);
      ctx.f1.f64 = rate;
    }
  }
  __imp__sub_827C6380(ctx, base);
}
