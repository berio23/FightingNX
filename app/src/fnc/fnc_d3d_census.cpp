// Stage 2 diagnostic: the Direct3D census of FNC (see fnc_d3d_census.h). PC bench only; off unless fnc_d3d_census.

#include "fnc_d3d_census.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

REXCVAR_DEFINE_BOOL(fnc_d3d_census, false, "FNC",
                    "Stage 2: count every Direct3D-range function's calls and callers (census builds only; "
                    "a report in the log every 10 s)");

namespace fnc::census {
namespace {

constexpr uint32_t kSwap = 0x82CEB9A8;  // D3DDevice_Swap (docs/fnc/stage2.md): the frame count
constexpr uint32_t kD3DLow = 0x82C80000, kD3DHigh = 0x82D10000;  // the census range (tools/fnc_d3d_census_build.py)
constexpr int kCallers = 6;

struct Entry {
  uint32_t address = 0;
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> reported{0};
  std::mutex mutex;
  uint32_t callers[kCallers] = {};
  uint64_t caller_calls[kCallers] = {};
  // Calls where r3..r6 pointed at a word of the shader-container magic family 0x102A11xx (first kMagicChecks calls):
  // the shader creation / registration functions get the container.
  std::atomic<uint32_t> magic_checks{0};
  std::atomic<uint64_t> magic[4] = {};
  // Calls where r3..r6 pointed at a D3DVERTEXELEMENT9 array (CreateVertexDeclaration's argument), same calls.
  std::atomic<uint64_t> elements[4] = {};
};
constexpr uint32_t kMagicChecks = 256;

// The guest word at `va`, if its host page is committed (a register can hold any number; an unmapped guest page
// would fault). Windows only; the census is a PC bench diagnostic.
bool ReadGuestWord(const uint8_t* base, uint32_t va, uint32_t& out) {
#if defined(_WIN32)
  if (va < 0x10000 || (va & 3)) return false;
  const uint8_t* p = base + va;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
      PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY)))
    return false;
  out = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
  return true;
#else
  (void)base, (void)va, (void)out;
  return false;
#endif
}

// True when `va` points at an Xbox 360 D3DVERTEXELEMENT9 array. Unlike the PC's 8-byte element, the 360's Type is a
// DWORD: WORD Stream, WORD Offset, DWORD Type, BYTE Method, Usage, UsageIndex, padded to 12 bytes. At least one
// element with a stream below 16, ending with D3DDECL_END (0x00FF0000, D3DDECLTYPE_UNUSED 0xFFFFFFFF, 0) within 32.
bool LooksLikeVertexElements(const uint8_t* base, uint32_t va) {
  constexpr uint32_t kStride = 12;
  for (uint32_t i = 0; i < 32; ++i) {
    uint32_t w0, w1, w2;
    if (!ReadGuestWord(base, va + kStride * i, w0) || !ReadGuestWord(base, va + kStride * i + 4, w1) ||
        !ReadGuestWord(base, va + kStride * i + 8, w2))
      return false;
    if (w0 == 0x00FF0000u && w1 == 0xFFFFFFFFu) return i > 0 && (w2 >> 8) == 0;
    if ((w0 >> 16) >= 16 || w1 == 0 || w1 == 0xFFFFFFFFu) return false;
  }
  return false;
}

std::unique_ptr<Entry[]> g_entries;
int g_count = 0;
int g_swap = -1;
std::once_flag g_reporter_once;

void Report() {
  uint64_t last_swaps = 0;
  while (true) {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const uint64_t swaps_now = g_swap >= 0 ? g_entries[g_swap].calls.load() : 0;
    const uint64_t swaps = swaps_now - last_swaps;
    last_swaps = swaps_now;
    std::vector<std::pair<uint64_t, int>> active;
    for (int i = 0; i < g_count; ++i) {
      const uint64_t now = g_entries[i].calls.load();
      const uint64_t delta = now - g_entries[i].reported.exchange(now);
      if (delta) active.emplace_back(delta, i);
    }
    std::sort(active.begin(), active.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    REXLOG_INFO("[fnc-census] window: {} swaps, {} functions called", swaps, active.size());
    for (const auto& [delta, i] : active) {
      Entry& e = g_entries[i];
      std::string callers;
      {
        std::lock_guard<std::mutex> lock(e.mutex);
        for (int k = 0; k < kCallers && e.callers[k]; ++k) {
          const bool d3d = e.callers[k] >= kD3DLow && e.callers[k] < kD3DHigh;
          callers += fmt::format(" {}{:08X}x{}", d3d ? "" : "game:", e.callers[k], e.caller_calls[k]);
        }
      }
      std::string magic, elements;
      for (int r = 0; r < 4; ++r) {
        if (const uint64_t m = e.magic[r].load()) magic += fmt::format(" r{}x{}", r + 3, m);
        if (const uint64_t m = e.elements[r].load()) elements += fmt::format(" r{}x{}", r + 3, m);
      }
      REXLOG_INFO("[fnc-census] sub_{:08X}: {} calls ({:.2f} per swap), total {}; callers:{}{}{}{}{}", e.address,
                  delta, swaps ? double(delta) / double(swaps) : 0.0, e.calls.load(), callers,
                  magic.empty() ? "" : "; shader container (0x102A11xx) in:", magic,
                  elements.empty() ? "" : "; vertex elements in:", elements);
    }
  }
}

}  // namespace

void Register(const uint32_t* addresses, int count) {
  g_entries = std::make_unique<Entry[]>(count);
  g_count = count;
  for (int i = 0; i < count; ++i) {
    g_entries[i].address = addresses[i];
    if (addresses[i] == kSwap) g_swap = i;
  }
}

void Note(int index, PPCContext& ctx, const uint8_t* base) {
  static const bool on = REXCVAR_GET(fnc_d3d_census);
  if (!on || index >= g_count) return;
  // One reporter thread for the life of the process (PC bench only: never in a Switch build, where a detached thread
  // ends the program, docs/platform-notes.md).
  std::call_once(g_reporter_once, [] { std::thread(Report).detach(); });
  Entry& e = g_entries[index];
  e.calls.fetch_add(1, std::memory_order_relaxed);
  if (e.magic_checks.fetch_add(1, std::memory_order_relaxed) < kMagicChecks) {
    const uint32_t regs[4] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32};
    for (int r = 0; r < 4; ++r) {
      uint32_t word;
      if (ReadGuestWord(base, regs[r], word) && (word & 0xFFFFFF00u) == 0x102A1100u)
        e.magic[r].fetch_add(1, std::memory_order_relaxed);
      if (LooksLikeVertexElements(base, regs[r])) e.elements[r].fetch_add(1, std::memory_order_relaxed);
    }
  }
  const uint32_t lr = uint32_t(ctx.lr);
  std::lock_guard<std::mutex> lock(e.mutex);
  for (int k = 0; k < kCallers; ++k) {
    if (e.callers[k] == lr) {
      ++e.caller_calls[k];
      return;
    }
    if (!e.callers[k]) {
      e.callers[k] = lr;
      e.caller_calls[k] = 1;
      return;
    }
  }
}

}  // namespace fnc::census
