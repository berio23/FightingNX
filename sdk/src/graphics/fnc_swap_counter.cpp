// FNC Reborn: number of frames the GPU has presented. The game exe's F1 frame-rate
// counter reads it through GetProcAddress, so the exe needs no link dependency on
// the GPU plugin.

#include <atomic>
#include <cstdint>

namespace rex::graphics::fncperf {
std::atomic<uint64_t> g_swaps{0};
}  // namespace rex::graphics::fncperf

#ifdef _WIN32
extern "C" __declspec(dllexport) uint64_t fnc_gpu_swap_count() {
  return rex::graphics::fncperf::g_swaps.load(std::memory_order_relaxed);
}
#endif
