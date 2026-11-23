// Stage 1: the native renderer (src/me) is not built, but the video capture (sa2_capture.cpp) asks it whether it is
// presenting. These answer "no", so the capture reads the SDK presenter's guest output.
#include <cstdint>

namespace me::native {
bool Enabled() { return false; }
uint64_t PresentedSwaps() { return 0; }
void SetPresentHook(void (*)(uint64_t)) {}
}  // namespace me::native

// Stage 2: Renut's frame pacing (renut_engine/gpu/sa2_renut_pacing.cpp) reads the relaxed-vsync switch, which sa2-nx
// defines in src/me/me_native_system.cpp (option A's PM4 renderer: SA2 addresses, not built for FNC). The same
// definition, with its key sa2_vsync_relaxed.
#include "cpu/sa2_enhancements.h"
SA2_ENHANCEMENT(g_vsync_relaxed, sa2_vsync_relaxed, sa2::enh::kNonCpu, true,
                "Relaxed vsync: the guest's vblank follows its swaps (one per swap, at most one per 1/60 s; the regular "
                "1/60 s ticks while it does not swap), so a frame that misses 60 fps is shown when it is ready instead "
                "of at the next vblank: no fall from 60 straight to 30");
