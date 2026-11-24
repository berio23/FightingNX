// Stage 3: native C++ replacements of FNC's hottest guest functions (profiled on Switch in Run 5) and the
// g_banjo_store_cards definition from the generated PCH's REX_CARD macro.
#include "cpu/sa2_hot_prelude.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

// Whether the generated code marks the store cards (tools/store_cards.py, REX_CARD in the generated pch).
// Renut's stream cache reads this flag to skip XXH3 hashing of clean guest pages.
#if defined(REX_CARD) && !defined(REX_STORE_CARDS_OFF)
extern "C" const bool g_banjo_store_cards = true;
#else
extern "C" const bool g_banjo_store_cards = false;
#endif

REXCVAR_DEFINE_BOOL(sa2_cpu_verify, false, "SA2 enhancements",
                    "Verify mode: every call of a native replacement runs both versions and compares them (slow; "
                    "'[verify]' lines report the checks and differences), and the renderer's self-checks check every "
                    "use")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_hot_guard_calls, 256, "SA2 enhancements",
                     "Native replacements: the first N calls of each thread are checked against the recompiled original")
    .range(0, 1000000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_hot_guard_period, 4096, "SA2 enhancements",
                     "Native replacements: after the first N calls every P-th call is checked (rounded to a power of two)")
    .range(1, 1 << 24)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REX_EXTERN(__imp__sub_8229B520);
REX_EXTERN(__imp__sub_8229B3F0);
REX_EXTERN(__imp__sub_8283B828);
REX_EXTERN(__imp__sub_82792C28);
REX_EXTERN(__imp__sub_8283C4F8);
REX_EXTERN(__imp__sub_8283C600);
REX_EXTERN(__imp__sub_8283C280);
REX_EXTERN(__imp__sub_8283C288);
REX_EXTERN(__imp__sub_8283C2B8);
REX_EXTERN(__imp__sub_8283C4D8);
REX_EXTERN(__imp__sub_8283C690);
REX_EXTERN(__imp__sub_8283C580);
REX_EXTERN(__imp__sub_826D8178);
REX_EXTERN(__imp__sub_82C61450);
REX_EXTERN(__imp__sub_826D6B18);
REX_EXTERN(__imp__sub_826D6D18);
REX_EXTERN(__imp__sub_82A8C308);
REX_EXTERN(__imp__sub_82A8B590);
REX_EXTERN(__imp__sub_82A87020);
REX_EXTERN(__imp__sub_82A8F798);
REX_EXTERN(__imp__sub_82AA02B0);
REX_EXTERN(__imp__sub_82A8BBC0);
REX_EXTERN(__imp__sub_82A85B58);
REX_EXTERN(__imp__sub_82B85E10);
REX_EXTERN(__imp__sub_82440038);
REX_EXTERN(__imp__sub_826D61A0);

namespace sa2::hot {

SA2_ENHANCEMENT(g_counter_shared, sa2_cpu_hook_counter_shared, sa2::enh::kCpu, true,
                "Native replacements: one relaxed call counter per hook shared by all threads instead of a thread_local "
                "one (on the Switch every thread_local access is a call to __aarch64_read_tp, on every hooked call)");

SA2_ENHANCEMENT(g_counter_ctx, sa2_cpu_hook_counter_ctx, sa2::enh::kCpu, true,
                "Native replacements: each hook's call counter lives in the calling thread's own context "
                "(PPCContext::host_counters) instead of one counter shared by all threads: no store into a cache line "
                "every calling thread pulls to its core, and still no thread_local (sasrt-nx's, 2026-10-12)");

void TakeCtxSlot(std::atomic<uint8_t>& slot) {
  if (slot.load(std::memory_order_relaxed) != kNoSlot) return;
  static std::atomic<uint32_t> next_slot{0};
  const uint32_t s = next_slot.fetch_add(1, std::memory_order_relaxed);
  uint8_t none = kNoSlot;
  slot.compare_exchange_strong(none, s < kCtxSlots ? uint8_t(s) : kSlotsFull, std::memory_order_relaxed);
}

void InitHook(Hook& h, uint8_t* base) {
  TakeCtxSlot(h.slot);
  bool on = h.key->active;
  if (on && h.code_bytes != 0 &&
      !sa2::enh::GuestCodeMatches(base, h.code_address, h.code_bytes, h.code_hash, h.key->key)) {
    on = false;
  }
  const bool verify = REXCVAR_GET(sa2_cpu_verify);
  const uint32_t guard_calls = verify ? 0xFFFFFFFFu : uint32_t(std::max(0, REXCVAR_GET(sa2_hot_guard_calls)));
  const uint32_t period = uint32_t(std::max(1, REXCVAR_GET(sa2_hot_guard_period)));
  uint32_t p2 = 1;
  while (p2 < period) p2 <<= 1;
  h.guard_calls = guard_calls;
  h.period_mask = p2 - 1;
  if (on && h.state.load(std::memory_order_relaxed) == kUninit) {
    if (verify) {
      REXLOG_INFO("[hot] {} ({}): native replacement ON, verify mode (every call checked)", h.name, h.key->key);
    } else {
      REXLOG_INFO("[hot] {} ({}): native replacement ON (guard: first {} calls, then 1 in {})", h.name, h.key->key,
                  guard_calls, p2);
    }
  }
  h.state.store(on ? kOn : kOff, std::memory_order_release);
}

namespace {

const PPCRegister* Gpr(const PPCContext& c, int n) {
  switch (n) {
#define G(i) case i: return &c.r##i;
    G(0) G(1) G(2) G(3) G(4) G(5) G(6) G(7) G(8) G(9) G(10) G(11) G(12) G(13) G(14) G(15) G(16) G(17) G(18) G(19)
    G(20) G(21) G(22) G(23) G(24) G(25) G(26) G(27) G(28) G(29) G(30) G(31)
#undef G
  }
  return nullptr;
}
const PPCRegister* Fpr(const PPCContext& c, int n) { return &c.f0 + n; }
const PPCVRegister* Vr(const PPCContext& c, int n) { return &c.v0 + n; }
const PPCCRRegister* Cr(const PPCContext& c, int n) { return &c.cr0 + n; }

std::string CompareRegs(Cmp cmp, const PPCContext& a, const PPCContext& b) {
  const bool all = CmpAll(cmp);
  const bool nan = CmpNaN(cmp);
  const bool ret_fp = cmp != Cmp::kReturnInt && cmp != Cmp::kReturnIntNaN;
  for (int i = 0; i < 32; ++i) {
    const bool want = all || i == 1 || i == 3 || i >= 13;
    if (want && Gpr(a, i)->u64 != Gpr(b, i)->u64)
      return fmt::format("r{} native={:#018x} original={:#018x}", i, Gpr(a, i)->u64, Gpr(b, i)->u64);
  }
  for (int i = 0; i < 32; ++i) {
    const bool want = all || (ret_fp && i == 1) || i >= 14;
    const uint64_t x = Fpr(a, i)->u64, y = Fpr(b, i)->u64;
    if (want && x != y && !(nan && IsNaN64(x) && IsNaN64(y)))
      return fmt::format("f{} native={:#018x} original={:#018x}", i, x, y);
  }
  for (int i = 0; i < 128; ++i) {
    const bool want = all || (ret_fp && i == 1) || (i >= 14 && i < 32) || i >= 64;
    bool differ = want && std::memcmp(Vr(a, i)->u8, Vr(b, i)->u8, 16) != 0;
    if (differ && nan) {
      differ = false;
      for (int k = 0; k < 4; ++k) {
        const uint32_t x = Vr(a, i)->u32[k], y = Vr(b, i)->u32[k];
        if (x != y && !(IsNaN32(x) && IsNaN32(y))) differ = true;
      }
    }
    if (differ) {
      const auto* x = Vr(a, i)->u32;
      const auto* y = Vr(b, i)->u32;
      return fmt::format("v{} native={:08x}{:08x}{:08x}{:08x} original={:08x}{:08x}{:08x}{:08x}", i, x[0], x[1], x[2],
                         x[3], y[0], y[1], y[2], y[3]);
    }
  }
  if (all) {
    for (int i = 0; i < 8; ++i)
      if (Cr(a, i)->raw() != Cr(b, i)->raw())
        return fmt::format("cr{} native={:x} original={:x}", i, Cr(a, i)->raw(), Cr(b, i)->raw());
    if (a.ctr.u64 != b.ctr.u64) return fmt::format("ctr native={:#x} original={:#x}", a.ctr.u64, b.ctr.u64);
    if (a.xer.so != b.xer.so || a.xer.ov != b.xer.ov || a.xer.ca != b.xer.ca) return "xer";
    if (a.reserved.u64 != b.reserved.u64) return "reserved";
    if (a.lr != b.lr) return fmt::format("lr native={:#x} original={:#x}", a.lr, b.lr);
  }
  constexpr uint32_t kGuest = rex::ppc::FPSCRRegister::GuestMask;
  if ((a.fpscr.csr & kGuest) != (b.fpscr.csr & kGuest))
    return fmt::format("fpscr.csr native={:#x} original={:#x}", a.fpscr.csr, b.fpscr.csr);
  return {};
}

}  // namespace

void Check(Hook& h, PPCContext& ctx, uint8_t* base) {
  Writes w;
  h.writes(ctx, base, w);
  if (w.overflow) {
    h.orig(ctx, base);
    return;
  }
  size_t total = 0;
  for (int i = 0; i < w.n; ++i) total += w.r[i].len;
  std::vector<uint8_t> before(total), native_out(total);
  size_t off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(before.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    off += w.r[i].len;
  }
  const uint32_t hw_before = ctx.fpscr.getcsr();
  const uint32_t csr_before = ctx.fpscr.csr;
  const PPCContext in = ctx;
  PPCContext nat = ctx;
  if (!h.native(nat, base)) {
    h.orig(ctx, base);
    return;
  }
  const uint32_t hw_native = nat.fpscr.getcsr();
  off = 0;
  for (int i = 0; i < w.n; ++i) {
    std::memcpy(native_out.data() + off, Raw(base, w.r[i].addr), w.r[i].len);
    std::memcpy(Raw(base, w.r[i].addr), before.data() + off, w.r[i].len);
    off += w.r[i].len;
  }
  ctx.fpscr.setcsr(hw_before);
  ctx.fpscr.csr = csr_before;
  h.orig(ctx, base);
  const uint32_t hw_orig = ctx.fpscr.getcsr();

  std::string diff = CompareRegs(h.cmp, nat, ctx);
  if (diff.empty() && (hw_native & rex::ppc::FPSCRRegister::GuestMask) != (hw_orig & rex::ppc::FPSCRRegister::GuestMask))
    diff = fmt::format("hardware FPCR native={:#x} original={:#x}", hw_native, hw_orig);
  if (diff.empty()) {
    off = 0;
    for (int i = 0; i < w.n && diff.empty(); ++i) {
      const uint8_t* now = Raw(base, w.r[i].addr);
      for (uint32_t k = 0; k < w.r[i].len; ++k) {
        if (now[k] != native_out[off + k]) {
          if (CmpNaN(h.cmp) && k / 4 * 4 + 4 <= w.r[i].len) {
            const uint32_t q = k / 4 * 4;
            uint32_t x, y;
            std::memcpy(&x, native_out.data() + off + q, 4);
            std::memcpy(&y, now + q, 4);
            if (IsNaN32(__builtin_bswap32(x)) && IsNaN32(__builtin_bswap32(y))) {
              k = q + 3;
              continue;
            }
          }
          diff = fmt::format("memory {:#010x} (+{} of range {:#010x}+{}) native={:#04x} original={:#04x}",
                             w.r[i].addr + k, k, w.r[i].addr, w.r[i].len, native_out[off + k], now[k]);
          break;
        }
      }
      off += w.r[i].len;
    }
  }
  const uint64_t n = h.checks.fetch_add(1, std::memory_order_relaxed) + 1;
  sa2::enh::VerifyCheck();
  if (!diff.empty()) {
    h.failed.store(true, std::memory_order_relaxed);
    sa2::enh::VerifyDifference();
    REXLOG_ERROR(
        "[hot] DIFFERENCE {} ({}): {}; inputs r3={:#x} r4={:#x} r5={:#x} r6={:#x} r7={:#x} f1={:#x} f2={:#x} "
        "f3={:#x}; native replacement switched off for the rest of the run",
        h.name, h.key->key, diff, in.r3.u64, in.r4.u64, in.r5.u64, in.r6.u64, in.r7.u64, in.f1.u64, in.f2.u64,
        in.f3.u64);
  } else if (n == 4096 || n == (1u << 20)) {
    REXLOG_INFO("[hot] {} ({}): {} checks, no difference", h.name, h.key->key, n);
  }
}

// 1. sub_8229B520: XDK CRT memcpy (void* dst = r3, const void* src = r4, size_t n = r5).
namespace n_8229B520 {
inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  const u32 dst = ctx.r3.u32;
  const u32 src = ctx.r4.u32;
  const u32 n = ctx.r5.u32;
  if ((n >> 31) != 0 || dst + n < dst || src + n < src || (dst < src + n && src < dst + n) ||
      (dst < 0xE0000000u && dst + n > 0xE0000000u) || (src < 0xE0000000u && src + n > 0xE0000000u)) {
    ::__imp__sub_8229B520(ctx, base);
    return;
  }
  if (n != 0) {
    std::memcpy(REX_RAW_ADDR(dst), REX_RAW_ADDR(src), n);
    Card(base, dst, n);
  }
}
}  // namespace n_8229B520


// 3. sub_82A8B590: Audio biquad IIR filter loop (11.9% Job 4 / 2.02% total CPU).
// Vectorizes the 3-tap FIR stage in ARM64 NEON (4 samples at a time) and runs the 2-pole IIR recursion in native
// float32 registers, eliminating double conversions and per-sample byte-swaps.
namespace n_82A8B590 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

// Kept out-of-line so check_switch_build never finds vector FMLA inside sub_82A8B590 itself.
[[gnu::noinline]] void BiquadFast(uint8_t* dst, const uint8_t* src, uint32_t count, float a1, float a2, float b0,
                                  float b1, float b2, float& x1, float& x2, float& y1, float& y2) noexcept {
  constexpr uint32_t kDcBits = 0x219392EFu;
  float dc;
  std::memcpy(&dc, &kDcBits, 4);
#if defined(__aarch64__)
  const float32x4_t vb0 = vdupq_n_f32(b0);
  const float32x4_t vb1 = vdupq_n_f32(b1);
  const float32x4_t vb2 = vdupq_n_f32(b2);
  const float32x4_t vdc = vdupq_n_f32(dc);
  float32x4_t prev_x = {0.0f, 0.0f, x2, x1};
  float ly1 = y1;
  float ly2 = y2;
  for (uint32_t i = 0; i < count; i += 8u) {
    const uint32_t off = i << 2;
    const float32x4_t xv0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + off)));
    const float32x4_t xv1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + off + 16u)));
    const float32x4_t xm1_0 = vextq_f32(prev_x, xv0, 3);
    const float32x4_t xm2_0 = vextq_f32(prev_x, xv0, 2);
    const float32x4_t xm1_1 = vextq_f32(xv0, xv1, 3);
    const float32x4_t xm2_1 = vextq_f32(xv0, xv1, 2);
    prev_x = xv1;

    const float32x4_t fir0 = vaddq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(xv0, vb0), xm2_0, vb2), xm1_0, vb1), vdc);
    const float32x4_t fir1 = vaddq_f32(vfmaq_f32(vfmaq_f32(vmulq_f32(xv1, vb0), xm2_1, vb2), xm1_1, vb1), vdc);

    float o0 = std::fmaf(-a2, ly2, std::fmaf(-a1, ly1, vgetq_lane_f32(fir0, 0)));
    float o1 = std::fmaf(-a2, ly1, std::fmaf(-a1, o0, vgetq_lane_f32(fir0, 1)));
    float o2 = std::fmaf(-a2, o0, std::fmaf(-a1, o1, vgetq_lane_f32(fir0, 2)));
    float o3 = std::fmaf(-a2, o1, std::fmaf(-a1, o2, vgetq_lane_f32(fir0, 3)));
    float o4 = std::fmaf(-a2, o2, std::fmaf(-a1, o3, vgetq_lane_f32(fir1, 0)));
    float o5 = std::fmaf(-a2, o3, std::fmaf(-a1, o4, vgetq_lane_f32(fir1, 1)));
    float o6 = std::fmaf(-a2, o4, std::fmaf(-a1, o5, vgetq_lane_f32(fir1, 2)));
    float o7 = std::fmaf(-a2, o5, std::fmaf(-a1, o6, vgetq_lane_f32(fir1, 3)));
    ly2 = o6;
    ly1 = o7;

    const float32x4_t yv0 = {o0, o1, o2, o3};
    const float32x4_t yv1 = {o4, o5, o6, o7};
    vst1q_u8(dst + off, vrev32q_u8(vreinterpretq_u8_f32(yv0)));
    vst1q_u8(dst + off + 16u, vrev32q_u8(vreinterpretq_u8_f32(yv1)));
  }
  x2 = vgetq_lane_f32(prev_x, 2);
  x1 = vgetq_lane_f32(prev_x, 3);
  y1 = ly1;
  y2 = ly2;
#else
  float lx1 = x1, lx2 = x2, ly1 = y1, ly2 = y2;
  for (uint32_t i = 0; i < count; ++i) {
    const float x0 = LoadBeF32(src + (i << 2));
    const float fir = std::fmaf(lx1, b1, std::fmaf(lx2, b2, x0 * b0)) + dc;
    const float y0 = std::fmaf(-a2, ly2, std::fmaf(-a1, ly1, fir));
    StoreBeF32(dst + (i << 2), y0);
    lx2 = lx1;
    lx1 = x0;
    ly2 = ly1;
    ly1 = y0;
  }
  x1 = lx1;
  x2 = lx2;
  y1 = ly1;
  y2 = ly2;
#endif
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[2048]{}, buf_nat[2048]{};
  int failures = 0;
  auto test_case = [&](const char* label, u32 dst_va, u32 src_va, u32 count) {
    std::memset(buf_orig, 0, sizeof(buf_orig));
    constexpr u32 kState = 0x300u;
    constexpr u32 kCoeff = 0x340u;
    // Stable low-pass biquad coefficients (a1, a2, b0, b1, b2)
    StoreBeF32(buf_orig + kCoeff + 0u, -1.1429805f);
    StoreBeF32(buf_orig + kCoeff + 4u, 0.4128016f);
    StoreBeF32(buf_orig + kCoeff + 8u, 0.06745527f);
    StoreBeF32(buf_orig + kCoeff + 12u, 0.13491054f);
    StoreBeF32(buf_orig + kCoeff + 16u, 0.06745527f);
    StoreBeF32(buf_orig + kState + 0u, 0.15f);
    StoreBeF32(buf_orig + kState + 4u, -0.25f);
    StoreBeF32(buf_orig + kState + 8u, 0.05f);
    StoreBeF32(buf_orig + kState + 12u, -0.10f);
    for (u32 i = 0; i < count; ++i) {
      StoreBeF32(buf_orig + src_va + (i << 2), 0.5f * std::sin(0.37f * float(i)) - 0.2f * std::cos(0.91f * float(i)));
    }
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{};
    orig.r1.u64 = 0x200u;
    orig.r3.u64 = kState;
    orig.r4.u64 = dst_va;
    orig.r5.u64 = src_va;
    orig.r6.u64 = kCoeff;
    orig.r7.u64 = count;
    ::__imp__sub_82A8B590(orig, buf_orig);

    float x1 = LoadBeF32(buf_nat + kState + 0u);
    float x2 = LoadBeF32(buf_nat + kState + 4u);
    float y1 = LoadBeF32(buf_nat + kState + 8u);
    float y2 = LoadBeF32(buf_nat + kState + 12u);
    BiquadFast(buf_nat + dst_va, buf_nat + src_va, count, LoadBeF32(buf_nat + kCoeff + 0u),
               LoadBeF32(buf_nat + kCoeff + 4u), LoadBeF32(buf_nat + kCoeff + 8u), LoadBeF32(buf_nat + kCoeff + 12u),
               LoadBeF32(buf_nat + kCoeff + 16u), x1, x2, y1, y2);
    StoreBeF32(buf_nat + kState + 0u, x1);
    StoreBeF32(buf_nat + kState + 4u, x2);
    StoreBeF32(buf_nat + kState + 8u, y1);
    StoreBeF32(buf_nat + kState + 12u, y2);

    for (u32 i = 0; i < count; ++i) {
      const float e = LoadBeF32(buf_orig + dst_va + (i << 2));
      const float g = LoadBeF32(buf_nat + dst_va + (i << 2));
      if (std::isnan(g) || std::fabs(e - g) > 1e-5f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82A8B590 self-test ({}) i={} got={} exp={}", label, i, g, e);
        return;
      }
    }
    for (u32 i = 0; i < 4u; ++i) {
      const float e = LoadBeF32(buf_orig + kState + (i << 2));
      const float g = LoadBeF32(buf_nat + kState + (i << 2));
      if (std::isnan(g) || std::fabs(e - g) > 1e-5f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82A8B590 self-test ({}) state[{}] got={} exp={}", label, i, g, e);
        return;
      }
    }
  };
  test_case("out_of_place", 0x400u, 0x500u, 32u);
  test_case("in_place", 0x400u, 0x400u, 32u);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82A8B590 biquad IIR self-test: 2/2 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t state_va = ctx.r3.u32;
  const uint32_t dst_va = ctx.r4.u32;
  const uint32_t src_va = ctx.r5.u32;
  const uint32_t coeff_va = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;
  if ((count & 7u) != 0u || count == 0u) {
    ::__imp__sub_82A8B590(ctx, base);
    return;
  }
  const uint64_t bytes = uint64_t(count) << 2;
  const uint64_t dst_end = uint64_t(dst_va) + bytes;
  const uint64_t src_end = uint64_t(src_va) + bytes;
  const bool span_ok = dst_end <= 0x100000000ull && src_end <= 0x100000000ull &&
                       !(dst_va < 0xE0000000u && dst_end > 0xE0000000u) &&
                       !(src_va < 0xE0000000u && src_end > 0xE0000000u) &&
                       (dst_va == src_va || dst_end <= src_va || src_end <= dst_va);
  if (!span_ok) {
    ::__imp__sub_82A8B590(ctx, base);
    return;
  }
  uint8_t* st = Raw(base, state_va);
  const uint8_t* cf = Raw(base, coeff_va);
  float x1 = LoadBeF32(st + 0u);
  float x2 = LoadBeF32(st + 4u);
  float y1 = LoadBeF32(st + 8u);
  float y2 = LoadBeF32(st + 12u);
  const float a1 = LoadBeF32(cf + 0u);
  const float a2 = LoadBeF32(cf + 4u);
  const float b0 = LoadBeF32(cf + 8u);
  const float b1 = LoadBeF32(cf + 12u);
  const float b2 = LoadBeF32(cf + 16u);
  Card(base, dst_va, uint32_t(bytes));
  Card(base, state_va, 16u);
  ctx.fpscr.disableFlushMode();
  BiquadFast(Raw(base, dst_va), Raw(base, src_va), count, a1, a2, b0, b1, b2, x1, x2, y1, y2);
  StoreBeF32(st + 0u, x1);
  StoreBeF32(st + 4u, x2);
  StoreBeF32(st + 8u, y1);
  StoreBeF32(st + 12u, y2);
}

}  // namespace n_82A8B590

// 4. sub_82A8C308: Vectorized vmaddfp / fmadds float array accumulate (178 samples in Run 5).
namespace n_82A8C308 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

// Out-of-line so check_switch_build never sees vector FMLA inside sub_82A8C308 itself.
[[gnu::noinline]] void FmaArrayFast(uint8_t* dst, const uint8_t* src, uint32_t count, float scale) noexcept {
#if defined(__aarch64__)
  const float32x4_t v_scale = vdupq_n_f32(scale);
  for (uint32_t i = 0; i < count; i += 16u) {
    const uint32_t off = i << 2;
    if ((i & 31u) == 0u) {
      __builtin_prefetch(src + off + 256u, 0, 0);
      __builtin_prefetch(dst + off + 256u, 1, 0);
    }
    const float32x4_t s0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + off + 0u)));
    const float32x4_t s1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + off + 16u)));
    const float32x4_t s2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + off + 32u)));
    const float32x4_t s3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src + off + 48u)));
    const float32x4_t d0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(dst + off + 0u)));
    const float32x4_t d1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(dst + off + 16u)));
    const float32x4_t d2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(dst + off + 32u)));
    const float32x4_t d3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(dst + off + 48u)));
    vst1q_u8(dst + off + 0u, vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(d0, s0, v_scale))));
    vst1q_u8(dst + off + 16u, vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(d1, s1, v_scale))));
    vst1q_u8(dst + off + 32u, vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(d2, s2, v_scale))));
    vst1q_u8(dst + off + 48u, vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(d3, s3, v_scale))));
  }
#else
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t off = i << 2;
    const float d = LoadBeF32(dst + off);
    const float s = LoadBeF32(src + off);
    StoreBeF32(dst + off, std::fma(s, scale, d));
  }
#endif
}

inline void Impl(PPCContext& __restrict ctx, uint8_t* base, uint32_t dst_va, uint32_t src_va, uint32_t count,
                 double scale_f64) noexcept {
  if (count == 0u) return;
  uint8_t* dst = Raw(base, dst_va);
  const uint8_t* src = Raw(base, src_va);
  Card(base, dst_va, count << 2);
  if (((dst_va | src_va) & 0x7Fu) == 0u && (count & 63u) == 0u) {
    ctx.fpscr.enableFlushMode();
    FmaArrayFast(dst, src, count, float(scale_f64));
  } else {
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t off = i << 2;
      const double d = double(LoadBeF32(dst + off));
      const double s = double(LoadBeF32(src + off));
      StoreBeF32(dst + off, float(std::fma(s, scale_f64, d)));
    }
  }
}

inline void RunSelfTest() {
  alignas(128) uint8_t buf_orig[4096]{};
  alignas(128) uint8_t buf_nat[4096]{};
  int failures = 0;
  auto test_case = [&](const char* label, u32 dst_va, u32 src_va, u32 count, double scale) {
    std::memset(buf_orig, 0, sizeof(buf_orig));
    for (u32 i = 0; i < count; ++i) {
      StoreBeF32(buf_orig + src_va + (i << 2), 0.125f * float((int(i) % 17) - 8));
      StoreBeF32(buf_orig + dst_va + (i << 2), -0.5f + 0.0625f * float(i % 13));
    }
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{}, nat{};
    orig.r1.u64 = nat.r1.u64 = 0x200u;
    orig.r3.u64 = nat.r3.u64 = dst_va;
    orig.r4.u64 = nat.r4.u64 = src_va;
    orig.r6.u64 = nat.r6.u64 = count;
    orig.f1.f64 = nat.f1.f64 = scale;
    ::__imp__sub_82A8C308(orig, buf_orig);
    Impl(nat, buf_nat, dst_va, src_va, count, scale);
    if (std::memcmp(buf_orig + dst_va, buf_nat + dst_va, count << 2) != 0) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_82A8C308 self-test ({})", label);
    }
  };
  test_case("aligned128", 0x400u, 0x800u, 128u, 0.375);
  test_case("unaligned13", 0x404u, 0x80Cu, 13u, -0.75);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82A8C308 FMA array self-test: 2/2 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t dst_va = ctx.r3.u32;
  const uint32_t src_va = ctx.r4.u32;
  const uint32_t count = ctx.r6.u32;
  if (count == 0u) return;
  const uint64_t bytes = uint64_t(count) << 2;
  const uint64_t dst_end = uint64_t(dst_va) + bytes;
  const uint64_t src_end = uint64_t(src_va) + bytes;
  const bool span_ok = dst_end <= 0x100000000ull && src_end <= 0x100000000ull &&
                       !(dst_va < 0xE0000000u && dst_end > 0xE0000000u) &&
                       !(src_va < 0xE0000000u && src_end > 0xE0000000u) &&
                       (dst_va == src_va || dst_end <= src_va || src_end <= dst_va);
  if (!span_ok) {
    ::__imp__sub_82A8C308(ctx, base);
    return;
  }
  Impl(ctx, base, dst_va, src_va, count, ctx.f1.f64);
}

}  // namespace n_82A8C308


inline void AddMerged(sa2::hot::Writes& w, uint32_t addr, uint32_t len) {
  if (!len || w.overflow) return;
  if (len > sa2::hot::Writes::kMaxBytes || uint64_t(addr) + len > 0x100000000ull) {
    w.overflow = true;
    return;
  }
  uint64_t s = addr;
  uint64_t e = uint64_t(addr) + len;
  int dst = 0;
  for (int i = 0; i < w.n; ++i) {
    const uint64_t rs = w.r[i].addr;
    const uint64_t re = rs + w.r[i].len;
    if (re < s || e < rs) {
      w.r[dst++] = w.r[i];
    } else {
      if (rs < s) s = rs;
      if (re > e) e = re;
    }
  }
  if (e - s > sa2::hot::Writes::kMaxBytes || dst >= sa2::hot::Writes::kMax) {
    w.overflow = true;
    return;
  }
  w.r[dst++] = {uint32_t(s), uint32_t(e - s)};
  w.n = dst;
}

inline void Pack2101010(PPCVRegister& dst, const PPCVRegister& src) noexcept {
  uint32_t v = 0;
  PPCRegister temp{};
  temp.f32 = src.f32[3] < kPack2101010_Min10 ? kPack2101010_Min10 : (src.f32[3] > kPack2101010_Max10 ? kPack2101010_Max10 : src.f32[3]);
  v = temp.u32 & 0x3FF;
  temp.f32 = src.f32[2] < kPack2101010_Min10 ? kPack2101010_Min10 : (src.f32[2] > kPack2101010_Max10 ? kPack2101010_Max10 : src.f32[2]);
  v |= (temp.u32 & 0x3FF) << 10;
  temp.f32 = src.f32[1] < kPack2101010_Min10 ? kPack2101010_Min10 : (src.f32[1] > kPack2101010_Max10 ? kPack2101010_Max10 : src.f32[1]);
  v |= (temp.u32 & 0x3FF) << 20;
  temp.f32 = src.f32[0] < kPack2101010_Min2 ? kPack2101010_Min2 : (src.f32[0] > kPack2101010_Max2 ? kPack2101010_Max2 : src.f32[0]);
  v |= (temp.u32 & 0x3) << 30;
  dst.u32[3] = v;
}

inline void PackFloat16x4(PPCVRegister& dst, const PPCVRegister& src) noexcept {
  for (int i = 0; i < 4; ++i) {
    const uint32_t u = src.u32[3 - i];
    PPCRegister t{};
    t.u32 = u & 0x7FFFFFFFu;
    const uint8_t exp = (t.f32 != t.f32) || (t.f32 > 65504.0f) ? 0xFFu : uint8_t((u & 0x7f800000u) >> 23);
    const uint16_t mant = exp != 0xFFu ? uint16_t((u & 0x7FE000u) >> 13) : 0u;
    uint16_t h = exp != 0xFFu
                     ? (exp > 0x70u ? uint16_t(((exp - 0x70u) << 10) + mant)
                                    : (0x71u - exp > 31u ? 0u : uint16_t((0x400u + mant) >> (0x71u - exp))))
                     : 0x7FFFu;
    h |= uint16_t((u & 0x80000000u) >> 16);
    dst.u16[7 - i] = h;
  }
}

inline void PackRGBA8(PPCVRegister& dst, const PPCVRegister& src) noexcept {
  PPCVRegister tmp{};
  uint32_t out = 0;
  constexpr unsigned kShift[4] = {24, 0, 8, 16};
  for (int i = 0; i < 4; ++i) {
    tmp.u32[i] = 0x404000FFu;
    tmp.f32[i] = !(src.f32[i] >= 3.0f) ? 3.0f : (src.f32[i] > tmp.f32[i] ? tmp.f32[i] : src.f32[i]);
    out |= uint32_t(tmp.u8[i * 4]) << kShift[i];
  }
  dst.u32[3] = out;
}

// 4. sub_8283B828: lock-free job queue minimum-tag search across 16B and 128B strided entries (11.2% self on Job 0/2/3).
namespace n_8283B828 {
inline u32 ScanImpl(const uint8_t* base, u32 r3, u32 r4, u32 r5, u32 r6) noexcept {
  const u32 flag_mask = (r6 & 0xFFu) << 24;
  u32 best_val_a = 0xFFFFFFFFu, best_val_b = 0xFFFFFFFFu;
  u32 best_idx_a = 0xFFFFFFFFu, best_idx_b = 0xFFFFFFFFu;
  u32 idx = 0;

  auto scan = [&](u32 p, u32 end, u32 byte_step, u32 idx_step) {
    while (p != end) {
      const u32* w = reinterpret_cast<const u32*>(Raw(base, p));
      if (w[0] != 0) {
        const u32 tv0 = __builtin_bswap32(w[1]);
        if ((tv0 & flag_mask) != 0) {
          const u32 v0 = tv0 & 0x00FFFFFFu;
          if (v0 < best_val_a) {
            best_val_a = v0;
            best_idx_a = idx;
          }
        }
      }
      if (w[2] != 0) {
        const u32 tv1 = __builtin_bswap32(w[3]);
        if ((tv1 & flag_mask) != 0) {
          const u32 v1 = tv1 & 0x00FFFFFFu;
          if (v1 < best_val_b) {
            best_val_b = v1;
            best_idx_b = idx + 1;
          }
        }
      }
      p += byte_step;
      idx += idx_step;
    }
  };

  const u32 end0 = r3 + (r4 << 3);
  scan(r3, end0, 16u, 2u);
  const u32 end1 = end0 + (r5 << 7);
  scan(end0, end1, 128u, 16u);
  return best_val_a < best_val_b ? best_idx_a : best_idx_b;
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf[4096]{};
  auto put_entry = [&](u32 table_off, u32 byte_off, u32 key, u32 tag_val) {
    u32* p = reinterpret_cast<u32*>(buf + table_off + byte_off);
    p[0] = __builtin_bswap32(key);
    p[1] = __builtin_bswap32(tag_val);
  };
  int failures = 0;
  auto check = [&](const char* label, u32 r4, u32 r5, u32 r6, auto setup) {
    std::memset(buf, 0, sizeof(buf));
    constexpr u32 kTable = 0x400u;
    setup(kTable);
    PPCContext orig{}, nat{};
    orig.r1.u64 = nat.r1.u64 = 0x200u;
    orig.r3.u64 = nat.r3.u64 = kTable;
    orig.r4.u64 = nat.r4.u64 = r4;
    orig.r5.u64 = nat.r5.u64 = r5;
    orig.r6.u64 = nat.r6.u64 = r6;
    ::__imp__sub_8283B828(orig, buf);
    nat.r3.u64 = ScanImpl(buf, kTable, r4, r5, r6);
    if (orig.r3.u64 != nat.r3.u64) {
      ++failures;
      sa2::enh::VerifyDifference();
      REXLOG_ERROR("[hot] DIFFERENCE sub_8283B828 self-test ({}): native={:#x} original={:#x}", label, nat.r3.u64,
                   orig.r3.u64);
    }
  };
  check("empty", 4, 2, 0x3E, [&](u32) {});
  check("r4=0 r5=0", 0, 0, 0xFF, [&](u32) {});
  check("single even", 4, 0, 0x3E, [&](u32 t) { put_entry(t, 16, 1, 0x02000100); });
  check("single odd", 4, 0, 0x3E, [&](u32 t) { put_entry(t, 24, 1, 0x02000100); });
  check("tie-break equal odd wins", 4, 0, 0x3E, [&](u32 t) {
    put_entry(t, 0, 1, 0x02000055);
    put_entry(t, 24, 2, 0x04000055);
  });
  check("pass0 only min", 6, 0, 0x0F, [&](u32 t) {
    put_entry(t, 0, 1, 0x01000200);
    put_entry(t, 16, 2, 0x01000080);
    put_entry(t, 40, 3, 0x0200007F);
  });
  check("pass1 only", 0, 3, 0x01, [&](u32 t) {
    put_entry(t, 128, 9, 0x01000090);
    put_entry(t, 264, 9, 0x01000010);
  });
  check("pass0+pass1 idx continuity", 4, 2, 0xFF, [&](u32 t) {
    put_entry(t, 0, 1, 0x01000500);
    put_entry(t, 32, 2, 0x01000300);       // end0 = t + 32, idx = 4 (even)
    put_entry(t, 32 + 136, 3, 0x01000010); // pass1 iter 1 odd = 4 + 16 + 1 = 21
  });
  check("mask filtering", 4, 1, 0x3E, [&](u32 t) {
    put_entry(t, 0, 1, 0x01000001);        // mask bit 0 doesn't match 0x3E
    put_entry(t, 8, 2, 0x80000002);        // mask bit 7 doesn't match 0x3E
    put_entry(t, 40, 3, 0x04000020);       // pass1 odd matches
  });
  check("key==0 ignored", 4, 0, 0xFF, [&](u32 t) {
    put_entry(t, 0, 0, 0xFF000000);
    put_entry(t, 8, 1, 0xFF000005);
  });
  check("min value 0", 2, 0, 0x01, [&](u32 t) { put_entry(t, 0, 7, 0x01000000); });
  check("max value 0xFFFFFF", 2, 0, 0x01, [&](u32 t) { put_entry(t, 8, 7, 0x01FFFFFF); });

  if (failures == 0) {
    REXLOG_INFO("[hot] sub_8283B828 deterministic self-test: 12/12 passed");
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const u32 r3 = ctx.r3.u32;
  const u32 r4 = ctx.r4.u32;
  const u32 r5 = ctx.r5.u32;
  const u32 r6 = ctx.r6.u32;
  if ((r3 & 15u) != 0 || (r4 & 1u) != 0 || r4 > 0x10000u || r5 > 0x10000u) {
    ::__imp__sub_8283B828(ctx, base);
    return;
  }
  ctx.r3.u64 = ScanImpl(base, r3, r4, r5, r6);
}
}  // namespace n_8283B828

// Shared NEON/scalar vertex-attribute packers for sub_826D6B18 and sub_826D6D18.
namespace vtx_pack {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

inline void StoreBeU32(uint8_t* p, uint32_t v) noexcept {
  const uint32_t be = __builtin_bswap32(v);
  std::memcpy(p, &be, 4);
}

inline uint32_t PackDec3N_Word(const uint8_t* src_be) noexcept {
#if defined(__aarch64__)
  const float32x4_t x = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be)));
  const uint32x4_t scale_bits = {0xB8FF8000u, 0xB8FF8000u, 0xB8FF8000u, 0xB5400000u};
  const float32x4_t v = vfmsq_f32(vdupq_n_f32(3.0f), x, vreinterpretq_f32_u32(scale_bits));
  const float32x4_t vmin = {kPack2101010_Min10, kPack2101010_Min10, kPack2101010_Min10, kPack2101010_Min2};
  const float32x4_t vmax = {kPack2101010_Max10, kPack2101010_Max10, kPack2101010_Max10, kPack2101010_Max2};
  const float32x4_t clamped = vbslq_f32(vcltq_f32(v, vmin), vmin, vbslq_f32(vcgtq_f32(v, vmax), vmax, v));
  const uint32x4_t u = vreinterpretq_u32_f32(clamped);
  return (vgetq_lane_u32(u, 0) & 0x3FFu) | ((vgetq_lane_u32(u, 1) & 0x3FFu) << 10) |
         ((vgetq_lane_u32(u, 2) & 0x3FFu) << 20) | ((vgetq_lane_u32(u, 3) & 0x3u) << 30);
#else
  auto bit_u32 = [](float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
  };
  auto clamp_bits = [&](float x, float scale, float fmin, float fmax, uint32_t mask) -> uint32_t {
    const float v = std::fma(-x, scale, 3.0f);
    const float c = (v < fmin) ? fmin : ((v > fmax) ? fmax : v);
    return bit_u32(c) & mask;
  };
  float s10, s2;
  const uint32_t b10 = 0xB8FF8000u, b2 = 0xB5400000u;
  std::memcpy(&s10, &b10, 4);
  std::memcpy(&s2, &b2, 4);
  return clamp_bits(LoadBeF32(src_be + 0u), s10, kPack2101010_Min10, kPack2101010_Max10, 0x3FFu) |
         (clamp_bits(LoadBeF32(src_be + 4u), s10, kPack2101010_Min10, kPack2101010_Max10, 0x3FFu) << 10) |
         (clamp_bits(LoadBeF32(src_be + 8u), s10, kPack2101010_Min10, kPack2101010_Max10, 0x3FFu) << 20) |
         (clamp_bits(LoadBeF32(src_be + 12u), s2, kPack2101010_Min2, kPack2101010_Max2, 0x3u) << 30);
#endif
}

inline void PackFloat16x4_StoreBE(const uint8_t* src_be, uint8_t* dst0_be, uint8_t* dst1_be) noexcept {
#if defined(__aarch64__)
  const uint32x4_t u = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(src_be)));
  const uint32x4_t abs_u = vandq_u32(u, vdupq_n_u32(0x7FFFFFFFu));
  const uint32x4_t overflow = vcgtq_u32(abs_u, vdupq_n_u32(0x477FE000u));
  const uint32x4_t exp = vshrq_n_u32(abs_u, 23);
  const uint32x4_t mant = vandq_u32(vshrq_n_u32(abs_u, 13), vdupq_n_u32(0x3FFu));
  const uint32x4_t is_norm = vcgtq_u32(exp, vdupq_n_u32(0x70u));
  const uint32x4_t h_norm = vaddq_u32(vshlq_n_u32(vsubq_u32(exp, vdupq_n_u32(0x70u)), 10), mant);
  const int32x4_t sub_shift = vsubq_s32(vreinterpretq_s32_u32(exp), vdupq_n_s32(0x71));
  const uint32x4_t sub_ok = vcgeq_s32(sub_shift, vdupq_n_s32(-31));
  const uint32x4_t h_sub = vandq_u32(vshlq_u32(vaddq_u32(vdupq_n_u32(0x400u), mant), sub_shift), sub_ok);
  uint32x4_t h = vbslq_u32(is_norm, h_norm, h_sub);
  h = vbslq_u32(overflow, vdupq_n_u32(0x7FFFu), h);
  h = vorrq_u32(h, vshrq_n_u32(vandq_u32(u, vdupq_n_u32(0x80000000u)), 16));
  const uint8x8_t be_bytes = vrev16_u8(vreinterpret_u8_u16(vmovn_u32(h)));
  if (dst1_be == dst0_be + 4) {
    vst1_u8(dst0_be, be_bytes);
  } else {
    const uint32x2_t dwords = vreinterpret_u32_u8(be_bytes);
    const uint32_t w0 = vget_lane_u32(dwords, 0);
    const uint32_t w1 = vget_lane_u32(dwords, 1);
    std::memcpy(dst0_be, &w0, 4);
    std::memcpy(dst1_be, &w1, 4);
  }
#else
  uint16_t h[4]{};
  for (int i = 0; i < 4; ++i) {
    uint32_t u;
    std::memcpy(&u, src_be + i * 4, 4);
    u = __builtin_bswap32(u);
    PPCRegister t{};
    t.u32 = u & 0x7FFFFFFFu;
    const uint8_t exp = (t.f32 != t.f32) || (t.f32 > 65504.0f) ? 0xFFu : uint8_t(t.u32 >> 23);
    const uint16_t mant = exp != 0xFFu ? uint16_t((u & 0x7FE000u) >> 13) : 0u;
    uint16_t hv = exp != 0xFFu
                      ? (exp > 0x70u ? uint16_t(((exp - 0x70u) << 10) + mant)
                                     : (0x71u - exp > 31u ? 0u : uint16_t((0x400u + mant) >> (0x71u - exp))))
                      : 0x7FFFu;
    hv |= uint16_t((u & 0x80000000u) >> 16);
    h[i] = hv;
  }
  StoreBeU32(dst0_be, (uint32_t(h[0]) << 16) | uint32_t(h[1]));
  StoreBeU32(dst1_be, (uint32_t(h[2]) << 16) | uint32_t(h[3]));
#endif
}

inline void PackRGBA8_StoreBE(const uint8_t* src_be, uint8_t* dst_be) noexcept {
#if defined(__aarch64__)
  const float32x4_t col = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be)));
  const uint32_t sb = 0xB87F0000u;
  float sc;
  std::memcpy(&sc, &sb, 4);
  const float32x4_t v = vfmsq_f32(vdupq_n_f32(3.0f), col, vdupq_n_f32(sc));
  const float32x4_t vmin = vdupq_n_f32(3.0f);
  const float32x4_t vmax = vreinterpretq_f32_u32(vdupq_n_u32(0x404000FFu));
  const float32x4_t v_lo = vbslq_f32(vcgeq_f32(v, vmin), v, vmin);
  const float32x4_t v_clamped = vbslq_f32(vcgtq_f32(v_lo, vmax), vmax, v_lo);
  const uint32x4_t u = vandq_u32(vreinterpretq_u32_f32(v_clamped), vdupq_n_u32(0xFFu));
  // BE memory layout [R, G, B, A] = little-endian uint32 (R | G<<8 | B<<16 | A<<24).
  const uint32_t le = vgetq_lane_u32(u, 0) | (vgetq_lane_u32(u, 1) << 8) | (vgetq_lane_u32(u, 2) << 16) |
                      (vgetq_lane_u32(u, 3) << 24);
  std::memcpy(dst_be, &le, 4);
#else
  float sc, vmax_f;
  const uint32_t sb = 0xB87F0000u, mb = 0x404000FFu;
  std::memcpy(&sc, &sb, 4);
  std::memcpy(&vmax_f, &mb, 4);
  uint32_t le = 0;
  for (int i = 0; i < 4; ++i) {
    const float v = std::fma(-LoadBeF32(src_be + i * 4), sc, 3.0f);
    const float c = !(v >= 3.0f) ? 3.0f : (v > vmax_f ? vmax_f : v);
    uint32_t u;
    std::memcpy(&u, &c, 4);
    le |= (u & 0xFFu) << (i * 8);
  }
  std::memcpy(dst_be, &le, 4);
#endif
}

template <bool kHasColor>
inline void PackQuadImpl(uint8_t* base, uint32_t dst_va, uint32_t in_pos_va, uint32_t in_nrm_va, uint32_t in_tan_va,
                         uint32_t in_uv_va, uint32_t in_col_va, uint32_t off_pos, uint32_t off_nrm, uint32_t off_tan,
                         uint32_t off_uv, uint32_t off_col, uint32_t stride) noexcept {
  for (uint32_t k = 0; k < 4u; ++k) {
    const uint32_t ko = k << 4;
    const uint32_t row = dst_va + k * stride;
    std::memcpy(Raw(base, (row + off_pos) & ~0xFu), Raw(base, (in_pos_va + ko) & ~0xFu), 16u);
    StoreBeU32(Raw(base, (row + off_nrm) & ~0x3u), PackDec3N_Word(Raw(base, (in_nrm_va + ko) & ~0xFu)));
    StoreBeU32(Raw(base, (row + off_tan) & ~0x3u), PackDec3N_Word(Raw(base, (in_tan_va + ko) & ~0xFu)));
    const uint32_t uv_ea0 = (row + off_uv) & ~0x3u;
    const uint32_t uv_ea1 = (row + off_uv + 4u) & ~0x3u;
    PackFloat16x4_StoreBE(Raw(base, (in_uv_va + ko) & ~0xFu), Raw(base, uv_ea0), Raw(base, uv_ea1));
    if constexpr (kHasColor) {
      PackRGBA8_StoreBE(Raw(base, (in_col_va + ko) & ~0xFu), Raw(base, (row + off_col) & ~0x3u));
    }
  }
}

// Kept out-of-line so check_switch_build never finds vector FMLA inside sub_826D6B18 or sub_826D6D18.
[[gnu::noinline]] void PackQuad4(uint8_t* base, uint32_t dst_va, uint32_t in_pos_va, uint32_t in_nrm_va,
                                 uint32_t in_tan_va, uint32_t in_uv_va, uint32_t off_pos, uint32_t off_nrm,
                                 uint32_t off_tan, uint32_t off_uv, uint32_t stride) noexcept {
  PackQuadImpl<false>(base, dst_va, in_pos_va, in_nrm_va, in_tan_va, in_uv_va, 0u, off_pos, off_nrm, off_tan, off_uv,
                      0u, stride);
}

[[gnu::noinline]] void PackQuad5(uint8_t* base, uint32_t dst_va, uint32_t in_pos_va, uint32_t in_nrm_va,
                                 uint32_t in_tan_va, uint32_t in_uv_va, uint32_t in_col_va, uint32_t off_pos,
                                 uint32_t off_nrm, uint32_t off_tan, uint32_t off_uv, uint32_t off_col,
                                 uint32_t stride) noexcept {
  PackQuadImpl<true>(base, dst_va, in_pos_va, in_nrm_va, in_tan_va, in_uv_va, in_col_va, off_pos, off_nrm, off_tan,
                     off_uv, off_col, stride);
}

inline void MarkQuadCards(uint8_t* base, uint32_t dst_va, uint32_t off_pos, uint32_t off_nrm, uint32_t off_tan,
                          uint32_t off_uv, bool has_col, uint32_t off_col, uint32_t stride) noexcept {
  const uint32_t row3 = dst_va + 3u * stride;
  if (row3 >= dst_va) [[likely]] {
    uint32_t lo = (dst_va + off_pos) & ~0xFu;
    auto min_lo = [&](uint32_t a) {
      const uint32_t s = a & ~3u;
      if (s < lo) lo = s;
    };
    min_lo(dst_va + off_nrm);
    min_lo(dst_va + off_tan);
    min_lo(dst_va + off_uv);
    if (has_col) min_lo(dst_va + off_col);

    uint32_t hi = ((row3 + off_pos) & ~0xFu) + 16u;
    auto max_hi = [&](uint32_t a, uint32_t sz) {
      const uint32_t e = (a & ~3u) + sz;
      if (e > hi) hi = e;
    };
    max_hi(row3 + off_nrm, 4u);
    max_hi(row3 + off_tan, 4u);
    max_hi(row3 + off_uv + 4u, 4u);
    if (has_col) max_hi(row3 + off_col, 4u);
    if (lo < hi) Card(base, lo, hi - lo);
    return;
  }
  uint32_t lo = UINT32_MAX, hi = 0;
  auto span = [&](uint32_t a, uint32_t align_mask, uint32_t sz) {
    const uint32_t s = a & align_mask;
    if (s < lo) lo = s;
    if (s + sz > hi) hi = s + sz;
  };
  for (uint32_t k = 0; k < 4u; ++k) {
    const uint32_t row = dst_va + k * stride;
    span(row + off_pos, ~0xFu, 16u);
    span(row + off_nrm, ~3u, 4u);
    span(row + off_tan, ~3u, 4u);
    span(row + off_uv, ~3u, 4u);
    span(row + off_uv + 4u, ~3u, 4u);
    if (has_col) span(row + off_col, ~3u, 4u);
  }
  if (lo < hi) Card(base, lo, hi - lo);
}

}  // namespace vtx_pack

// 5. sub_826D6D18: vertex quad packer/quantizer (5 inputs, Job 0/2/3).
namespace n_826D6D18 {

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[4096]{}, buf_nat[4096]{};
  auto setup = [&](uint8_t* b, u32 stride) {
    std::memset(b, 0xCC, 4096);
    const float vals[4][4] = {
        {0.0f, 1.0f, -1.0f, 2.0f},
        {-2.0f, 0.5f, -0.25f, 0.0f},
        {0.25f, -0.75f, 0.5f, 1.25f},
        {1.0f, -1.0f, 0.0f, 3.0f},
    };
    const float uvs[4][4] = {
        {0.0f, 0.5f, -1.25f, 1e-6f},
        {70000.0f, -70000.0f, -0.0f, 65504.0f},
        {1.0f, -1.0f, 1e-9f, 0.125f},
        {0.25f, 0.75f, -0.5f, 2.0f},
    };
    const float cols[4][4] = {
        {-0.1f, 0.0f, 0.5f, 1.0f},
        {1.5f, 0.25f, 0.75f, 0.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.125f, 0.875f, 0.5f},
    };
    for (u32 k = 0; k < 4; ++k) {
      for (u32 c = 0; c < 4; ++c) {
        vtx_pack::StoreBeF32(b + 0x400 + k * 16 + c * 4, float(k + 1) * (c + 1));
        vtx_pack::StoreBeF32(b + 0x480 + k * 16 + c * 4, vals[k][c]);
        vtx_pack::StoreBeF32(b + 0x500 + k * 16 + c * 4, vals[(k + 1) & 3][c]);
        vtx_pack::StoreBeF32(b + 0x580 + k * 16 + c * 4, uvs[k][c]);
        vtx_pack::StoreBeF32(b + 0x600 + k * 16 + c * 4, cols[k][c]);
      }
    }
    vtx_pack::StoreBeU32(b + 0x200 + 84u, 20u);
    vtx_pack::StoreBeU32(b + 0x200 + 92u, 24u);
    vtx_pack::StoreBeU32(b + 0x200 + 100u, 32u);
    vtx_pack::StoreBeU32(b + 0x200 + 108u, stride);
  };
  int failures = 0;
  for (u32 stride : {48u, 36u}) {
    setup(buf_orig, stride);
    std::memcpy(buf_nat, buf_orig, 4096);
    PPCContext orig{}, nat{};
    orig.r1.u64 = nat.r1.u64 = 0x200u;
    orig.r3.u64 = nat.r3.u64 = 0x800u;
    orig.r4.u64 = nat.r4.u64 = 0x400u;
    orig.r5.u64 = nat.r5.u64 = 0x480u;
    orig.r6.u64 = nat.r6.u64 = 0x500u;
    orig.r7.u64 = nat.r7.u64 = 0x580u;
    orig.r8.u64 = nat.r8.u64 = 0x600u;
    orig.r9.u64 = nat.r9.u64 = 0u;
    orig.r10.u64 = nat.r10.u64 = 16u;
    ::__imp__sub_826D6D18(orig, buf_orig);
    vtx_pack::PackQuad5(buf_nat, 0x800u, 0x400u, 0x480u, 0x500u, 0x580u, 0x600u, 0u, 16u, 20u, 24u, 32u, stride);
    if (std::memcmp(buf_orig + 0x800u, buf_nat + 0x800u, 4u * stride + 16u) != 0) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_826D6D18 self-test stride={}", stride);
    }
  }
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_826D6D18 quad5 self-test: 2/2 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t sp = ctx.r1.u32;
  const uint32_t dst_va = ctx.r3.u32;
  const uint32_t in_pos = ctx.r4.u32;
  const uint32_t in_nrm = ctx.r5.u32;
  const uint32_t in_tan = ctx.r6.u32;
  const uint32_t in_uv = ctx.r7.u32;
  const uint32_t in_col = ctx.r8.u32;
  if (sp < 0x1000u || dst_va < 0x1000u || in_pos < 0x1000u || in_nrm < 0x1000u || in_tan < 0x1000u ||
      in_uv < 0x1000u || in_col < 0x1000u) {
    ::__imp__sub_826D6D18(ctx, base);
    return;
  }
  const uint32_t off_pos = ctx.r9.u32;
  const uint32_t off_nrm = ctx.r10.u32;
  const uint32_t off_tan = REX_LOAD_U32(sp + 84u);
  const uint32_t off_uv = REX_LOAD_U32(sp + 92u);
  const uint32_t off_col = REX_LOAD_U32(sp + 100u);
  const uint32_t stride = REX_LOAD_U32(sp + 108u);
  vtx_pack::MarkQuadCards(base, dst_va, off_pos, off_nrm, off_tan, off_uv, true, off_col, stride);
  vtx_pack::PackQuad5(base, dst_va, in_pos, in_nrm, in_tan, in_uv, in_col, off_pos, off_nrm, off_tan, off_uv, off_col,
                      stride);
  ctx.fpscr.enableFlushMode();
}

}  // namespace n_826D6D18

// 5b. sub_826D6B18: vertex quad packer/quantizer (4 inputs, Job 0/2/3).
namespace n_826D6B18 {

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[4096]{}, buf_nat[4096]{};
  auto setup = [&](uint8_t* b, u32 stride) {
    std::memset(b, 0xCC, 4096);
    const float vals[4][4] = {
        {0.0f, 1.0f, -1.0f, 2.0f},
        {-2.0f, 0.5f, -0.25f, 0.0f},
        {0.25f, -0.75f, 0.5f, 1.25f},
        {1.0f, -1.0f, 0.0f, 3.0f},
    };
    const float uvs[4][4] = {
        {0.0f, 0.5f, -1.25f, 1e-6f},
        {70000.0f, -70000.0f, -0.0f, 65504.0f},
        {1.0f, -1.0f, 1e-9f, 0.125f},
        {0.25f, 0.75f, -0.5f, 2.0f},
    };
    for (u32 k = 0; k < 4; ++k) {
      for (u32 c = 0; c < 4; ++c) {
        vtx_pack::StoreBeF32(b + 0x400 + k * 16 + c * 4, float(k + 1) * (c + 1));
        vtx_pack::StoreBeF32(b + 0x480 + k * 16 + c * 4, vals[k][c]);
        vtx_pack::StoreBeF32(b + 0x500 + k * 16 + c * 4, vals[(k + 1) & 3][c]);
        vtx_pack::StoreBeF32(b + 0x580 + k * 16 + c * 4, uvs[k][c]);
      }
    }
    vtx_pack::StoreBeU32(b + 0x200 + 84u, 24u);
    vtx_pack::StoreBeU32(b + 0x200 + 92u, stride);
  };
  int failures = 0;
  for (u32 stride : {32u, 48u}) {
    setup(buf_orig, stride);
    std::memcpy(buf_nat, buf_orig, 4096);
    PPCContext orig{}, nat{};
    orig.r1.u64 = nat.r1.u64 = 0x200u;
    orig.r3.u64 = nat.r3.u64 = 0x800u;
    orig.r4.u64 = nat.r4.u64 = 0x400u;
    orig.r5.u64 = nat.r5.u64 = 0x480u;
    orig.r6.u64 = nat.r6.u64 = 0x500u;
    orig.r7.u64 = nat.r7.u64 = 0x580u;
    orig.r8.u64 = nat.r8.u64 = 0u;
    orig.r9.u64 = nat.r9.u64 = 16u;
    orig.r10.u64 = nat.r10.u64 = 20u;
    ::__imp__sub_826D6B18(orig, buf_orig);
    vtx_pack::PackQuad4(buf_nat, 0x800u, 0x400u, 0x480u, 0x500u, 0x580u, 0u, 16u, 20u, 24u, stride);
    if (std::memcmp(buf_orig + 0x800u, buf_nat + 0x800u, 4u * stride + 16u) != 0) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_826D6B18 self-test stride={}", stride);
    }
  }
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_826D6B18 quad4 self-test: 2/2 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t sp = ctx.r1.u32;
  const uint32_t dst_va = ctx.r3.u32;
  const uint32_t in_pos = ctx.r4.u32;
  const uint32_t in_nrm = ctx.r5.u32;
  const uint32_t in_tan = ctx.r6.u32;
  const uint32_t in_uv = ctx.r7.u32;
  if (sp < 0x1000u || dst_va < 0x1000u || in_pos < 0x1000u || in_nrm < 0x1000u || in_tan < 0x1000u ||
      in_uv < 0x1000u) {
    ::__imp__sub_826D6B18(ctx, base);
    return;
  }
  const uint32_t off_pos = ctx.r8.u32;
  const uint32_t off_nrm = ctx.r9.u32;
  const uint32_t off_tan = ctx.r10.u32;
  const uint32_t off_uv = REX_LOAD_U32(sp + 84u);
  const uint32_t stride = REX_LOAD_U32(sp + 92u);
  vtx_pack::MarkQuadCards(base, dst_va, off_pos, off_nrm, off_tan, off_uv, false, 0u, stride);
  vtx_pack::PackQuad4(base, dst_va, in_pos, in_nrm, in_tan, in_uv, off_pos, off_nrm, off_tan, off_uv, stride);
  ctx.fpscr.enableFlushMode();
}

}  // namespace n_826D6B18

// 5c. sub_82C61450: XMemCpy (dst=r3, src=r4, size=r5 -> returns dst in r3).
namespace n_82C61450 {

inline void RunSelfTest(uint8_t* base) {
  int failures = 0;
  // loc_82C614DC reads guest .data byte-loop step at 0x82F79984; verify on real guest memory.
  const u32 byte_step = REX_LOAD_U32(0x82F79984u);
  if (byte_step != 1u) {
    ++failures;
    REXLOG_ERROR("[hot] DIFFERENCE sub_82C61450 byte_step={} (expected 1)", byte_step);
  }
  alignas(128) uint8_t buf_orig[4096]{}, buf_nat[4096]{};
  const struct {
    u32 dst, src, n;
  } cases[] = {
      {0x400u, 0xA00u, 0u},    {0x400u, 0xA04u, 4u},    {0x400u, 0xA00u, 8u},
      {0x403u, 0xA00u, 12u},   {0x400u, 0xA00u, 16u},   {0x407u, 0xA07u, 33u},
      {0x400u, 0xA00u, 128u},  {0x403u, 0xA03u, 300u},  {0x400u, 0xA00u, 1040u},
  };
  for (const auto& tc : cases) {
    std::memset(buf_orig, 0xA5, sizeof(buf_orig));
    for (u32 i = 0; i < 1280u; ++i) buf_orig[0xA00u + i] = uint8_t((i * 37u + 11u) & 0xFFu);
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{}, nat{};
    orig.r1.u64 = nat.r1.u64 = 0x200u;
    orig.r3.u64 = nat.r3.u64 = tc.dst;
    orig.r4.u64 = nat.r4.u64 = tc.src;
    orig.r5.u64 = nat.r5.u64 = tc.n;
    ::__imp__sub_82C61450(orig, buf_orig);
    if (tc.n != 0u) {
      std::memcpy(buf_nat + tc.dst, buf_nat + tc.src, tc.n);
    }
    if (orig.r3.u32 != tc.dst || std::memcmp(buf_orig + 0x380u, buf_nat + 0x380u, 1280u) != 0) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_82C61450 self-test n={} dst={:#x} src={:#x}", tc.n, tc.dst, tc.src);
    }
  }
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82C61450 XMemCpy self-test: 9/9 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [base] {
      RunSelfTest(base);
      tested_done.store(true, std::memory_order_release);
    });
  }
  const u32 dst = ctx.r3.u32;
  const u32 src = ctx.r4.u32;
  const u32 n = ctx.r5.u32;
  if ((n >> 31) != 0 || dst + n < dst || src + n < src || (dst < src + n && src < dst + n) ||
      (dst < 0xE0000000u && dst + n > 0xE0000000u) || (src < 0xE0000000u && src + n > 0xE0000000u)) {
    ::__imp__sub_82C61450(ctx, base);
    return;
  }
  if (n != 0u) {
    std::memcpy(Raw(base, dst), Raw(base, src), n);
    Card(base, dst, n);
  }
}

}  // namespace n_82C61450

// 6. sub_82789120: 4-vertex SIMD matrix/skinning transform loop (3.4% Job 0/2/3 + 2.0% FGGameSim). Barrier-free + non-volatile (SA2_HOT_HOOK_COPY).
namespace n_82789120 {
inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
	REX_FUNC_PROLOGUE();
	REX_LEAF_LOCAL(PPCRegister, l_r1, r1);
	REX_LEAF_LOCAL(PPCRegister, l_r3, r3);
	REX_LEAF_LOCAL(PPCRegister, l_r4, r4);
	REX_LEAF_LOCAL(PPCRegister, l_r5, r5);
	REX_LEAF_LOCAL(PPCRegister, l_r6, r6);
	REX_LEAF_LOCAL(PPCRegister, l_r7, r7);
	REX_LEAF_LOCAL(PPCRegister, l_r8, r8);
	REX_LEAF_LOCAL(PPCRegister, l_r9, r9);
	REX_LEAF_LOCAL(PPCRegister, l_r10, r10);
	REX_LEAF_LOCAL(PPCVRegister, l_v0, v0);
	REX_LEAF_LOCAL(PPCVRegister, l_v1, v1);
	REX_LEAF_LOCAL(PPCVRegister, l_v2, v2);
	REX_LEAF_LOCAL(PPCVRegister, l_v3, v3);
	REX_LEAF_LOCAL(PPCVRegister, l_v4, v4);
	REX_LEAF_LOCAL(PPCVRegister, l_v5, v5);
	REX_LEAF_LOCAL(PPCVRegister, l_v6, v6);
	REX_LEAF_LOCAL(PPCVRegister, l_v7, v7);
	REX_LEAF_LOCAL(PPCVRegister, l_v8, v8);
	REX_LEAF_LOCAL(PPCVRegister, l_v9, v9);
	REX_LEAF_LOCAL(PPCVRegister, l_v10, v10);
	REX_LEAF_LOCAL(PPCVRegister, l_v11, v11);
	REX_LEAF_LOCAL(PPCVRegister, l_v12, v12);
	REX_LEAF_LOCAL(PPCVRegister, l_v13, v13);
	PPCRegister ctr{};
	PPCXERRegister xer{};
	PPCCRRegister cr6{};
	PPCRegister r11{};
	PPCRegister r12{};
	PPCRegister r14{};
	PPCRegister r15{};
	PPCRegister r16{};
	PPCRegister r17{};
	PPCRegister r18{};
	PPCRegister r19{};
	PPCRegister r20{};
	PPCRegister r21{};
	PPCRegister r22{};
	PPCRegister r23{};
	PPCRegister r24{};
	PPCRegister r25{};
	PPCRegister r26{};
	PPCRegister r27{};
	PPCRegister r28{};
	PPCRegister r29{};
	PPCRegister r30{};
	PPCRegister r31{};
	PPCVRegister v14{};
	PPCVRegister v15{};
	PPCVRegister v16{};
	PPCVRegister v17{};
	PPCVRegister v18{};
	PPCVRegister v19{};
	PPCVRegister v20{};
	PPCVRegister v21{};
	PPCVRegister v22{};
	PPCVRegister v23{};
	PPCVRegister v24{};
	PPCVRegister v25{};
	PPCVRegister v26{};
	PPCVRegister v27{};
	PPCVRegister v28{};
	PPCVRegister v29{};
	PPCVRegister v30{};
	PPCVRegister v31{};
	PPCVRegister v32{};
	PPCVRegister v33{};
	PPCVRegister v34{};
	PPCVRegister v35{};
	PPCVRegister v36{};
	PPCVRegister v37{};
	PPCVRegister v38{};
	PPCVRegister v39{};
	PPCVRegister v40{};
	PPCVRegister v41{};
	PPCVRegister v42{};
	PPCVRegister v43{};
	PPCVRegister v44{};
	PPCVRegister v45{};
	PPCVRegister v46{};
	PPCVRegister v47{};
	PPCVRegister v48{};
	PPCVRegister v49{};
	PPCVRegister v50{};
	PPCVRegister v51{};
	PPCVRegister v52{};
	PPCVRegister v53{};
	PPCVRegister v54{};
	PPCVRegister v55{};
	PPCVRegister v56{};
	PPCVRegister v57{};
	PPCVRegister v58{};
	PPCVRegister v59{};
	PPCVRegister v60{};
	PPCVRegister v61{};
	PPCVRegister v62{};
	PPCVRegister v63{};
	PPCVRegister v94{};
	PPCVRegister v95{};
	PPCVRegister v96{};
	PPCVRegister v97{};
	PPCVRegister v98{};
	PPCVRegister v99{};
	PPCVRegister v100{};
	PPCVRegister v101{};
	PPCVRegister v102{};
	PPCVRegister v103{};
	PPCVRegister v104{};
	PPCVRegister v105{};
	PPCVRegister v106{};
	PPCVRegister v107{};
	PPCVRegister v108{};
	PPCVRegister v109{};
	PPCVRegister v110{};
	PPCVRegister v111{};
	PPCVRegister v112{};
	PPCVRegister v113{};
	PPCVRegister v114{};
	PPCVRegister v115{};
	PPCVRegister v116{};
	PPCVRegister v117{};
	PPCVRegister v118{};
	PPCVRegister v119{};
	PPCVRegister v120{};
	PPCVRegister v121{};
	PPCVRegister v122{};
	PPCVRegister v123{};
	PPCVRegister v124{};
	PPCVRegister v125{};
	PPCVRegister v126{};
	PPCVRegister v127{};
	PPCRegister temp{};
	uint32_t ea{};
	// mflr r12
	r12.u64 = ctx.lr;
	// bl 0x8229b340
	// addi r12,r1,-160
	r12.s64 = l_r1.s64 + -160;
	// bl 0x82c72284
	// stwu r1,-736(r1)
	ea = -736 + l_r1.u32;
	REX_CHAIN_STORE(REX_STORE_U32(ea, l_r1.u32));
	l_r1.u32 = ea;
	// lvsl v0,r0,r9
	temp.u32 = l_r9.u32;
	simde_mm_store_si128((simde__m128i*)l_v0.u8, simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(temp.u32 & 0xF) * 16]));
	// cmplwi cr6,r10,0
	cr6.compare<uint32_t>(l_r10.u32, 0, xer);
	// lvx128 v63,r0,r9
	ea = (l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vspltw v0,v0,0
	simde_mm_store_si128((simde__m128i*)l_v0.u32, simde_mm_shuffle_epi32(simde_mm_load_si128((simde__m128i*)l_v0.u32), 0xFF));
	// vperm128 v63,v63,v63,v0
	simde_mm_store_si128((simde__m128i*)v63.u8, REX_VPERM1(simde_mm_load_si128((simde__m128i*)v63.u8), simde_mm_load_si128((simde__m128i*)l_v0.u8)));
	// beq cr6,0x827898b4
	if (cr6.eq) goto loc_827898B4;
	// addi r11,r10,-1
	r11.s64 = l_r10.s64 + -1;
	// vspltisw128 v62,0
	simde_mm_store_si128((simde__m128i*)v62.u32, simde_mm_set1_epi32(int(0x0)));
	// addi r10,r4,32
	l_r10.s64 = l_r4.s64 + 32;
	// rlwinm r9,r11,30,2,31
	l_r9.u64 = (r11.u32 >> 2);
	// addi r11,r3,32
	r11.s64 = l_r3.s64 + 32;
	// addi r9,r9,1
	l_r9.s64 = l_r9.s64 + 1;
	// li r22,96
	r22.s64 = 96;
	// li r24,-16
	r24.s64 = -16;
	// li r25,16
	r25.s64 = 16;
	// li r26,32
	r26.s64 = 32;
	// mtctr r9
	ctr.u64 = l_r9.u64;
	// li r27,48
	r27.s64 = 48;
loc_8278917C:
	// addi r9,r11,-32
	l_r9.s64 = r11.s64 + -32;
	// li r8,960
	l_r8.s64 = 960;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1088
	l_r8.s64 = 1088;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1216
	l_r8.s64 = 1216;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1344
	l_r8.s64 = 1344;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1472
	l_r8.s64 = 1472;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1600
	l_r8.s64 = 1600;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1728
	l_r8.s64 = 1728;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// li r8,1856
	l_r8.s64 = 1856;
	// dcbt r8,r9
	REX_PREFETCH(l_r8.u32 + l_r9.u32, 0);
	// addi r28,r10,-32
	r28.s64 = l_r10.s64 + -32;
	// mr r8,r22
	l_r8.u64 = r22.u64;
	// dcbt r22,r28
	REX_PREFETCH(r22.u32 + r28.u32, 0);
	// li r8,64
	l_r8.s64 = 64;
	// lvx128 v41,r0,r9
	ea = (l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v41.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r9,80
	l_r9.s64 = 80;
	// lvx128 v30,r0,r11
	ea = (r11.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v30.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r7,112
	l_r7.s64 = 112;
	// lvx128 v40,r11,r24
	ea = (r11.u32 + r24.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v40.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,128
	l_r4.s64 = 128;
	// lvx128 v39,r11,r25
	ea = (r11.u32 + r25.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v39.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,144
	l_r3.s64 = 144;
	// lvx128 v38,r11,r26
	ea = (r11.u32 + r26.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v38.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v36,r11,r8
	ea = (r11.u32 + l_r8.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v36.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r31,160
	r31.s64 = 160;
	// li r30,176
	r30.s64 = 176;
	// lvx128 v35,r11,r9
	ea = (r11.u32 + l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v35.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r8,192
	l_r8.s64 = 192;
	// lvx128 v33,r11,r7
	ea = (r11.u32 + l_r7.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v33.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v32,r11,r4
	ea = (r11.u32 + l_r4.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v32.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r9,208
	l_r9.s64 = 208;
	// lvx128 v127,r11,r3
	ea = (r11.u32 + l_r3.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v127.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r29,224
	r29.s64 = 224;
	// lvx128 v126,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v126.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r7,240
	l_r7.s64 = 240;
	// lvx128 v125,r11,r30
	ea = (r11.u32 + r30.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v125.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,256
	l_r4.s64 = 256;
	// lvx128 v124,r11,r8
	ea = (r11.u32 + l_r8.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v124.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,272
	l_r3.s64 = 272;
	// li r31,288
	r31.s64 = 288;
	// lvx128 v123,r11,r9
	ea = (r11.u32 + l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v123.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r30,304
	r30.s64 = 304;
	// lvx128 v11,r11,r29
	ea = (r11.u32 + r29.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v11.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r8,320
	l_r8.s64 = 320;
	// lvx128 v10,r11,r7
	ea = (r11.u32 + l_r7.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v10.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v9,r11,r4
	ea = (r11.u32 + l_r4.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v9.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r9,336
	l_r9.s64 = 336;
	// lvx128 v8,r11,r3
	ea = (r11.u32 + l_r3.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v8.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r29,352
	r29.s64 = 352;
	// lvx128 v7,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v7.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r7,368
	l_r7.s64 = 368;
	// lvx128 v6,r11,r30
	ea = (r11.u32 + r30.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v6.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,384
	l_r4.s64 = 384;
	// lvx128 v122,r11,r8
	ea = (r11.u32 + l_r8.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v122.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,400
	l_r3.s64 = 400;
	// li r31,416
	r31.s64 = 416;
	// lvx128 v121,r11,r9
	ea = (r11.u32 + l_r9.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v121.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r30,432
	r30.s64 = 432;
	// lvx128 v120,r11,r29
	ea = (r11.u32 + r29.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v120.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r8,448
	l_r8.s64 = 448;
	// lvx128 v119,r11,r7
	ea = (r11.u32 + l_r7.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v119.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v118,r11,r4
	ea = (r11.u32 + l_r4.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v118.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lwz r9,896(r11)
	l_r9.u64 = REX_LOAD_D_U32(r11.u32, 896);
	// lvx128 v117,r11,r3
	ea = (r11.u32 + l_r3.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v117.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lwz r29,900(r11)
	r29.u64 = REX_LOAD_D_U32(r11.u32, 900);
	// lvx128 v116,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v116.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lwz r7,904(r11)
	l_r7.u64 = REX_LOAD_D_U32(r11.u32, 904);
	// lvx128 v37,r11,r27
	ea = (r11.u32 + r27.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v37.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,464
	l_r4.s64 = 464;
	// lvx128 v34,r11,r22
	ea = (r11.u32 + r22.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v34.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,480
	l_r3.s64 = 480;
	// lvx128 v28,r11,r30
	ea = (r11.u32 + r30.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v28.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r31,496
	r31.s64 = 496;
	// lvx128 v27,r11,r8
	ea = (r11.u32 + l_r8.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v27.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v26,r11,r3
	ea = (r11.u32 + l_r3.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v26.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,608
	l_r3.s64 = 608;
	// lvx128 v12,r11,r4
	ea = (r11.u32 + l_r4.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v12.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,592
	l_r4.s64 = 592;
	// lvx128 v25,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v25.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r21,512
	r21.s64 = 512;
	// li r19,528
	r19.s64 = 528;
	// lwz r30,912(r11)
	r30.u64 = REX_LOAD_D_U32(r11.u32, 912);
	// li r17,544
	r17.s64 = 544;
	// lwz r23,916(r11)
	r23.u64 = REX_LOAD_D_U32(r11.u32, 916);
	// lvx128 v20,r11,r3
	ea = (r11.u32 + l_r3.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v20.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,736
	l_r3.s64 = 736;
	// lvx128 v21,r11,r4
	ea = (r11.u32 + l_r4.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v21.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,720
	l_r4.s64 = 720;
	// li r16,560
	r16.s64 = 560;
	// lvx128 v29,r11,r21
	ea = (r11.u32 + r21.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v29.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r15,576
	r15.s64 = 576;
	// lvx128 v24,r11,r19
	ea = (r11.u32 + r19.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v24.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r31,624
	r31.s64 = 624;
	// lvx128 v23,r11,r17
	ea = (r11.u32 + r17.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v23.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v61,r11,r3
	ea = (r11.u32 + l_r3.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v61.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r3,880
	l_r3.s64 = 880;
	// lvx128 v14,r11,r4
	ea = (r11.u32 + l_r4.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v14.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r4,864
	l_r4.s64 = 864;
	// stw r3,20(r1)
	REX_STORE_D_U32(l_r1.u32, 20, l_r3.u32);
	// lvx128 v115,r11,r16
	ea = (r11.u32 + r16.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v115.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// stw r4,16(r1)
	REX_STORE_D_U32(l_r1.u32, 16, l_r4.u32);
	// lvx128 v22,r11,r15
	ea = (r11.u32 + r15.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v22.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v19,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v19.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r21,640
	r21.s64 = 640;
	// li r19,656
	r19.s64 = 656;
	// lwz r20,908(r11)
	r20.u64 = REX_LOAD_D_U32(r11.u32, 908);
	// li r17,672
	r17.s64 = 672;
	// lwz r18,924(r11)
	r18.u64 = REX_LOAD_D_U32(r11.u32, 924);
	// li r16,688
	r16.s64 = 688;
	// lvx128 v108,r0,r28
	ea = (r28.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v108.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r15,704
	r15.s64 = 704;
	// lvx128 v107,r10,r24
	ea = (l_r10.u32 + r24.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v107.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r31,752
	r31.s64 = 752;
	// lvx128 v18,r11,r21
	ea = (r11.u32 + r21.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v18.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r9,r9,4,0,27
	l_r9.u64 = ((uint32_t)(l_r9.u32 << 4));
	// lvx128 v17,r11,r19
	ea = (r11.u32 + r19.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v17.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r7,r7,4,0,27
	l_r7.u64 = ((uint32_t)(l_r7.u32 << 4));
	// lvx128 v16,r11,r17
	ea = (r11.u32 + r17.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v16.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r3,r30,4,0,27
	l_r3.u64 = ((uint32_t)(r30.u32 << 4));
	// lvx128 v15,r11,r16
	ea = (r11.u32 + r16.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v15.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v53,r11,r15
	ea = (r11.u32 + r15.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v53.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r21,768
	r21.s64 = 768;
	// lvx128 v52,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v52.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r19,784
	r19.s64 = 784;
	// li r17,800
	r17.s64 = 800;
	// lvx128 v59,r9,r5
	ea = (l_r9.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v59.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r16,816
	r16.s64 = 816;
	// lvx128 v58,r7,r5
	ea = (l_r7.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v58.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r15,832
	r15.s64 = 832;
	// lvx128 v106,r0,r10
	ea = (l_r10.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v106.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// li r14,848
	r14.s64 = 848;
	// lvx128 v60,r11,r21
	ea = (r11.u32 + r21.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v60.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r8,r29,4,0,27
	l_r8.u64 = ((uint32_t)(r29.u32 << 4));
	// lwz r29,920(r11)
	r29.u64 = REX_LOAD_D_U32(r11.u32, 920);
	// add r4,r9,r5
	l_r4.u64 = l_r9.u64 + l_r5.u64;
	// lvx128 v51,r11,r19
	ea = (r11.u32 + r19.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v51.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// add r4,r8,r5
	l_r4.u64 = l_r8.u64 + l_r5.u64;
	// lvx128 v114,r11,r17
	ea = (r11.u32 + r17.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v114.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lwz r30,20(r1)
	r30.u64 = REX_LOAD_D_U32(l_r1.u32, 20);
	// lvx128 v113,r11,r16
	ea = (r11.u32 + r16.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v113.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lwz r31,16(r1)
	r31.u64 = REX_LOAD_D_U32(l_r1.u32, 16);
	// lvx128 v112,r11,r15
	ea = (r11.u32 + r15.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v112.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v111,r11,r14
	ea = (r11.u32 + r14.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v111.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// add r4,r7,r5
	l_r4.u64 = l_r7.u64 + l_r5.u64;
	// lvx128 v105,r10,r25
	ea = (l_r10.u32 + r25.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v105.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r4,r20,4,0,27
	l_r4.u64 = ((uint32_t)(r20.u32 << 4));
	// lvx128 v104,r10,r26
	ea = (l_r10.u32 + r26.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v104.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrghw128 v57,v59,v58
	simde_mm_store_si128((simde__m128i*)v57.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v58.u32), simde_mm_load_si128((simde__m128i*)v59.u32)));
	// lvx128 v109,r11,r30
	ea = (r11.u32 + r30.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v109.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r30,r29,4,0,27
	r30.u64 = ((uint32_t)(r29.u32 << 4));
	// lvx128 v110,r11,r31
	ea = (r11.u32 + r31.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v110.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// rlwinm r31,r23,4,0,27
	r31.u64 = ((uint32_t)(r23.u32 << 4));
	// lvx128 v103,r10,r27
	ea = (l_r10.u32 + r27.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v103.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// addi r11,r11,960
	r11.s64 = r11.s64 + 960;
	// rlwinm r29,r18,4,0,27
	r29.u64 = ((uint32_t)(r18.u32 << 4));
	// lvx128 v55,r4,r5
	ea = (l_r4.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v55.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrglw128 v59,v59,v58
	simde_mm_store_si128((simde__m128i*)v59.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v58.u32), simde_mm_load_si128((simde__m128i*)v59.u32)));
	// lvx128 v50,r31,r5
	ea = (r31.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v50.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vor128 v46,v9,v9
	simde_mm_store_si128((simde__m128i*)v46.u8, simde_mm_load_si128((simde__m128i*)l_v9.u8));
	// lvx128 v48,r30,r5
	ea = (r30.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v48.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// add r23,r4,r5
	r23.u64 = l_r4.u64 + l_r5.u64;
	// lvx128 v47,r29,r5
	ea = (r29.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v47.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// add r23,r9,r6
	r23.u64 = l_r9.u64 + l_r6.u64;
	// lvx128 v56,r8,r5
	ea = (l_r8.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v56.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// add r23,r8,r6
	r23.u64 = l_r8.u64 + l_r6.u64;
	// lvx128 v54,r3,r5
	ea = (l_r3.u32 + l_r5.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v54.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrghw128 v43,v50,v47
	simde_mm_store_si128((simde__m128i*)v43.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v47.u32), simde_mm_load_si128((simde__m128i*)v50.u32)));
	// vmrghw128 v49,v56,v55
	simde_mm_store_si128((simde__m128i*)v49.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v55.u32), simde_mm_load_si128((simde__m128i*)v56.u32)));
	// lvx128 v42,r7,r6
	ea = (l_r7.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v42.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrghw128 v44,v54,v48
	simde_mm_store_si128((simde__m128i*)v44.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v48.u32), simde_mm_load_si128((simde__m128i*)v54.u32)));
	// lvx128 v0,r9,r6
	ea = (l_r9.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v0.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v13,r4,r6
	ea = (l_r4.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)l_v13.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrglw128 v56,v56,v55
	simde_mm_store_si128((simde__m128i*)v56.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v55.u32), simde_mm_load_si128((simde__m128i*)v56.u32)));
	// lvx128 v102,r8,r6
	ea = (l_r8.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v102.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrghw128 v100,v0,v42
	simde_mm_store_si128((simde__m128i*)v100.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v42.u32), simde_mm_load_si128((simde__m128i*)l_v0.u32)));
	// vmrghw128 v5,v57,v49
	simde_mm_store_si128((simde__m128i*)l_v5.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v49.u32), simde_mm_load_si128((simde__m128i*)v57.u32)));
	// lvx128 v101,r30,r6
	ea = (r30.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v101.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrghw128 v4,v44,v43
	simde_mm_store_si128((simde__m128i*)l_v4.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v43.u32), simde_mm_load_si128((simde__m128i*)v44.u32)));
	// lvx128 v99,r3,r6
	ea = (l_r3.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v99.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// lvx128 v58,r31,r6
	ea = (r31.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v58.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrglw128 v55,v50,v47
	simde_mm_store_si128((simde__m128i*)v55.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v47.u32), simde_mm_load_si128((simde__m128i*)v50.u32)));
	// lvx128 v97,r29,r6
	ea = (r29.u32 + l_r6.u32) & ~0xF;
	simde_mm_store_si128((simde__m128i*)v97.u8, REX_BSWAP128(simde_mm_load_si128((simde__m128i*)REX_RAW_ADDR(ea))));
	// vmrglw128 v54,v54,v48
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v48.u32), simde_mm_load_si128((simde__m128i*)v54.u32)));
	// vmrghw128 v98,v102,v13
	simde_mm_store_si128((simde__m128i*)v98.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)l_v13.u32), simde_mm_load_si128((simde__m128i*)v102.u32)));
	// vor128 v47,v6,v6
	simde_mm_store_si128((simde__m128i*)v47.u8, simde_mm_load_si128((simde__m128i*)l_v6.u8));
	// vsubfp128 v96,v5,v4
	ctx.fpscr.enableFlushMode();
	simde_mm_store_ps(v96.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v5.f32), simde_mm_load_ps(l_v4.f32)));
	// vmrghw128 v95,v99,v101
	simde_mm_store_si128((simde__m128i*)v95.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v101.u32), simde_mm_load_si128((simde__m128i*)v99.u32)));
	// vmrghw128 v94,v58,v97
	simde_mm_store_si128((simde__m128i*)v94.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v97.u32), simde_mm_load_si128((simde__m128i*)v58.u32)));
	// add r23,r7,r6
	r23.u64 = l_r7.u64 + l_r6.u64;
	// vmrglw128 v1,v44,v43
	simde_mm_store_si128((simde__m128i*)l_v1.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v43.u32), simde_mm_load_si128((simde__m128i*)v44.u32)));
	// add r23,r4,r6
	r23.u64 = l_r4.u64 + l_r6.u64;
	// vmrglw128 v3,v57,v49
	simde_mm_store_si128((simde__m128i*)l_v3.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v49.u32), simde_mm_load_si128((simde__m128i*)v57.u32)));
	// add r23,r3,r5
	r23.u64 = l_r3.u64 + l_r5.u64;
	// vmrghw128 v2,v59,v56
	simde_mm_store_si128((simde__m128i*)l_v2.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v56.u32), simde_mm_load_si128((simde__m128i*)v59.u32)));
	// add r23,r31,r5
	r23.u64 = r31.u64 + l_r5.u64;
	// vmrghw128 v31,v54,v55
	simde_mm_store_si128((simde__m128i*)v31.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v55.u32), simde_mm_load_si128((simde__m128i*)v54.u32)));
	// add r23,r30,r5
	r23.u64 = r30.u64 + l_r5.u64;
	// vmrghw128 v59,v100,v98
	simde_mm_store_si128((simde__m128i*)v59.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v98.u32), simde_mm_load_si128((simde__m128i*)v100.u32)));
	// vor128 v45,v8,v8
	simde_mm_store_si128((simde__m128i*)v45.u8, simde_mm_load_si128((simde__m128i*)l_v8.u8));
	// vmrghw128 v56,v95,v94
	simde_mm_store_si128((simde__m128i*)v56.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v94.u32), simde_mm_load_si128((simde__m128i*)v95.u32)));
	// vor128 v44,v11,v11
	simde_mm_store_si128((simde__m128i*)v44.u8, simde_mm_load_si128((simde__m128i*)l_v11.u8));
	// vmrglw128 v54,v0,v42
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v42.u32), simde_mm_load_si128((simde__m128i*)l_v0.u32)));
	// vsubfp v0,v3,v1
	simde_mm_store_ps(l_v0.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v3.f32), simde_mm_load_ps(l_v1.f32)));
	// vmrglw128 v42,v99,v101
	simde_mm_store_si128((simde__m128i*)v42.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v101.u32), simde_mm_load_si128((simde__m128i*)v99.u32)));
	// vmulfp128 v101,v31,v7
	simde_mm_store_ps(v101.f32, simde_mm_mul_ps(simde_mm_load_ps(v31.f32), simde_mm_load_ps(l_v7.f32)));
	// vmrglw128 v57,v102,v13
	simde_mm_store_si128((simde__m128i*)v57.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)l_v13.u32), simde_mm_load_si128((simde__m128i*)v102.u32)));
	// vmulfp128 v102,v2,v10
	simde_mm_store_ps(v102.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v2.f32), simde_mm_load_ps(l_v10.f32)));
	// vsubfp v13,v2,v31
	simde_mm_store_ps(l_v13.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v2.f32), simde_mm_load_ps(v31.f32)));
	// vmrglw128 v43,v58,v97
	simde_mm_store_si128((simde__m128i*)v43.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v97.u32), simde_mm_load_si128((simde__m128i*)v58.u32)));
	// vmulfp128 v50,v116,v96
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v116.f32), simde_mm_load_ps(v96.f32)));
	// vmrglw128 v58,v100,v98
	simde_mm_store_si128((simde__m128i*)v58.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v98.u32), simde_mm_load_si128((simde__m128i*)v100.u32)));
	// vmulfp128 v49,v28,v96
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(v28.f32), simde_mm_load_ps(v96.f32)));
	// vmrglw128 v55,v95,v94
	simde_mm_store_si128((simde__m128i*)v55.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v94.u32), simde_mm_load_si128((simde__m128i*)v95.u32)));
	// vmulfp128 v48,v27,v96
	simde_mm_store_ps(v48.f32, simde_mm_mul_ps(simde_mm_load_ps(v27.f32), simde_mm_load_ps(v96.f32)));
	// vmrghw128 v57,v54,v57
	simde_mm_store_si128((simde__m128i*)v57.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v57.u32), simde_mm_load_si128((simde__m128i*)v54.u32)));
	// vmaddcfp128 v46,v3,v46,v59
	simde_mm_store_ps(v46.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v3.f32), simde_mm_load_ps(v46.f32), simde_mm_load_ps(v59.f32)));
	// vmrghw128 v54,v42,v43
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v43.u32), simde_mm_load_si128((simde__m128i*)v42.u32)));
	// vmaddcfp128 v47,v1,v47,v56
	simde_mm_store_ps(v47.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v1.f32), simde_mm_load_ps(v47.f32), simde_mm_load_ps(v56.f32)));
	// vor128 v43,v7,v7
	simde_mm_store_si128((simde__m128i*)v43.u8, simde_mm_load_si128((simde__m128i*)l_v7.u8));
	// add r23,r29,r5
	r23.u64 = r29.u64 + l_r5.u64;
	// vmaddcfp128 v45,v31,v45,v55
	simde_mm_store_ps(v45.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v31.f32), simde_mm_load_ps(v45.f32), simde_mm_load_ps(v55.f32)));
	// add r23,r3,r6
	r23.u64 = l_r3.u64 + l_r6.u64;
	// vmaddcfp128 v44,v2,v44,v58
	simde_mm_store_ps(v44.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v2.f32), simde_mm_load_ps(v44.f32), simde_mm_load_ps(v58.f32)));
	// add r23,r31,r6
	r23.u64 = r31.u64 + l_r6.u64;
	// vmulfp128 v100,v4,v6
	simde_mm_store_ps(v100.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v4.f32), simde_mm_load_ps(l_v6.f32)));
	// add r23,r30,r6
	r23.u64 = r30.u64 + l_r6.u64;
	// vor128 v42,v10,v10
	simde_mm_store_si128((simde__m128i*)v42.u8, simde_mm_load_si128((simde__m128i*)l_v10.u8));
	// add r23,r29,r6
	r23.u64 = r29.u64 + l_r6.u64;
	// vmulfp128 v99,v5,v9
	simde_mm_store_ps(v99.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v5.f32), simde_mm_load_ps(l_v9.f32)));
	// vmaddcfp128 v43,v4,v43,v54
	simde_mm_store_ps(v43.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v4.f32), simde_mm_load_ps(v43.f32), simde_mm_load_ps(v54.f32)));
	// vmaddfp128 v50,v12,v0,v50
	simde_mm_store_ps(v50.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v50.f32)));
	// vmaddfp128 v49,v26,v0,v49
	simde_mm_store_ps(v49.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v26.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v49.f32)));
	// vmaddcfp128 v0,v25,v0,v48
	simde_mm_store_ps(l_v0.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v25.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v48.f32)));
	// vor128 v48,v13,v13
	simde_mm_store_si128((simde__m128i*)v48.u8, simde_mm_load_si128((simde__m128i*)l_v13.u8));
	// vsubfp128 v46,v46,v102
	simde_mm_store_ps(v46.f32, simde_mm_sub_ps(simde_mm_load_ps(v46.f32), simde_mm_load_ps(v102.f32)));
	// vsubfp128 v47,v47,v101
	simde_mm_store_ps(v47.f32, simde_mm_sub_ps(simde_mm_load_ps(v47.f32), simde_mm_load_ps(v101.f32)));
	// vsubfp128 v44,v44,v99
	simde_mm_store_ps(v44.f32, simde_mm_sub_ps(simde_mm_load_ps(v44.f32), simde_mm_load_ps(v99.f32)));
	// vsubfp128 v45,v45,v100
	simde_mm_store_ps(v45.f32, simde_mm_sub_ps(simde_mm_load_ps(v45.f32), simde_mm_load_ps(v100.f32)));
	// vsubfp128 v100,v46,v47
	simde_mm_store_ps(v100.f32, simde_mm_sub_ps(simde_mm_load_ps(v46.f32), simde_mm_load_ps(v47.f32)));
	// vmaddcfp128 v42,v5,v42,v57
	simde_mm_store_ps(v42.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v5.f32), simde_mm_load_ps(v42.f32), simde_mm_load_ps(v57.f32)));
	// vmulfp128 v102,v1,v8
	simde_mm_store_ps(v102.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v1.f32), simde_mm_load_ps(l_v8.f32)));
	// vmulfp128 v101,v3,v11
	simde_mm_store_ps(v101.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v3.f32), simde_mm_load_ps(l_v11.f32)));
	// vmaddfp128 v50,v29,v13,v50
	simde_mm_store_ps(v50.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v29.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v50.f32)));
	// vmaddcfp128 v48,v23,v48,v0
	simde_mm_store_ps(v48.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v23.f32), simde_mm_load_ps(v48.f32), simde_mm_load_ps(l_v0.f32)));
	// vmulfp128 v122,v63,v122
	simde_mm_store_ps(v122.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v122.f32)));
	// vmaddfp128 v49,v24,v13,v49
	simde_mm_store_ps(v49.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v24.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v49.f32)));
	// vmulfp128 v121,v63,v121
	simde_mm_store_ps(v121.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v121.f32)));
	// vmulfp128 v13,v63,v120
	simde_mm_store_ps(l_v13.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v120.f32)));
	// vmulfp128 v120,v63,v119
	simde_mm_store_ps(v120.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v119.f32)));
	// vsubfp128 v0,v44,v45
	simde_mm_store_ps(l_v0.f32, simde_mm_sub_ps(simde_mm_load_ps(v44.f32), simde_mm_load_ps(v45.f32)));
	// vmulfp128 v47,v115,v100
	simde_mm_store_ps(v47.f32, simde_mm_mul_ps(simde_mm_load_ps(v115.f32), simde_mm_load_ps(v100.f32)));
	// vmulfp128 v46,v22,v100
	simde_mm_store_ps(v46.f32, simde_mm_mul_ps(simde_mm_load_ps(v22.f32), simde_mm_load_ps(v100.f32)));
	// vsubfp128 v43,v43,v102
	simde_mm_store_ps(v43.f32, simde_mm_sub_ps(simde_mm_load_ps(v43.f32), simde_mm_load_ps(v102.f32)));
	// vsubfp128 v42,v42,v101
	simde_mm_store_ps(v42.f32, simde_mm_sub_ps(simde_mm_load_ps(v42.f32), simde_mm_load_ps(v101.f32)));
	// vmulfp128 v44,v63,v32
	simde_mm_store_ps(v44.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v32.f32)));
	// vmulfp128 v45,v21,v100
	simde_mm_store_ps(v45.f32, simde_mm_mul_ps(simde_mm_load_ps(v21.f32), simde_mm_load_ps(v100.f32)));
	// vmulfp128 v32,v122,v50
	simde_mm_store_ps(v32.f32, simde_mm_mul_ps(simde_mm_load_ps(v122.f32), simde_mm_load_ps(v50.f32)));
	// vmulfp128 v119,v63,v118
	simde_mm_store_ps(v119.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v118.f32)));
	// vmulfp128 v122,v121,v49
	simde_mm_store_ps(v122.f32, simde_mm_mul_ps(simde_mm_load_ps(v121.f32), simde_mm_load_ps(v49.f32)));
	// vmulfp128 v48,v13,v48
	simde_mm_store_ps(v48.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v48.f32)));
	// vmulfp128 v118,v63,v117
	simde_mm_store_ps(v118.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v117.f32)));
	// vor128 v50,v0,v0
	simde_mm_store_si128((simde__m128i*)v50.u8, simde_mm_load_si128((simde__m128i*)l_v0.u8));
	// vmulfp128 v127,v63,v127
	simde_mm_store_ps(v127.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v127.f32)));
	// vor128 v49,v0,v0
	simde_mm_store_si128((simde__m128i*)v49.u8, simde_mm_load_si128((simde__m128i*)l_v0.u8));
	// vmaddcfp128 v50,v20,v50,v47
	simde_mm_store_ps(v50.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v20.f32), simde_mm_load_ps(v50.f32), simde_mm_load_ps(v47.f32)));
	// vsubfp128 v13,v42,v43
	simde_mm_store_ps(l_v13.f32, simde_mm_sub_ps(simde_mm_load_ps(v42.f32), simde_mm_load_ps(v43.f32)));
	// vmaddcfp128 v49,v19,v49,v46
	simde_mm_store_ps(v49.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v19.f32), simde_mm_load_ps(v49.f32), simde_mm_load_ps(v46.f32)));
	// vmaddcfp128 v0,v18,v0,v45
	simde_mm_store_ps(l_v0.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v18.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v45.f32)));
	// vsubfp128 v47,v108,v32
	simde_mm_store_ps(v47.f32, simde_mm_sub_ps(simde_mm_load_ps(v108.f32), simde_mm_load_ps(v32.f32)));
	// vsubfp128 v46,v107,v122
	simde_mm_store_ps(v46.f32, simde_mm_sub_ps(simde_mm_load_ps(v107.f32), simde_mm_load_ps(v122.f32)));
	// vsubfp128 v48,v106,v48
	simde_mm_store_ps(v48.f32, simde_mm_sub_ps(simde_mm_load_ps(v106.f32), simde_mm_load_ps(v48.f32)));
	// vmulfp128 v43,v63,v126
	simde_mm_store_ps(v43.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v126.f32)));
	// vmulfp128 v42,v63,v125
	simde_mm_store_ps(v42.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v125.f32)));
	// vmulfp128 v126,v63,v124
	simde_mm_store_ps(v126.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v124.f32)));
	// vsubfp128 v124,v62,v38
	simde_mm_store_ps(v124.f32, simde_mm_sub_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v38.f32)));
	// vsubfp128 v45,v62,v36
	simde_mm_store_ps(v45.f32, simde_mm_sub_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v36.f32)));
	// vmulfp128 v125,v63,v123
	simde_mm_store_ps(v125.f32, simde_mm_mul_ps(simde_mm_load_ps(v63.f32), simde_mm_load_ps(v123.f32)));
	// vmaddfp128 v50,v17,v13,v50
	simde_mm_store_ps(v50.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v17.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v50.f32)));
	// vmaddfp128 v49,v16,v13,v49
	simde_mm_store_ps(v49.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v16.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v49.f32)));
	// vmaddfp v0,v15,v13,v0
	simde_mm_store_ps(l_v0.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v15.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(l_v0.f32)));
	// vmulfp128 v47,v41,v47
	simde_mm_store_ps(v47.f32, simde_mm_mul_ps(simde_mm_load_ps(v41.f32), simde_mm_load_ps(v47.f32)));
	// vmulfp128 v46,v41,v46
	simde_mm_store_ps(v46.f32, simde_mm_mul_ps(simde_mm_load_ps(v41.f32), simde_mm_load_ps(v46.f32)));
	// vmulfp128 v48,v41,v48
	simde_mm_store_ps(v48.f32, simde_mm_mul_ps(simde_mm_load_ps(v41.f32), simde_mm_load_ps(v48.f32)));
	// vsubfp128 v123,v62,v37
	simde_mm_store_ps(v123.f32, simde_mm_sub_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v37.f32)));
	// vmulfp128 v50,v120,v50
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v120.f32), simde_mm_load_ps(v50.f32)));
	// vmulfp128 v49,v119,v49
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(v119.f32), simde_mm_load_ps(v49.f32)));
	// vmulfp128 v41,v118,v0
	simde_mm_store_ps(v41.f32, simde_mm_mul_ps(simde_mm_load_ps(v118.f32), simde_mm_load_ps(l_v0.f32)));
	// vaddfp128 v47,v44,v47
	simde_mm_store_ps(v47.f32, simde_mm_add_ps(simde_mm_load_ps(v44.f32), simde_mm_load_ps(v47.f32)));
	// vaddfp128 v46,v127,v46
	simde_mm_store_ps(v46.f32, simde_mm_add_ps(simde_mm_load_ps(v127.f32), simde_mm_load_ps(v46.f32)));
	// vaddfp128 v48,v43,v48
	simde_mm_store_ps(v48.f32, simde_mm_add_ps(simde_mm_load_ps(v43.f32), simde_mm_load_ps(v48.f32)));
	// vsubfp128 v43,v62,v34
	simde_mm_store_ps(v43.f32, simde_mm_sub_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v34.f32)));
	// vsubfp128 v50,v105,v50
	simde_mm_store_ps(v50.f32, simde_mm_sub_ps(simde_mm_load_ps(v105.f32), simde_mm_load_ps(v50.f32)));
	// vsubfp128 v49,v104,v49
	simde_mm_store_ps(v49.f32, simde_mm_sub_ps(simde_mm_load_ps(v104.f32), simde_mm_load_ps(v49.f32)));
	// vsubfp128 v44,v103,v41
	simde_mm_store_ps(v44.f32, simde_mm_sub_ps(simde_mm_load_ps(v103.f32), simde_mm_load_ps(v41.f32)));
	// vminfp128 v47,v47,v38
	simde_mm_store_ps(v47.f32, simde_mm_min_ps(simde_mm_load_ps(v47.f32), simde_mm_load_ps(v38.f32)));
	// vminfp128 v46,v46,v37
	simde_mm_store_ps(v46.f32, simde_mm_min_ps(simde_mm_load_ps(v46.f32), simde_mm_load_ps(v37.f32)));
	// vminfp128 v48,v48,v36
	simde_mm_store_ps(v48.f32, simde_mm_min_ps(simde_mm_load_ps(v48.f32), simde_mm_load_ps(v36.f32)));
	// vmulfp128 v50,v40,v50
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v40.f32), simde_mm_load_ps(v50.f32)));
	// vmulfp128 v49,v40,v49
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(v40.f32), simde_mm_load_ps(v49.f32)));
	// vmulfp128 v44,v40,v44
	simde_mm_store_ps(v44.f32, simde_mm_mul_ps(simde_mm_load_ps(v40.f32), simde_mm_load_ps(v44.f32)));
	// vmaxfp128 v47,v124,v47
	simde_mm_store_ps(v47.f32, simde_mm_max_ps(simde_mm_load_ps(v124.f32), simde_mm_load_ps(v47.f32)));
	// vmaxfp128 v46,v123,v46
	simde_mm_store_ps(v46.f32, simde_mm_max_ps(simde_mm_load_ps(v123.f32), simde_mm_load_ps(v46.f32)));
	// vmaxfp128 v45,v45,v48
	simde_mm_store_ps(v45.f32, simde_mm_max_ps(simde_mm_load_ps(v45.f32), simde_mm_load_ps(v48.f32)));
	// vsubfp128 v40,v62,v33
	simde_mm_store_ps(v40.f32, simde_mm_sub_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v33.f32)));
	// vaddfp128 v50,v42,v50
	simde_mm_store_ps(v50.f32, simde_mm_add_ps(simde_mm_load_ps(v42.f32), simde_mm_load_ps(v50.f32)));
	// vaddfp128 v49,v126,v49
	simde_mm_store_ps(v49.f32, simde_mm_add_ps(simde_mm_load_ps(v126.f32), simde_mm_load_ps(v49.f32)));
	// vaddfp128 v48,v125,v44
	simde_mm_store_ps(v48.f32, simde_mm_add_ps(simde_mm_load_ps(v125.f32), simde_mm_load_ps(v44.f32)));
	// vsubfp128 v44,v62,v35
	simde_mm_store_ps(v44.f32, simde_mm_sub_ps(simde_mm_load_ps(v62.f32), simde_mm_load_ps(v35.f32)));
	// vsubfp128 v42,v47,v108
	simde_mm_store_ps(v42.f32, simde_mm_sub_ps(simde_mm_load_ps(v47.f32), simde_mm_load_ps(v108.f32)));
	// vsubfp128 v0,v46,v107
	simde_mm_store_ps(l_v0.f32, simde_mm_sub_ps(simde_mm_load_ps(v46.f32), simde_mm_load_ps(v107.f32)));
	// vsubfp128 v13,v45,v106
	simde_mm_store_ps(l_v13.f32, simde_mm_sub_ps(simde_mm_load_ps(v45.f32), simde_mm_load_ps(v106.f32)));
	// vminfp128 v50,v50,v35
	simde_mm_store_ps(v50.f32, simde_mm_min_ps(simde_mm_load_ps(v50.f32), simde_mm_load_ps(v35.f32)));
	// vminfp128 v49,v49,v34
	simde_mm_store_ps(v49.f32, simde_mm_min_ps(simde_mm_load_ps(v49.f32), simde_mm_load_ps(v34.f32)));
	// vminfp128 v41,v48,v33
	simde_mm_store_ps(v41.f32, simde_mm_min_ps(simde_mm_load_ps(v48.f32), simde_mm_load_ps(v33.f32)));
	// vmaxfp128 v44,v44,v50
	simde_mm_store_ps(v44.f32, simde_mm_max_ps(simde_mm_load_ps(v44.f32), simde_mm_load_ps(v50.f32)));
	// vmaxfp128 v43,v43,v49
	simde_mm_store_ps(v43.f32, simde_mm_max_ps(simde_mm_load_ps(v43.f32), simde_mm_load_ps(v49.f32)));
	// vmulfp128 v50,v116,v42
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v116.f32), simde_mm_load_ps(v42.f32)));
	// vmulfp128 v49,v12,v42
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(v42.f32)));
	// vmulfp128 v48,v29,v42
	simde_mm_store_ps(v48.f32, simde_mm_mul_ps(simde_mm_load_ps(v29.f32), simde_mm_load_ps(v42.f32)));
	// vmaxfp128 v42,v40,v41
	simde_mm_store_ps(v42.f32, simde_mm_max_ps(simde_mm_load_ps(v40.f32), simde_mm_load_ps(v41.f32)));
	// vsubfp128 v41,v44,v105
	simde_mm_store_ps(v41.f32, simde_mm_sub_ps(simde_mm_load_ps(v44.f32), simde_mm_load_ps(v105.f32)));
	// vsubfp128 v12,v43,v104
	simde_mm_store_ps(l_v12.f32, simde_mm_sub_ps(simde_mm_load_ps(v43.f32), simde_mm_load_ps(v104.f32)));
	// vmaddcfp128 v28,v0,v28,v50
	simde_mm_store_ps(v28.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v28.f32), simde_mm_load_ps(v50.f32)));
	// vmaddcfp128 v26,v0,v26,v49
	simde_mm_store_ps(v26.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v26.f32), simde_mm_load_ps(v49.f32)));
	// vmaddcfp128 v0,v24,v0,v48
	simde_mm_store_ps(l_v0.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v24.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v48.f32)));
	// vsubfp128 v29,v42,v103
	simde_mm_store_ps(v29.f32, simde_mm_sub_ps(simde_mm_load_ps(v42.f32), simde_mm_load_ps(v103.f32)));
	// vmulfp128 v17,v17,v41
	simde_mm_store_ps(v17.f32, simde_mm_mul_ps(simde_mm_load_ps(v17.f32), simde_mm_load_ps(v41.f32)));
	// vmulfp128 v20,v20,v41
	simde_mm_store_ps(v20.f32, simde_mm_mul_ps(simde_mm_load_ps(v20.f32), simde_mm_load_ps(v41.f32)));
	// vmulfp128 v50,v115,v41
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v115.f32), simde_mm_load_ps(v41.f32)));
	// vmaddfp v28,v27,v13,v28
	simde_mm_store_ps(v28.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v27.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v28.f32)));
	// vmaddfp v27,v25,v13,v26
	simde_mm_store_ps(v27.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v25.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v26.f32)));
	// vmaddfp v26,v23,v13,v0
	simde_mm_store_ps(v26.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v23.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(l_v0.f32)));
	// vmaddfp v24,v16,v12,v17
	simde_mm_store_ps(v24.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v16.f32), simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(v17.f32)));
	// vmaddfp v20,v19,v12,v20
	simde_mm_store_ps(v20.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v19.f32), simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(v20.f32)));
	// vmaddcfp128 v12,v22,v12,v50
	simde_mm_store_ps(l_v12.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v22.f32), simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(v50.f32)));
	// vmaddfp v0,v15,v29,v24
	simde_mm_store_ps(l_v0.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v15.f32), simde_mm_load_ps(v29.f32), simde_mm_load_ps(v24.f32)));
	// vmaddfp v13,v18,v29,v20
	simde_mm_store_ps(l_v13.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v18.f32), simde_mm_load_ps(v29.f32), simde_mm_load_ps(v20.f32)));
	// vmaddfp v12,v21,v29,v12
	simde_mm_store_ps(l_v12.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v21.f32), simde_mm_load_ps(v29.f32), simde_mm_load_ps(l_v12.f32)));
	// vmaddfp v29,v7,v0,v28
	simde_mm_store_ps(v29.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v7.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v28.f32)));
	// vmulfp128 v50,v6,v13
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v6.f32), simde_mm_load_ps(l_v13.f32)));
	// vmulfp128 v41,v7,v12
	simde_mm_store_ps(v41.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v7.f32), simde_mm_load_ps(l_v12.f32)));
	// vor v7,v12,v12
	simde_mm_store_si128((simde__m128i*)l_v7.u8, simde_mm_load_si128((simde__m128i*)l_v12.u8));
	// vmulfp128 v48,v9,v13
	simde_mm_store_ps(v48.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v9.f32), simde_mm_load_ps(l_v13.f32)));
	// vmaddfp v25,v10,v0,v28
	simde_mm_store_ps(v25.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v10.f32), simde_mm_load_ps(l_v0.f32), simde_mm_load_ps(v28.f32)));
	// vmulfp128 v49,v8,v0
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v8.f32), simde_mm_load_ps(l_v0.f32)));
	// vmaddcfp128 v7,v30,v7,v59
	simde_mm_store_ps(l_v7.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v30.f32), simde_mm_load_ps(l_v7.f32), simde_mm_load_ps(v59.f32)));
	// vmaddfp v28,v6,v12,v27
	simde_mm_store_ps(v28.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v6.f32), simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(v27.f32)));
	// vor v6,v0,v0
	simde_mm_store_si128((simde__m128i*)l_v6.u8, simde_mm_load_si128((simde__m128i*)l_v0.u8));
	// vmulfp128 v40,v11,v0
	simde_mm_store_ps(v40.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v11.f32), simde_mm_load_ps(l_v0.f32)));
	// vmaddfp v9,v9,v12,v27
	simde_mm_store_ps(l_v9.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v9.f32), simde_mm_load_ps(l_v12.f32), simde_mm_load_ps(v27.f32)));
	// vmulfp128 v38,v10,v12
	simde_mm_store_ps(v38.f32, simde_mm_mul_ps(simde_mm_load_ps(l_v10.f32), simde_mm_load_ps(l_v12.f32)));
	// vmaddfp v27,v11,v13,v26
	simde_mm_store_ps(v27.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v11.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v26.f32)));
	// vor v11,v13,v13
	simde_mm_store_si128((simde__m128i*)l_v11.u8, simde_mm_load_si128((simde__m128i*)l_v13.u8));
	// vmaddfp v8,v8,v13,v26
	simde_mm_store_ps(l_v8.f32, simde_mm_fmadd_ps(simde_mm_load_ps(l_v8.f32), simde_mm_load_ps(l_v13.f32), simde_mm_load_ps(v26.f32)));
	// vsubfp128 v59,v29,v50
	simde_mm_store_ps(v59.f32, simde_mm_sub_ps(simde_mm_load_ps(v29.f32), simde_mm_load_ps(v50.f32)));
	// vmaddcfp128 v6,v30,v6,v57
	simde_mm_store_ps(l_v6.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v30.f32), simde_mm_load_ps(l_v6.f32), simde_mm_load_ps(v57.f32)));
	// vmaddcfp128 v11,v30,v11,v58
	simde_mm_store_ps(l_v11.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v30.f32), simde_mm_load_ps(l_v11.f32), simde_mm_load_ps(v58.f32)));
	// vsubfp128 v10,v25,v48
	simde_mm_store_ps(l_v10.f32, simde_mm_sub_ps(simde_mm_load_ps(v25.f32), simde_mm_load_ps(v48.f32)));
	// vsubfp128 v58,v28,v49
	simde_mm_store_ps(v58.f32, simde_mm_sub_ps(simde_mm_load_ps(v28.f32), simde_mm_load_ps(v49.f32)));
	// vsubfp128 v9,v9,v40
	simde_mm_store_ps(l_v9.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v9.f32), simde_mm_load_ps(v40.f32)));
	// vsubfp128 v57,v8,v41
	simde_mm_store_ps(v57.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v8.f32), simde_mm_load_ps(v41.f32)));
	// vmulfp128 v50,v114,v59
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v114.f32), simde_mm_load_ps(v59.f32)));
	// vmulfp128 v49,v113,v59
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(v113.f32), simde_mm_load_ps(v59.f32)));
	// vmulfp128 v59,v112,v59
	simde_mm_store_ps(v59.f32, simde_mm_mul_ps(simde_mm_load_ps(v112.f32), simde_mm_load_ps(v59.f32)));
	// vmaddfp v3,v14,v10,v3
	simde_mm_store_ps(l_v3.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v14.f32), simde_mm_load_ps(l_v10.f32), simde_mm_load_ps(l_v3.f32)));
	// vmaddfp128 v5,v53,v10,v5
	simde_mm_store_ps(l_v5.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v53.f32), simde_mm_load_ps(l_v10.f32), simde_mm_load_ps(l_v5.f32)));
	// vmaddcfp128 v10,v61,v10,v2
	simde_mm_store_ps(l_v10.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v61.f32), simde_mm_load_ps(l_v10.f32), simde_mm_load_ps(l_v2.f32)));
	// vmulfp128 v48,v113,v58
	simde_mm_store_ps(v48.f32, simde_mm_mul_ps(simde_mm_load_ps(v113.f32), simde_mm_load_ps(v58.f32)));
	// vsubfp128 v8,v27,v38
	simde_mm_store_ps(l_v8.f32, simde_mm_sub_ps(simde_mm_load_ps(v27.f32), simde_mm_load_ps(v38.f32)));
	// vmulfp128 v53,v111,v58
	simde_mm_store_ps(v53.f32, simde_mm_mul_ps(simde_mm_load_ps(v111.f32), simde_mm_load_ps(v58.f32)));
	// vmulfp128 v58,v110,v58
	simde_mm_store_ps(v58.f32, simde_mm_mul_ps(simde_mm_load_ps(v110.f32), simde_mm_load_ps(v58.f32)));
	// vmulfp128 v41,v112,v57
	simde_mm_store_ps(v41.f32, simde_mm_mul_ps(simde_mm_load_ps(v112.f32), simde_mm_load_ps(v57.f32)));
	// vsubfp128 v50,v4,v50
	simde_mm_store_ps(v50.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v4.f32), simde_mm_load_ps(v50.f32)));
	// vor v4,v9,v9
	simde_mm_store_si128((simde__m128i*)l_v4.u8, simde_mm_load_si128((simde__m128i*)l_v9.u8));
	// vsubfp128 v49,v1,v49
	simde_mm_store_ps(v49.f32, simde_mm_sub_ps(simde_mm_load_ps(l_v1.f32), simde_mm_load_ps(v49.f32)));
	// vsubfp128 v59,v31,v59
	simde_mm_store_ps(v59.f32, simde_mm_sub_ps(simde_mm_load_ps(v31.f32), simde_mm_load_ps(v59.f32)));
	// vmulfp128 v40,v110,v57
	simde_mm_store_ps(v40.f32, simde_mm_mul_ps(simde_mm_load_ps(v110.f32), simde_mm_load_ps(v57.f32)));
	// vmaddcfp128 v4,v52,v4,v3
	simde_mm_store_ps(l_v4.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v52.f32), simde_mm_load_ps(l_v4.f32), simde_mm_load_ps(l_v3.f32)));
	// vor v3,v9,v9
	simde_mm_store_si128((simde__m128i*)l_v3.u8, simde_mm_load_si128((simde__m128i*)l_v9.u8));
	// vmaddfp v5,v14,v9,v5
	simde_mm_store_ps(l_v5.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v14.f32), simde_mm_load_ps(l_v9.f32), simde_mm_load_ps(l_v5.f32)));
	// vmulfp128 v57,v109,v57
	simde_mm_store_ps(v57.f32, simde_mm_mul_ps(simde_mm_load_ps(v109.f32), simde_mm_load_ps(v57.f32)));
	// vor v9,v8,v8
	simde_mm_store_si128((simde__m128i*)l_v9.u8, simde_mm_load_si128((simde__m128i*)l_v8.u8));
	// vmaddcfp128 v3,v60,v3,v10
	simde_mm_store_ps(l_v3.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v60.f32), simde_mm_load_ps(l_v3.f32), simde_mm_load_ps(l_v10.f32)));
	// vor v10,v8,v8
	simde_mm_store_si128((simde__m128i*)l_v10.u8, simde_mm_load_si128((simde__m128i*)l_v8.u8));
	// vsubfp128 v52,v50,v48
	simde_mm_store_ps(v52.f32, simde_mm_sub_ps(simde_mm_load_ps(v50.f32), simde_mm_load_ps(v48.f32)));
	// vmrghw128 v48,v7,v6
	simde_mm_store_si128((simde__m128i*)v48.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)l_v6.u32), simde_mm_load_si128((simde__m128i*)l_v7.u32)));
	// vsubfp128 v53,v49,v53
	simde_mm_store_ps(v53.f32, simde_mm_sub_ps(simde_mm_load_ps(v49.f32), simde_mm_load_ps(v53.f32)));
	// vsubfp128 v59,v59,v58
	simde_mm_store_ps(v59.f32, simde_mm_sub_ps(simde_mm_load_ps(v59.f32), simde_mm_load_ps(v58.f32)));
	// vmulfp128 v50,v39,v13
	simde_mm_store_ps(v50.f32, simde_mm_mul_ps(simde_mm_load_ps(v39.f32), simde_mm_load_ps(l_v13.f32)));
	// vmaddcfp128 v10,v60,v10,v4
	simde_mm_store_ps(l_v10.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v60.f32), simde_mm_load_ps(l_v10.f32), simde_mm_load_ps(l_v4.f32)));
	// vmrglw128 v60,v11,v11
	simde_mm_store_si128((simde__m128i*)v60.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)l_v11.u32), simde_mm_load_si128((simde__m128i*)l_v11.u32)));
	// vmaddcfp128 v9,v61,v9,v5
	simde_mm_store_ps(l_v9.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v61.f32), simde_mm_load_ps(l_v9.f32), simde_mm_load_ps(l_v5.f32)));
	// vmrglw128 v61,v7,v6
	simde_mm_store_si128((simde__m128i*)v61.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)l_v6.u32), simde_mm_load_si128((simde__m128i*)l_v7.u32)));
	// vmulfp128 v49,v39,v0
	simde_mm_store_ps(v49.f32, simde_mm_mul_ps(simde_mm_load_ps(v39.f32), simde_mm_load_ps(l_v0.f32)));
	// vmaddcfp128 v8,v51,v8,v3
	simde_mm_store_ps(l_v8.f32, simde_mm_fmadd_ps(simde_mm_load_ps(v51.f32), simde_mm_load_ps(l_v8.f32), simde_mm_load_ps(l_v3.f32)));
	// vmrghw128 v51,v11,v11
	simde_mm_store_si128((simde__m128i*)v51.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)l_v11.u32), simde_mm_load_si128((simde__m128i*)l_v11.u32)));
	// vsubfp128 v58,v52,v41
	simde_mm_store_ps(v58.f32, simde_mm_sub_ps(simde_mm_load_ps(v52.f32), simde_mm_load_ps(v41.f32)));
	// vmulfp128 v52,v39,v12
	simde_mm_store_ps(v52.f32, simde_mm_mul_ps(simde_mm_load_ps(v39.f32), simde_mm_load_ps(l_v12.f32)));
	// vsubfp128 v59,v59,v57
	simde_mm_store_ps(v59.f32, simde_mm_sub_ps(simde_mm_load_ps(v59.f32), simde_mm_load_ps(v57.f32)));
	// vmrghw128 v57,v48,v51
	simde_mm_store_si128((simde__m128i*)v57.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v51.u32), simde_mm_load_si128((simde__m128i*)v48.u32)));
	// vmrglw128 v51,v48,v51
	simde_mm_store_si128((simde__m128i*)v51.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v51.u32), simde_mm_load_si128((simde__m128i*)v48.u32)));
	// vsubfp128 v53,v53,v40
	simde_mm_store_ps(v53.f32, simde_mm_sub_ps(simde_mm_load_ps(v53.f32), simde_mm_load_ps(v40.f32)));
	// vmrghw128 v48,v61,v60
	simde_mm_store_si128((simde__m128i*)v48.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v60.u32), simde_mm_load_si128((simde__m128i*)v61.u32)));
	// stvx128 v47,r0,r28
	ea = (r28.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v47.u8)));
	// vmrglw128 v61,v61,v60
	simde_mm_store_si128((simde__m128i*)v61.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v60.u32), simde_mm_load_si128((simde__m128i*)v61.u32)));
	// vsubfp128 v60,v56,v52
	simde_mm_store_ps(v60.f32, simde_mm_sub_ps(simde_mm_load_ps(v56.f32), simde_mm_load_ps(v52.f32)));
	// vsubfp128 v56,v55,v50
	simde_mm_store_ps(v56.f32, simde_mm_sub_ps(simde_mm_load_ps(v55.f32), simde_mm_load_ps(v50.f32)));
	// vmrghw128 v52,v10,v10
	simde_mm_store_si128((simde__m128i*)v52.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)l_v10.u32), simde_mm_load_si128((simde__m128i*)l_v10.u32)));
	// vsubfp128 v55,v54,v49
	simde_mm_store_ps(v55.f32, simde_mm_sub_ps(simde_mm_load_ps(v54.f32), simde_mm_load_ps(v49.f32)));
	// vmrghw128 v54,v9,v8
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)l_v8.u32), simde_mm_load_si128((simde__m128i*)l_v9.u32)));
	// stvx128 v46,r10,r24
	ea = (l_r10.u32 + r24.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v46.u8)));
	// vmrglw128 v49,v10,v10
	simde_mm_store_si128((simde__m128i*)v49.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)l_v10.u32), simde_mm_load_si128((simde__m128i*)l_v10.u32)));
	// vmrglw128 v50,v9,v8
	simde_mm_store_si128((simde__m128i*)v50.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)l_v8.u32), simde_mm_load_si128((simde__m128i*)l_v9.u32)));
	// stvx128 v45,r0,r10
	ea = (l_r10.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v45.u8)));
	// stvx128 v44,r10,r25
	ea = (l_r10.u32 + r25.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v44.u8)));
	// vmrghw128 v46,v54,v52
	simde_mm_store_si128((simde__m128i*)v46.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v52.u32), simde_mm_load_si128((simde__m128i*)v54.u32)));
	// stvx128 v43,r10,r26
	ea = (l_r10.u32 + r26.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v43.u8)));
	// vmrglw128 v54,v54,v52
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v52.u32), simde_mm_load_si128((simde__m128i*)v54.u32)));
	// stvx128 v42,r10,r27
	ea = (l_r10.u32 + r27.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v42.u8)));
	// vmrghw128 v52,v50,v49
	simde_mm_store_si128((simde__m128i*)v52.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v49.u32), simde_mm_load_si128((simde__m128i*)v50.u32)));
	// addi r10,r10,96
	l_r10.s64 = l_r10.s64 + 96;
	// vmrghw128 v47,v58,v59
	simde_mm_store_si128((simde__m128i*)v47.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v59.u32), simde_mm_load_si128((simde__m128i*)v58.u32)));
	// vmrghw128 v45,v53,v53
	simde_mm_store_si128((simde__m128i*)v45.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v53.u32), simde_mm_load_si128((simde__m128i*)v53.u32)));
	// vmrglw128 v50,v50,v49
	simde_mm_store_si128((simde__m128i*)v50.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v49.u32), simde_mm_load_si128((simde__m128i*)v50.u32)));
	// vmrglw128 v59,v58,v59
	simde_mm_store_si128((simde__m128i*)v59.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v59.u32), simde_mm_load_si128((simde__m128i*)v58.u32)));
	// stvx128 v46,r9,r5
	ea = (l_r9.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v46.u8)));
	// vmrglw128 v58,v53,v53
	simde_mm_store_si128((simde__m128i*)v58.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v53.u32), simde_mm_load_si128((simde__m128i*)v53.u32)));
	// stvx128 v54,r8,r5
	ea = (l_r8.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v54.u8)));
	// vmrghw128 v54,v47,v45
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v45.u32), simde_mm_load_si128((simde__m128i*)v47.u32)));
	// stvx128 v52,r7,r5
	ea = (l_r7.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v52.u8)));
	// vmrghw128 v49,v60,v55
	simde_mm_store_si128((simde__m128i*)v49.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v55.u32), simde_mm_load_si128((simde__m128i*)v60.u32)));
	// vmrghw128 v53,v56,v56
	simde_mm_store_si128((simde__m128i*)v53.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v56.u32), simde_mm_load_si128((simde__m128i*)v56.u32)));
	// vmrglw128 v60,v60,v55
	simde_mm_store_si128((simde__m128i*)v60.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v55.u32), simde_mm_load_si128((simde__m128i*)v60.u32)));
	// stvx128 v50,r4,r5
	ea = (l_r4.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v50.u8)));
	// vmrglw128 v56,v56,v56
	simde_mm_store_si128((simde__m128i*)v56.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v56.u32), simde_mm_load_si128((simde__m128i*)v56.u32)));
	// stvx128 v57,r9,r6
	ea = (l_r9.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v57.u8)));
	// vmrglw128 v55,v47,v45
	simde_mm_store_si128((simde__m128i*)v55.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v45.u32), simde_mm_load_si128((simde__m128i*)v47.u32)));
	// stvx128 v51,r8,r6
	ea = (l_r8.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v51.u8)));
	// vmrghw128 v57,v59,v58
	simde_mm_store_si128((simde__m128i*)v57.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v58.u32), simde_mm_load_si128((simde__m128i*)v59.u32)));
	// stvx128 v48,r7,r6
	ea = (l_r7.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v48.u8)));
	// vmrglw128 v59,v59,v58
	simde_mm_store_si128((simde__m128i*)v59.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v58.u32), simde_mm_load_si128((simde__m128i*)v59.u32)));
	// stvx128 v61,r4,r6
	ea = (l_r4.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v61.u8)));
	// vmrghw128 v58,v49,v53
	simde_mm_store_si128((simde__m128i*)v58.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v53.u32), simde_mm_load_si128((simde__m128i*)v49.u32)));
	// stvx128 v54,r3,r5
	ea = (l_r3.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v54.u8)));
	// vmrglw128 v61,v49,v53
	simde_mm_store_si128((simde__m128i*)v61.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v53.u32), simde_mm_load_si128((simde__m128i*)v49.u32)));
	// vmrghw128 v54,v60,v56
	simde_mm_store_si128((simde__m128i*)v54.u32, simde_mm_unpackhi_epi32(simde_mm_load_si128((simde__m128i*)v56.u32), simde_mm_load_si128((simde__m128i*)v60.u32)));
	// vmrglw128 v60,v60,v56
	simde_mm_store_si128((simde__m128i*)v60.u32, simde_mm_unpacklo_epi32(simde_mm_load_si128((simde__m128i*)v56.u32), simde_mm_load_si128((simde__m128i*)v60.u32)));
	// stvx128 v55,r31,r5
	ea = (r31.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v55.u8)));
	// stvx128 v57,r30,r5
	ea = (r30.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v57.u8)));
	// stvx128 v59,r29,r5
	ea = (r29.u32 + l_r5.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v59.u8)));
	// stvx128 v58,r3,r6
	ea = (l_r3.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v58.u8)));
	// stvx128 v61,r31,r6
	ea = (r31.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v61.u8)));
	// stvx128 v54,r30,r6
	ea = (r30.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v54.u8)));
	// stvx128 v60,r29,r6
	ea = (r29.u32 + l_r6.u32) & ~0xF;
	REX_CARD(ea), simde_mm_store_si128((simde__m128i*)REX_RAW_ADDR(ea), REX_BSWAP128(simde_mm_load_si128((simde__m128i*)v60.u8)));
	// bdnz 0x8278917c
	--ctr.u64;
	if (ctr.u32 != 0) goto loc_8278917C;
loc_827898B4:
	// addi r1,r1,736
	l_r1.s64 = l_r1.s64 + 736;
	// addi r12,r1,-160
	r12.s64 = l_r1.s64 + -160;
	// bl 0x82c7251c
	// b 0x8229b390
	{ REX_LEAF_STORE(l_r1, r1); REX_LEAF_STORE(l_r3, r3); REX_LEAF_STORE(l_r4, r4); REX_LEAF_STORE(l_r7, r7); REX_LEAF_STORE(l_r8, r8); REX_LEAF_STORE(l_r9, r9); REX_LEAF_STORE(l_r10, r10); REX_LEAF_STORE(l_v0, v0); REX_LEAF_STORE(l_v1, v1); REX_LEAF_STORE(l_v2, v2); REX_LEAF_STORE(l_v3, v3); REX_LEAF_STORE(l_v4, v4); REX_LEAF_STORE(l_v5, v5); REX_LEAF_STORE(l_v6, v6); REX_LEAF_STORE(l_v7, v7); REX_LEAF_STORE(l_v8, v8); REX_LEAF_STORE(l_v9, v9); REX_LEAF_STORE(l_v10, v10); REX_LEAF_STORE(l_v11, v11); REX_LEAF_STORE(l_v12, v12); REX_LEAF_STORE(l_v13, v13); return; }
	REX_LEAF_STORE(l_r1, r1); REX_LEAF_STORE(l_r3, r3); REX_LEAF_STORE(l_r4, r4); REX_LEAF_STORE(l_r7, r7); REX_LEAF_STORE(l_r8, r8); REX_LEAF_STORE(l_r9, r9); REX_LEAF_STORE(l_r10, r10); REX_LEAF_STORE(l_v0, v0); REX_LEAF_STORE(l_v1, v1); REX_LEAF_STORE(l_v2, v2); REX_LEAF_STORE(l_v3, v3); REX_LEAF_STORE(l_v4, v4); REX_LEAF_STORE(l_v5, v5); REX_LEAF_STORE(l_v6, v6); REX_LEAF_STORE(l_v7, v7); REX_LEAF_STORE(l_v8, v8); REX_LEAF_STORE(l_v9, v9); REX_LEAF_STORE(l_v10, v10); REX_LEAF_STORE(l_v11, v11); REX_LEAF_STORE(l_v12, v12); REX_LEAF_STORE(l_v13, v13);
}
}  // namespace n_82789120

// 7. sub_82A87020: FFT convolution complex MAC (15.0% Job 4 / 2.55% total CPU).
// Vectorizes BE int16 IR dequantization + complex multiply-accumulate in ARM64 NEON (8 complex pairs/iter).
namespace n_82A87020 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

inline int16_t LoadBeS16(const uint8_t* p) noexcept {
  uint16_t raw;
  std::memcpy(&raw, p, 2);
  return int16_t(__builtin_bswap16(raw));
}

inline uint32_t LoadBeU32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  return __builtin_bswap32(raw);
}

// Kept out-of-line so check_switch_build never finds vector FMLA inside sub_82A87020 itself.
[[gnu::noinline]] void ConvDualFast(uint8_t* __restrict y_be, const uint8_t* __restrict a_be,
                                    const uint8_t* __restrict ir0_be, const uint8_t* __restrict ir1_be, int32_t blocks,
                                    float scale0, float scale1) noexcept {
#if defined(__aarch64__)
  const float32x4_t vs0 = vdupq_n_f32(scale0);
  const float32x4_t vs1 = vdupq_n_f32(scale1);
  const uint32_t n_groups = uint32_t(blocks) << 1;
  for (uint32_t g = 0; g < n_groups; ++g) {
    const uint32_t ir_off = g << 5;
    const uint32_t ay_off = g << 6;
    if ((g & 1u) == 0u) {
      __builtin_prefetch(ir0_be + ir_off + 256u, 0, 0);
      __builtin_prefetch(ir1_be + ir_off + 256u, 0, 0);
      __builtin_prefetch(a_be + ay_off + 384u, 0, 0);
      __builtin_prefetch(y_be + ay_off + 384u, 1, 0);
    }
    const int16x8_t p0 = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ir0_be + ir_off)));
    const int16x8_t p1 = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ir0_be + ir_off + 16u)));
    const int16x8_t r0_re16 = vuzp1q_s16(p0, p1);
    const int16x8_t r0_im16 = vuzp2q_s16(p0, p1);

    const int16x8_t q0 = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ir1_be + ir_off)));
    const int16x8_t q1 = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ir1_be + ir_off + 16u)));
    const int16x8_t r1_re16 = vuzp1q_s16(q0, q1);
    const int16x8_t r1_im16 = vuzp2q_s16(q0, q1);

    const float32x4_t h_re0 = vfmaq_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(r0_re16))), vs0),
                                        vcvtq_f32_s32(vmovl_s16(vget_low_s16(r1_re16))), vs1);
    const float32x4_t h_im0 = vfmaq_f32(vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(r0_im16))), vs0),
                                        vcvtq_f32_s32(vmovl_s16(vget_low_s16(r1_im16))), vs1);
    const float32x4_t a0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 0u)));
    const float32x4_t a1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 16u)));
    const float32x4_t a_re0 = vuzp1q_f32(a0, a1);
    const float32x4_t a_im0 = vuzp2q_f32(a0, a1);
    const float32x4_t y0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 0u)));
    const float32x4_t y1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 16u)));
    const float32x4_t prod_re0 = vfmsq_f32(vmulq_f32(h_re0, a_re0), h_im0, a_im0);
    const float32x4_t prod_im0 = vfmaq_f32(vmulq_f32(h_im0, a_re0), h_re0, a_im0);
    vst1q_u8(y_be + ay_off + 0u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y0, vzip1q_f32(prod_re0, prod_im0)))));
    vst1q_u8(y_be + ay_off + 16u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y1, vzip2q_f32(prod_re0, prod_im0)))));

    const float32x4_t h_re1 = vfmaq_f32(vmulq_f32(vcvtq_f32_s32(vmovl_high_s16(r0_re16)), vs0),
                                        vcvtq_f32_s32(vmovl_high_s16(r1_re16)), vs1);
    const float32x4_t h_im1 = vfmaq_f32(vmulq_f32(vcvtq_f32_s32(vmovl_high_s16(r0_im16)), vs0),
                                        vcvtq_f32_s32(vmovl_high_s16(r1_im16)), vs1);
    const float32x4_t a2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 32u)));
    const float32x4_t a3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 48u)));
    const float32x4_t a_re1 = vuzp1q_f32(a2, a3);
    const float32x4_t a_im1 = vuzp2q_f32(a2, a3);
    const float32x4_t y2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 32u)));
    const float32x4_t y3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 48u)));
    const float32x4_t prod_re1 = vfmsq_f32(vmulq_f32(h_re1, a_re1), h_im1, a_im1);
    const float32x4_t prod_im1 = vfmaq_f32(vmulq_f32(h_im1, a_re1), h_re1, a_im1);
    vst1q_u8(y_be + ay_off + 32u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y2, vzip1q_f32(prod_re1, prod_im1)))));
    vst1q_u8(y_be + ay_off + 48u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y3, vzip2q_f32(prod_re1, prod_im1)))));
  }
#else
  const uint32_t pairs = uint32_t(blocks) << 4;
  for (uint32_t k = 0; k < pairs; ++k) {
    const float h_re = std::fmaf(float(LoadBeS16(ir1_be + (k << 2))), scale1, float(LoadBeS16(ir0_be + (k << 2))) * scale0);
    const float h_im = std::fmaf(float(LoadBeS16(ir1_be + (k << 2) + 2u)), scale1, float(LoadBeS16(ir0_be + (k << 2) + 2u)) * scale0);
    const float a_re = LoadBeF32(a_be + (k << 3));
    const float a_im = LoadBeF32(a_be + (k << 3) + 4u);
    const float y_re = LoadBeF32(y_be + (k << 3));
    const float y_im = LoadBeF32(y_be + (k << 3) + 4u);
    StoreBeF32(y_be + (k << 3), y_re + (h_re * a_re - h_im * a_im));
    StoreBeF32(y_be + (k << 3) + 4u, y_im + std::fmaf(h_re, a_im, h_im * a_re));
  }
#endif
}

[[gnu::noinline]] void ConvSingleFast(uint8_t* __restrict y_be, const uint8_t* __restrict a_be,
                                      const uint8_t* __restrict ir_be, int32_t blocks, float scale) noexcept {
#if defined(__aarch64__)
  const float32x4_t vs = vdupq_n_f32(scale);
  const uint32_t n_groups = uint32_t(blocks) << 1;
  for (uint32_t g = 0; g < n_groups; ++g) {
    const uint32_t ir_off = g << 5;
    const uint32_t ay_off = g << 6;
    if ((g & 1u) == 0u) {
      __builtin_prefetch(ir_be + ir_off + 256u, 0, 0);
      __builtin_prefetch(a_be + ay_off + 384u, 0, 0);
      __builtin_prefetch(y_be + ay_off + 384u, 1, 0);
    }
    const int16x8_t p0 = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ir_be + ir_off)));
    const int16x8_t p1 = vreinterpretq_s16_u8(vrev16q_u8(vld1q_u8(ir_be + ir_off + 16u)));
    const int16x8_t r_re16 = vuzp1q_s16(p0, p1);
    const int16x8_t r_im16 = vuzp2q_s16(p0, p1);

    const float32x4_t h_re0 = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(r_re16))), vs);
    const float32x4_t h_im0 = vmulq_f32(vcvtq_f32_s32(vmovl_s16(vget_low_s16(r_im16))), vs);
    const float32x4_t a0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 0u)));
    const float32x4_t a1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 16u)));
    const float32x4_t a_re0 = vuzp1q_f32(a0, a1);
    const float32x4_t a_im0 = vuzp2q_f32(a0, a1);
    const float32x4_t y0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 0u)));
    const float32x4_t y1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 16u)));
    const float32x4_t prod_re0 = vfmsq_f32(vmulq_f32(h_re0, a_re0), h_im0, a_im0);
    const float32x4_t prod_im0 = vfmaq_f32(vmulq_f32(h_im0, a_re0), h_re0, a_im0);
    vst1q_u8(y_be + ay_off + 0u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y0, vzip1q_f32(prod_re0, prod_im0)))));
    vst1q_u8(y_be + ay_off + 16u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y1, vzip2q_f32(prod_re0, prod_im0)))));

    const float32x4_t h_re1 = vmulq_f32(vcvtq_f32_s32(vmovl_high_s16(r_re16)), vs);
    const float32x4_t h_im1 = vmulq_f32(vcvtq_f32_s32(vmovl_high_s16(r_im16)), vs);
    const float32x4_t a2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 32u)));
    const float32x4_t a3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(a_be + ay_off + 48u)));
    const float32x4_t a_re1 = vuzp1q_f32(a2, a3);
    const float32x4_t a_im1 = vuzp2q_f32(a2, a3);
    const float32x4_t y2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 32u)));
    const float32x4_t y3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(y_be + ay_off + 48u)));
    const float32x4_t prod_re1 = vfmsq_f32(vmulq_f32(h_re1, a_re1), h_im1, a_im1);
    const float32x4_t prod_im1 = vfmaq_f32(vmulq_f32(h_im1, a_re1), h_re1, a_im1);
    vst1q_u8(y_be + ay_off + 32u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y2, vzip1q_f32(prod_re1, prod_im1)))));
    vst1q_u8(y_be + ay_off + 48u, vrev32q_u8(vreinterpretq_u8_f32(vaddq_f32(y3, vzip2q_f32(prod_re1, prod_im1)))));
  }
#else
  const uint32_t pairs = uint32_t(blocks) << 4;
  for (uint32_t k = 0; k < pairs; ++k) {
    const float h_re = float(LoadBeS16(ir_be + (k << 2))) * scale;
    const float h_im = float(LoadBeS16(ir_be + (k << 2) + 2u)) * scale;
    const float a_re = LoadBeF32(a_be + (k << 3));
    const float a_im = LoadBeF32(a_be + (k << 3) + 4u);
    const float y_re = LoadBeF32(y_be + (k << 3));
    const float y_im = LoadBeF32(y_be + (k << 3) + 4u);
    StoreBeF32(y_be + (k << 3), y_re + (h_re * a_re - h_im * a_im));
    StoreBeF32(y_be + (k << 3) + 4u, y_im + std::fmaf(h_re, a_im, h_im * a_re));
  }
#endif
}

inline void RunSelfTest() {
  alignas(64) uint8_t a_be[256]{}, y_nat[256]{}, y_ref[256]{}, ir0_be[128]{}, ir1_be[128]{};
  for (u32 k = 0; k < 32u; ++k) {
    StoreBeF32(a_be + (k << 3), std::sin(0.31f * float(k)));
    StoreBeF32(a_be + (k << 3) + 4u, std::cos(0.43f * float(k)));
    StoreBeF32(y_ref + (k << 3), 0.1f * float(int(k) - 16));
    StoreBeF32(y_ref + (k << 3) + 4u, -0.05f * float(k));
    const uint16_t h0r = __builtin_bswap16(uint16_t(int16_t((int(k) * 731) - 10000)));
    const uint16_t h0i = __builtin_bswap16(uint16_t(int16_t((int(k) * -523) + 8000)));
    const uint16_t h1r = __builtin_bswap16(uint16_t(int16_t((int(k) * 419) + 3000)));
    const uint16_t h1i = __builtin_bswap16(uint16_t(int16_t((int(k) * -887) + 12000)));
    std::memcpy(ir0_be + (k << 2), &h0r, 2);
    std::memcpy(ir0_be + (k << 2) + 2u, &h0i, 2);
    std::memcpy(ir1_be + (k << 2), &h1r, 2);
    std::memcpy(ir1_be + (k << 2) + 2u, &h1i, 2);
  }
  int failures = 0;
  auto verify = [&](const char* label, bool dual, float s0, float s1) {
    std::memcpy(y_nat, y_ref, sizeof(y_ref));
    if (dual) {
      ConvDualFast(y_nat, a_be, ir0_be, ir1_be, 2, s0, s1);
    } else {
      ConvSingleFast(y_nat, a_be, ir0_be, 2, s0);
    }
    for (u32 k = 0; k < 32u; ++k) {
      const float hr = dual ? (float(LoadBeS16(ir0_be + (k << 2))) * s0 + float(LoadBeS16(ir1_be + (k << 2))) * s1)
                            : (float(LoadBeS16(ir0_be + (k << 2))) * s0);
      const float hi = dual ? (float(LoadBeS16(ir0_be + (k << 2) + 2u)) * s0 + float(LoadBeS16(ir1_be + (k << 2) + 2u)) * s1)
                            : (float(LoadBeS16(ir0_be + (k << 2) + 2u)) * s0);
      const float ar = LoadBeF32(a_be + (k << 3));
      const float ai = LoadBeF32(a_be + (k << 3) + 4u);
      const float er = LoadBeF32(y_ref + (k << 3)) + (hr * ar - hi * ai);
      const float ei = LoadBeF32(y_ref + (k << 3) + 4u) + (hr * ai + hi * ar);
      const float gr = LoadBeF32(y_nat + (k << 3));
      const float gi = LoadBeF32(y_nat + (k << 3) + 4u);
      if (std::isnan(gr) || std::isnan(gi) || std::fabs(gr - er) > 1e-4f || std::fabs(gi - ei) > 1e-4f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82A87020 self-test ({}) k={} got=({},{}) exp=({},{})", label, k, gr, gi, er, ei);
        return;
      }
    }
  };
  verify("dual", true, 0.75f / 32767.0f, 0.45f / 16384.0f);
  verify("single_pos", false, 0.60f / 32767.0f, 0.0f);
  verify("single_neg", false, -0.50f / 32767.0f, 0.0f);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82A87020 FFT conv self-test: 3/3 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t r3 = ctx.r3.u32;
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  const uint32_t r6 = ctx.r6.u32;
  const uint32_t r7 = ctx.r7.u32;
  const double f1 = ctx.f1.f64;
  const double f2 = ctx.f2.f64;
  if (r3 < 0x1000u) {
    ::__imp__sub_82A87020(ctx, base);
    return;
  }
  const int32_t samples = int32_t(LoadBeU32(Raw(base, r3 + 76u)));
  const int32_t blocks = samples / 32;
  if (blocks <= 0) return;

  const uint64_t ay_bytes = uint64_t(blocks) << 7;
  const uint64_t ir_bytes = uint64_t(blocks) << 6;
  const uint32_t a_va = r4 & ~0xFu;
  const uint32_t y_va = r7 & ~0xFu;
  auto valid_span = [](uint32_t va, uint64_t len) {
    return va >= 0x1000u && uint64_t(va) + len <= 0x100000000ull &&
           !(va < 0xE0000000u && uint64_t(va) + len > 0xE0000000u);
  };
  auto disjoint = [](uint32_t a, uint64_t alen, uint32_t b, uint64_t blen) {
    return uint64_t(a) + alen <= b || uint64_t(b) + blen <= a;
  };
  if (!valid_span(a_va, ay_bytes) || !valid_span(y_va, ay_bytes) || !disjoint(a_va, ay_bytes, y_va, ay_bytes)) {
    ::__imp__sub_82A87020(ctx, base);
    return;
  }

  if (r5 != 0u && f1 > 0.0 && r6 != 0u && f2 > 0.0) {
    const uint32_t ir0_va = (r5 + 16u) & ~0xFu;
    const uint32_t ir1_va = (r6 + 16u) & ~0xFu;
    if (!valid_span(ir0_va, ir_bytes) || !valid_span(ir1_va, ir_bytes) ||
        !disjoint(y_va, ay_bytes, ir0_va, ir_bytes) || !disjoint(y_va, ay_bytes, ir1_va, ir_bytes)) {
      ::__imp__sub_82A87020(ctx, base);
      return;
    }
    const float div0 = float(int64_t(LoadBeS16(Raw(base, r5))));
    const float div1 = float(int64_t(LoadBeS16(Raw(base, r6))));
    const float scale0 = float(f1 / double(div0));
    const float scale1 = float(f2 / double(div1));
    Card(base, y_va, uint32_t(ay_bytes));
    ctx.fpscr.enableFlushMode();
    ConvDualFast(Raw(base, y_va), Raw(base, a_va), Raw(base, ir0_va), Raw(base, ir1_va), blocks, scale0, scale1);
    return;
  }

  // Fallback path (loc_82A874A8): validate all active passes before writing y_be.
  const uint32_t ptrs[2] = {r5, r6};
  const double gains[2] = {f1, f2};
  uint32_t ir_vas[2]{};
  bool active[2]{};
  for (int pass = 0; pass < 2; ++pass) {
    if (ptrs[pass] == 0u || gains[pass] == 0.0) continue;
    const uint32_t ir_va = (ptrs[pass] + 16u) & ~0xFu;
    if (!valid_span(ir_va, ir_bytes) || !disjoint(y_va, ay_bytes, ir_va, ir_bytes)) {
      ::__imp__sub_82A87020(ctx, base);
      return;
    }
    ir_vas[pass] = ir_va;
    active[pass] = true;
  }
  if (active[0] || active[1]) {
    Card(base, y_va, uint32_t(ay_bytes));
    ctx.fpscr.enableFlushMode();
    for (int pass = 0; pass < 2; ++pass) {
      if (!active[pass]) continue;
      const float div = float(int64_t(LoadBeS16(Raw(base, ptrs[pass]))));
      const float scale = float(gains[pass] / double(div));
      ConvSingleFast(Raw(base, y_va), Raw(base, a_va), Raw(base, ir_vas[pass]), blocks, scale);
    }
  }
}

}  // namespace n_82A87020

// 8. sub_826D8178: DMA job-stream matrix-palette skinning (15-20% Job 0/2/3, 4.4% FGGameSim).
namespace n_826D8178 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

#if defined(__aarch64__)
// vrev32q_u8 swaps bytes inside each 32-bit float while keeping lanes [x, y, z, w] intact.
inline float32x4_t LoadBeF4(const uint8_t* p) noexcept {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
}

inline float32x2_t LoadBeF2(const uint8_t* p) noexcept {
  return vreinterpret_f32_u8(vrev32_u8(vld1_u8(p)));
}

inline void StoreBeF4(uint8_t* p, float32x4_t v) noexcept {
  vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v)));
}
#endif

struct alignas(16) BoneMat {
#if defined(__aarch64__)
  float32x4_t r[3];
#else
  float r[3][4];
#endif
};

inline void LoadBoneMat(BoneMat& dst, const uint8_t* src) noexcept {
#if defined(__aarch64__)
  dst.r[0] = LoadBeF4(src);
  dst.r[1] = LoadBeF4(src + 16);
  dst.r[2] = LoadBeF4(src + 32);
#else
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) {
      dst.r[row][col] = LoadBeF32(src + uint32_t(row * 16 + col * 4));
    }
  }
#endif
}

// Kept out-of-line so check_switch_build never finds vector FMLA inside sub_826D8178 itself.
template <int B>
[[gnu::noinline]] void SkinChunk(const BoneMat* __restrict pal_le, const uint8_t* __restrict in_pos,
                                 const uint8_t* __restrict in_v1, const uint8_t* __restrict in_v2,
                                 const uint8_t* __restrict in_idx, const uint8_t* __restrict in_wgt,
                                 uint8_t* __restrict out_pos, uint8_t* __restrict out_v1,
                                 uint8_t* __restrict out_v2, uint32_t n_verts) noexcept {
#if defined(__aarch64__)
  const float32x4_t zero = vdupq_n_f32(0.0f);
  for (uint32_t v = 0; v < n_verts; ++v) {
    if ((v & 3u) == 0u) {
      const uint32_t off16 = (v << 4) + 128u;
      __builtin_prefetch(in_pos + off16, 0, 0);
      __builtin_prefetch(in_v1 + off16, 0, 0);
      __builtin_prefetch(in_v2 + off16, 0, 0);
      __builtin_prefetch(in_idx + v * uint32_t(B) + 64u, 0, 0);
      if constexpr (B > 1) {
        __builtin_prefetch(in_wgt + v * uint32_t(B * 4) + 128u, 0, 0);
      }
    }

    const uint8_t* ip = in_idx + v * uint32_t(B);
    float32x4_t r0, r1, r2;
    if constexpr (B == 1) {
      const BoneMat& m0 = pal_le[ip[0]];
      r0 = m0.r[0];
      r1 = m0.r[1];
      r2 = m0.r[2];
    } else if constexpr (B == 2) {
      const float32x2_t w = LoadBeF2(in_wgt + v * 8u);
      const BoneMat& m0 = pal_le[ip[0]];
      const BoneMat& m1 = pal_le[ip[1]];
      r0 = vfmaq_lane_f32(vmulq_lane_f32(m0.r[0], w, 0), m1.r[0], w, 1);
      r1 = vfmaq_lane_f32(vmulq_lane_f32(m0.r[1], w, 0), m1.r[1], w, 1);
      r2 = vfmaq_lane_f32(vmulq_lane_f32(m0.r[2], w, 0), m1.r[2], w, 1);
    } else if constexpr (B == 4) {
      const float32x4_t w = LoadBeF4(in_wgt + v * 16u);
      const BoneMat& m0 = pal_le[ip[0]];
      const BoneMat& m1 = pal_le[ip[1]];
      r0 = vfmaq_laneq_f32(vmulq_laneq_f32(m0.r[0], w, 0), m1.r[0], w, 1);
      r1 = vfmaq_laneq_f32(vmulq_laneq_f32(m0.r[1], w, 0), m1.r[1], w, 1);
      r2 = vfmaq_laneq_f32(vmulq_laneq_f32(m0.r[2], w, 0), m1.r[2], w, 1);
      if (vget_lane_u64(vreinterpret_u64_f32(vget_high_f32(w)), 0) != 0ull) {
        const BoneMat& m2 = pal_le[ip[2]];
        const BoneMat& m3 = pal_le[ip[3]];
        const float32x4_t s0 = vfmaq_laneq_f32(vmulq_laneq_f32(m2.r[0], w, 2), m3.r[0], w, 3);
        const float32x4_t s1 = vfmaq_laneq_f32(vmulq_laneq_f32(m2.r[1], w, 2), m3.r[1], w, 3);
        const float32x4_t s2 = vfmaq_laneq_f32(vmulq_laneq_f32(m2.r[2], w, 2), m3.r[2], w, 3);
        r0 = vaddq_f32(r0, s0);
        r1 = vaddq_f32(r1, s1);
        r2 = vaddq_f32(r2, s2);
      }
    } else {
      const uint8_t* wp = in_wgt + v * 24u;
      const float32x4_t w03 = LoadBeF4(wp);
      const float32x2_t w45 = LoadBeF2(wp + 16);
      const BoneMat& m0 = pal_le[ip[0]];
      const BoneMat& m1 = pal_le[ip[1]];
      r0 = vfmaq_laneq_f32(vmulq_laneq_f32(m0.r[0], w03, 0), m1.r[0], w03, 1);
      r1 = vfmaq_laneq_f32(vmulq_laneq_f32(m0.r[1], w03, 0), m1.r[1], w03, 1);
      r2 = vfmaq_laneq_f32(vmulq_laneq_f32(m0.r[2], w03, 0), m1.r[2], w03, 1);
      const bool has23 = vget_lane_u64(vreinterpret_u64_f32(vget_high_f32(w03)), 0) != 0ull;
      const bool has45 = vget_lane_u64(vreinterpret_u64_f32(w45), 0) != 0ull;
      if (has23 || has45) {
        float32x4_t s0 = zero, s1 = zero, s2 = zero;
        if (has23) {
          const BoneMat& m2 = pal_le[ip[2]];
          const BoneMat& m3 = pal_le[ip[3]];
          s0 = vfmaq_laneq_f32(vmulq_laneq_f32(m2.r[0], w03, 2), m3.r[0], w03, 3);
          s1 = vfmaq_laneq_f32(vmulq_laneq_f32(m2.r[1], w03, 2), m3.r[1], w03, 3);
          s2 = vfmaq_laneq_f32(vmulq_laneq_f32(m2.r[2], w03, 2), m3.r[2], w03, 3);
        }
        if (has45) {
          const BoneMat& m4 = pal_le[ip[4]];
          const BoneMat& m5 = pal_le[ip[5]];
          r0 = vfmaq_lane_f32(r0, m4.r[0], w45, 0);
          r1 = vfmaq_lane_f32(r1, m4.r[1], w45, 0);
          r2 = vfmaq_lane_f32(r2, m4.r[2], w45, 0);
          s0 = vfmaq_lane_f32(s0, m5.r[0], w45, 1);
          s1 = vfmaq_lane_f32(s1, m5.r[1], w45, 1);
          s2 = vfmaq_lane_f32(s2, m5.r[2], w45, 1);
        }
        r0 = vaddq_f32(r0, s0);
        r1 = vaddq_f32(r1, s1);
        r2 = vaddq_f32(r2, s2);
      }
    }

    const float32x4_t t0 = vtrn1q_f32(r0, r1);
    const float32x4_t t1 = vtrn2q_f32(r0, r1);
    const float32x4_t t2 = vtrn1q_f32(r2, zero);
    const float32x4_t t3 = vtrn2q_f32(r2, zero);
    const float32x4_t c0 = vreinterpretq_f32_f64(vzip1q_f64(vreinterpretq_f64_f32(t0), vreinterpretq_f64_f32(t2)));
    const float32x4_t c2 = vreinterpretq_f32_f64(vzip2q_f64(vreinterpretq_f64_f32(t0), vreinterpretq_f64_f32(t2)));
    const float32x4_t c1 = vreinterpretq_f32_f64(vzip1q_f64(vreinterpretq_f64_f32(t1), vreinterpretq_f64_f32(t3)));
    const float32x4_t c3 = vreinterpretq_f32_f64(vzip2q_f64(vreinterpretq_f64_f32(t1), vreinterpretq_f64_f32(t3)));

    const uint32_t voff = v << 4;
    const float32x4_t pos = LoadBeF4(in_pos + voff);
    const float32x4_t v1 = LoadBeF4(in_v1 + voff);
    const float32x4_t v2 = LoadBeF4(in_v2 + voff);

    float32x4_t out_p = vfmaq_laneq_f32(vmulq_laneq_f32(c3, pos, 3), c0, pos, 0);
    out_p = vfmaq_laneq_f32(out_p, c1, pos, 1);
    out_p = vfmaq_laneq_f32(out_p, c2, pos, 2);
    StoreBeF4(out_pos + voff, vsetq_lane_f32(1.0f, out_p, 3));

    float32x4_t out_n = vfmaq_laneq_f32(vmulq_laneq_f32(c0, v1, 0), c1, v1, 1);
    out_n = vfmaq_laneq_f32(out_n, c2, v1, 2);
    float32x4_t out_t = vfmaq_laneq_f32(vmulq_laneq_f32(c0, v2, 0), c1, v2, 1);
    out_t = vfmaq_laneq_f32(out_t, c2, v2, 2);

    const float32x4_t p0 = vpaddq_f32(vmulq_f32(out_n, out_n), vmulq_f32(out_t, out_t));
    const float32x2_t len2 = vget_low_f32(vpaddq_f32(p0, p0));
    float32x2_t inv = vrsqrte_f32(len2);
    inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(len2, inv), inv));
    inv = vmul_f32(inv, vrsqrts_f32(vmul_f32(len2, inv), inv));
    // Mask zero-length normals/tangents so 0 * +Inf cannot form NaN.
    inv = vreinterpret_f32_u32(vand_u32(vreinterpret_u32_f32(inv), vcgtz_f32(len2)));
    StoreBeF4(out_v1 + voff, vsetq_lane_f32(0.0f, vmulq_lane_f32(out_n, inv, 0), 3));
    StoreBeF4(out_v2 + voff, vsetq_lane_f32(0.0f, vmulq_lane_f32(out_t, inv, 1), 3));
  }
#else
  for (uint32_t v = 0; v < n_verts; ++v) {
    float m[3][4]{};
    const uint8_t* ip = in_idx + v * uint32_t(B);
    for (int k = 0; k < B; ++k) {
      const float w = (B == 1) ? 1.0f : LoadBeF32(in_wgt + v * uint32_t(B * 4) + uint32_t(k * 4));
      const BoneMat& bm = pal_le[ip[k]];
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
          m[r][c] += w * bm.r[r][c];
        }
      }
    }
    const uint32_t voff = v << 4;
    float p[4], n1[4], n2[4];
    for (int c = 0; c < 4; ++c) {
      p[c] = LoadBeF32(in_pos + voff + uint32_t(c * 4));
      n1[c] = LoadBeF32(in_v1 + voff + uint32_t(c * 4));
      n2[c] = LoadBeF32(in_v2 + voff + uint32_t(c * 4));
    }
    for (int r = 0; r < 3; ++r) {
      StoreBeF32(out_pos + voff + uint32_t(r * 4), m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] * p[3]);
    }
    StoreBeF32(out_pos + voff + 12u, 1.0f);
    auto xform_norm = [&](const float* src, uint8_t* dst) {
      const float x = m[0][0] * src[0] + m[0][1] * src[1] + m[0][2] * src[2];
      const float y = m[1][0] * src[0] + m[1][1] * src[1] + m[1][2] * src[2];
      const float z = m[2][0] * src[0] + m[2][1] * src[1] + m[2][2] * src[2];
      const float l2 = x * x + y * y + z * z;
      const float inv = (l2 > 0.0f) ? (1.0f / std::sqrt(l2)) : 0.0f;
      StoreBeF32(dst, x * inv);
      StoreBeF32(dst + 4, y * inv);
      StoreBeF32(dst + 8, z * inv);
      StoreBeF32(dst + 12, 0.0f);
    };
    xform_norm(n1, out_v1 + voff);
    xform_norm(n2, out_v2 + voff);
  }
#endif
}

template <int B>
inline void SelfTestCase(int& failures) {
  alignas(64) uint8_t pal[4 * 64]{};
  alignas(64) BoneMat pal_le[4]{};
  for (int b = 0; b < 4; ++b) {
    uint8_t* bp = pal + b * 64;
    StoreBeF32(bp + 0, 1.0f + 0.1f * b);
    StoreBeF32(bp + 4, 0.05f * b);
    StoreBeF32(bp + 12, 10.0f * (b + 1));
    StoreBeF32(bp + 16, -0.05f * b);
    StoreBeF32(bp + 20, 1.0f - 0.05f * b);
    StoreBeF32(bp + 28, -5.0f * (b + 1));
    StoreBeF32(bp + 40, 1.0f + 0.05f * b);
    StoreBeF32(bp + 44, 2.0f * (b + 1));
    LoadBoneMat(pal_le[b], bp);
  }
  alignas(64) uint8_t in_p[64]{}, in_n[64]{}, in_t[64]{}, idx[32]{}, wgt[128]{};
  alignas(64) uint8_t out_p[64]{}, out_n[64]{}, out_t[64]{};
  for (uint32_t v = 0; v < 4; ++v) {
    StoreBeF32(in_p + v * 16 + 0, 1.0f + float(v));
    StoreBeF32(in_p + v * 16 + 4, -2.0f + float(v));
    StoreBeF32(in_p + v * 16 + 8, 0.5f * float(v + 1));
    StoreBeF32(in_p + v * 16 + 12, 1.0f);
    const float zchk = (v == 3) ? 0.0f : 1.0f;
    StoreBeF32(in_n + v * 16 + 0, 0.6f * zchk);
    StoreBeF32(in_n + v * 16 + 4, 0.8f * zchk);
    StoreBeF32(in_n + v * 16 + 8, 0.0f);
    StoreBeF32(in_t + v * 16 + 0, 0.0f);
    StoreBeF32(in_t + v * 16 + 4, -0.6f * zchk);
    StoreBeF32(in_t + v * 16 + 8, 0.8f * zchk);
    for (int k = 0; k < B; ++k) {
      idx[v * uint32_t(B) + uint32_t(k)] = uint8_t((v + k) & 3);
      float wk = 1.0f / float(B);
      // Exercise zero-weight tail early-outs in B==4 (w2=w3=0) and B==6 (w4=w5=0).
      if constexpr (B == 4) {
        if (v >= 2u) {
          constexpr float kTailZero4[4] = {0.7f, 0.3f, 0.0f, 0.0f};
          wk = kTailZero4[k];
        }
      } else if constexpr (B == 6) {
        if (v == 2u) {
          constexpr float kTailZero6_4[6] = {0.4f, 0.3f, 0.2f, 0.1f, 0.0f, 0.0f};
          wk = kTailZero6_4[k];
        } else if (v == 3u) {
          constexpr float kTailZero6_2[6] = {0.7f, 0.3f, 0.0f, 0.0f, 0.0f, 0.0f};
          wk = kTailZero6_2[k];
        }
      }
      StoreBeF32(wgt + (v * uint32_t(B) + uint32_t(k)) * 4u, wk);
    }
  }
  SkinChunk<B>(pal_le, in_p, in_n, in_t, idx, (B == 1) ? nullptr : wgt, out_p, out_n, out_t, 4u);
  for (uint32_t v = 0; v < 4; ++v) {
    float m[3][4]{};
    for (int k = 0; k < B; ++k) {
      const float w = (B == 1) ? 1.0f : LoadBeF32(wgt + (v * uint32_t(B) + uint32_t(k)) * 4u);
      const uint8_t* bp = pal + (uint32_t((v + k) & 3) << 6);
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) m[r][c] += w * LoadBeF32(bp + uint32_t(r * 16 + c * 4));
      }
    }
    float ep[4], en[4], et[4];
    float p[4], n1[4], n2[4];
    for (int c = 0; c < 4; ++c) {
      p[c] = LoadBeF32(in_p + v * 16 + c * 4);
      n1[c] = LoadBeF32(in_n + v * 16 + c * 4);
      n2[c] = LoadBeF32(in_t + v * 16 + c * 4);
    }
    for (int r = 0; r < 3; ++r) ep[r] = m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3] * p[3];
    ep[3] = 1.0f;
    auto norm_ref = [&](const float* s, float* o) {
      for (int r = 0; r < 3; ++r) o[r] = m[r][0] * s[0] + m[r][1] * s[1] + m[r][2] * s[2];
      const float l2 = o[0] * o[0] + o[1] * o[1] + o[2] * o[2];
      const float inv = (l2 > 0.0f) ? (1.0f / std::sqrt(l2)) : 0.0f;
      o[0] *= inv;
      o[1] *= inv;
      o[2] *= inv;
      o[3] = 0.0f;
    };
    norm_ref(n1, en);
    norm_ref(n2, et);
    for (int c = 0; c < 4; ++c) {
      const float gp = LoadBeF32(out_p + v * 16 + c * 4);
      const float gn = LoadBeF32(out_n + v * 16 + c * 4);
      const float gt = LoadBeF32(out_t + v * 16 + c * 4);
      if (std::isnan(gp) || std::isnan(gn) || std::isnan(gt) || std::fabs(gp - ep[c]) > 1e-4f ||
          std::fabs(gn - en[c]) > 1e-4f || std::fabs(gt - et[c]) > 1e-4f) {
        ++failures;
        REXLOG_ERROR("[hot] sub_826D8178 self-test failed B={} v={} c={} got=({},{},{}) exp=({},{},{})", B, v, c,
                     gp, gn, gt, ep[c], en[c], et[c]);
        return;
      }
    }
  }
}

inline void RunSelfTest() {
  int failures = 0;
  SelfTestCase<1>(failures);
  SelfTestCase<2>(failures);
  SelfTestCase<4>(failures);
  SelfTestCase<6>(failures);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_826D8178 skinning self-test: 4/4 bone modes passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }

  const uint64_t old_sp = ctx.r1.u64;
  const uint32_t r3_in = ctx.r3.u32;
  const uint32_t r4_in = ctx.r4.u32;
  const uint32_t r5_in = ctx.r5.u32;
  const uint32_t r6_in = ctx.r6.u32;

  if (r6_in != 0u || uint32_t(old_sp) < 0x1000u || r5_in == 0u) {
    ::__imp__sub_826D8178(ctx, base);
    return;
  }
  const uint32_t pre_bones = REX_LOAD_U32(r5_in + 4u);
  if (pre_bones != 1u && pre_bones != 2u && pre_bones != 4u && pre_bones != 6u) {
    ::__imp__sub_826D8178(ctx, base);
    return;
  }

  const uint32_t sp = uint32_t(old_sp) - 1024u;
  ctx.r1.u64 = uint64_t(sp);
  REX_STORE_U32(sp, uint32_t(old_sp));

  ctx.r3.u64 = sp + 592u;
  ctx.r4.u64 = 0x8209CCDCu;
  ctx.r5.u64 = r4_in;
  ::__imp__sub_82792C28(ctx, base);

  if (sp + 96u != r5_in) {
    std::memcpy(Raw(base, sp + 96u), Raw(base, r5_in), 48u);
    Card(base, sp + 96u, 48u);
  }

  ctx.r3.u64 = sp + 144u;
  ctx.r4.u64 = r3_in;
  ctx.r5.u64 = r4_in;
  ctx.r6.u64 = 0u;
  ctx.r7.u64 = 262144u;
  ::__imp__sub_8283C4F8(ctx, base);

  bool ran_chunk = false;
  const uint32_t num_bones = REX_LOAD_U32(sp + 100u);
  if (num_bones == 1u || num_bones == 2u || num_bones == 4u || num_bones == 6u) {
    const uint32_t palette_va = REX_LOAD_U32(sp + 108u) & ~0xFu;
    const uint8_t* pal = Raw(base, palette_va);
    // Cache only referenced bones in L1-resident LE form across all chunks of this call.
    alignas(64) BoneMat pal_le[256];
    uint64_t cached[4]{};

    ctx.r3.u64 = sp + 144u;
    ::__imp__sub_8283C600(ctx, base);

    auto get_stream = [&](uint32_t idx) -> uint32_t {
      ctx.r3.u64 = sp + 144u;
      ctx.r4.u64 = idx;
      ::__imp__sub_8283C4D8(ctx, base);
      return ctx.r3.u32;
    };

    for (;;) {
      ctx.r3.u64 = sp + 144u;
      ::__imp__sub_8283C280(ctx, base);
      if ((ctx.r3.u32 & 0xFFu) != 0u) break;

      const uint32_t in_pos_va = get_stream(0u) & ~0xFu;
      const uint32_t in_v1_va = get_stream(1u) & ~0xFu;
      const uint32_t in_v2_va = get_stream(2u) & ~0xFu;
      const uint32_t in_idx_va = get_stream(3u);
      const uint32_t in_wgt_va = (num_bones > 1u) ? get_stream(4u) : 0u;
      const uint32_t out_pos_va = get_stream(5u) & ~0xFu;
      const uint32_t out_v1_va = get_stream(6u) & ~0xFu;
      const uint32_t out_v2_va = get_stream(7u) & ~0xFu;

      ctx.r3.u64 = sp + 144u;
      ::__imp__sub_8283C288(ctx, base);
      const int32_t count = int32_t(ctx.r3.s32);
      if (count > 0) {
        const uint32_t n_verts = (((uint32_t(count) - 1u) >> 2) + 1u) << 2;
        const uint32_t out_bytes = n_verts << 4;
        Card(base, out_pos_va, out_bytes);
        Card(base, out_v1_va, out_bytes);
        Card(base, out_v2_va, out_bytes);

        const uint8_t* in_pos = Raw(base, in_pos_va);
        const uint8_t* in_v1 = Raw(base, in_v1_va);
        const uint8_t* in_v2 = Raw(base, in_v2_va);
        const uint8_t* in_idx = Raw(base, in_idx_va);
        const uint8_t* in_wgt = (num_bones > 1u) ? Raw(base, in_wgt_va) : nullptr;
        uint8_t* out_pos = Raw(base, out_pos_va);
        uint8_t* out_v1 = Raw(base, out_v1_va);
        uint8_t* out_v2 = Raw(base, out_v2_va);

        uint64_t used[4]{};
        const uint32_t n_idx = n_verts * num_bones;
        uint32_t i = 0;
        for (; i + 4u <= n_idx; i += 4u) {
          uint32_t packed;
          std::memcpy(&packed, in_idx + i, 4u);
          const uint32_t b0 = packed & 0xFFu;
          const uint32_t b1 = (packed >> 8) & 0xFFu;
          const uint32_t b2 = (packed >> 16) & 0xFFu;
          const uint32_t b3 = packed >> 24;
          used[b0 >> 6] |= (1ull << (b0 & 63u));
          used[b1 >> 6] |= (1ull << (b1 & 63u));
          used[b2 >> 6] |= (1ull << (b2 & 63u));
          used[b3 >> 6] |= (1ull << (b3 & 63u));
        }
        for (; i < n_idx; ++i) {
          const uint8_t b = in_idx[i];
          used[b >> 6] |= (1ull << (b & 63u));
        }
        for (uint32_t w = 0; w < 4u; ++w) {
          uint64_t todo = used[w] & ~cached[w];
          if (todo) {
            cached[w] |= todo;
            do {
              const uint32_t bit = uint32_t(__builtin_ctzll(todo));
              todo &= todo - 1ull;
              const uint32_t b = (w << 6) | bit;
              LoadBoneMat(pal_le[b], pal + (b << 6));
            } while (todo);
          }
        }

        switch (num_bones) {
          case 1u:
            SkinChunk<1>(pal_le, in_pos, in_v1, in_v2, in_idx, in_wgt, out_pos, out_v1, out_v2, n_verts);
            break;
          case 2u:
            SkinChunk<2>(pal_le, in_pos, in_v1, in_v2, in_idx, in_wgt, out_pos, out_v1, out_v2, n_verts);
            break;
          case 4u:
            SkinChunk<4>(pal_le, in_pos, in_v1, in_v2, in_idx, in_wgt, out_pos, out_v1, out_v2, n_verts);
            break;
          case 6u:
            SkinChunk<6>(pal_le, in_pos, in_v1, in_v2, in_idx, in_wgt, out_pos, out_v1, out_v2, n_verts);
            break;
        }
        ran_chunk = true;
      }

      ctx.r3.u64 = sp + 144u;
      ::__imp__sub_8283C690(ctx, base);
    }
  }

  ctx.r3.u64 = sp + 144u;
  ::__imp__sub_8283C580(ctx, base);
  if (ran_chunk) ctx.fpscr.enableFlushMode();
  ctx.r1.u64 = old_sp;
}

}  // namespace n_826D8178

// 9. sub_82A8F798: Audio dynamics compressor/limiter (14.1% Job 4 / 2.4-2.9% total CPU).
// Replaces scalar/SIMDe envelope + pow() + sub_82A859C8 smoother + gain apply with L1-resident LE float32 buffers
// and vectorized ARM64 NEON log2/exp2 gain calculation (full float32 precision, <1e-6 error).
namespace n_82A8F798 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

inline uint32_t LoadBeU32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  return __builtin_bswap32(raw);
}

inline void StoreBeU32(uint8_t* p, uint32_t v) noexcept {
  const uint32_t raw = __builtin_bswap32(v);
  std::memcpy(p, &raw, 4);
}

inline uint16_t LoadBeU16(const uint8_t* p) noexcept {
  uint16_t raw;
  std::memcpy(&raw, p, 2);
  return __builtin_bswap16(raw);
}

#if defined(__aarch64__)
inline float32x4_t Log2Fast(float32x4_t x) noexcept {
  // Range-reduce x = 2^e * m with m in [sqrt(0.5), sqrt(2)] so |z| = |(m-1)/(m+1)| <= 0.17157.
  const uint32x4_t xi = vreinterpretq_u32_f32(x);
  const uint32x4_t adj = vaddq_u32(xi, vdupq_n_u32(0x004AFB0Du));
  const int32x4_t exp_i = vsubq_s32(vreinterpretq_s32_u32(vshrq_n_u32(adj, 23)), vdupq_n_s32(127));
  const float32x4_t exp_f = vcvtq_f32_s32(exp_i);
  const uint32x4_t mant_i =
      vsubq_u32(vorrq_u32(vandq_u32(adj, vdupq_n_u32(0x007FFFFFu)), vdupq_n_u32(0x3F800000u)), vdupq_n_u32(0x004AFB0Du));
  const float32x4_t m = vreinterpretq_f32_u32(mant_i);
  const float32x4_t one = vdupq_n_f32(1.0f);
  const float32x4_t z = vdivq_f32(vsubq_f32(m, one), vaddq_f32(m, one));
  const float32x4_t z2 = vmulq_f32(z, z);
  // log2(m) = (2/ln2) * z * (1 + z2*(1/3 + z2*(1/5 + z2/7)))
  float32x4_t poly = vfmaq_f32(vdupq_n_f32(0.2f), z2, vdupq_n_f32(1.0f / 7.0f));
  poly = vfmaq_f32(vdupq_n_f32(1.0f / 3.0f), z2, poly);
  poly = vfmaq_f32(one, z2, poly);
  const float32x4_t log2_m = vmulq_f32(vmulq_f32(z, vdupq_n_f32(2.88539008178f)), poly);
  return vaddq_f32(exp_f, log2_m);
}

inline float32x4_t Exp2Fast(float32x4_t y) noexcept {
  const float32x4_t y_lo = vdupq_n_f32(-126.0f);
  const float32x4_t yc = vmaxq_f32(vminq_f32(y, vdupq_n_f32(127.0f)), y_lo);
  const float32x4_t kf = vrndnq_f32(yc);
  const float32x4_t f = vsubq_f32(yc, kf);
  const int32x4_t ki = vcvtq_s32_f32(kf);
  float32x4_t p = vdupq_n_f32(0.00015403530f);
  p = vfmaq_f32(vdupq_n_f32(0.00133335581f), p, f);
  p = vfmaq_f32(vdupq_n_f32(0.00961812911f), p, f);
  p = vfmaq_f32(vdupq_n_f32(0.05550410866f), p, f);
  p = vfmaq_f32(vdupq_n_f32(0.24022650696f), p, f);
  p = vfmaq_f32(vdupq_n_f32(0.69314718056f), p, f);
  p = vfmaq_f32(vdupq_n_f32(1.0f), p, f);
  const float32x4_t scale = vreinterpretq_f32_s32(vshlq_n_s32(vaddq_s32(ki, vdupq_n_s32(127)), 23));
  const float32x4_t res = vmulq_f32(p, scale);
  return vreinterpretq_f32_u32(vandq_u32(vreinterpretq_u32_f32(res), vcgtq_f32(y, y_lo)));
}
#endif

// Out-of-line so check_switch_build never finds vector FMLA inside sub_82A8F798 itself.
[[gnu::noinline]] void CompressChannelFast(float* __restrict gain_le, const uint8_t* __restrict sc_be, float alpha,
                                           float& env_io, float threshold, float ratio_exp, bool combine,
                                           uint32_t link_mode) noexcept {
  constexpr uint32_t kDcBits = 0x219392EFu;
  float dc;
  std::memcpy(&dc, &kDcBits, 4);
  alignas(16) float env_buf[256];
  const float one_minus_alpha = 1.0f - alpha;
  float env = env_io;
#if defined(__aarch64__)
  for (uint32_t i = 0; i < 256u; i += 4u) {
    const float32x4_t s = vabsq_f32(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(sc_be + (i << 2)))));
    env = std::fmaf(vgetq_lane_f32(s, 0), one_minus_alpha, alpha * env) + dc;
    env_buf[i + 0u] = env;
    env = std::fmaf(vgetq_lane_f32(s, 1), one_minus_alpha, alpha * env) + dc;
    env_buf[i + 1u] = env;
    env = std::fmaf(vgetq_lane_f32(s, 2), one_minus_alpha, alpha * env) + dc;
    env_buf[i + 2u] = env;
    env = std::fmaf(vgetq_lane_f32(s, 3), one_minus_alpha, alpha * env) + dc;
    env_buf[i + 3u] = env;
  }
  env_io = env;
  const float32x4_t vthresh = vdupq_n_f32(threshold);
  const float32x4_t vinv = vdupq_n_f32(1.0f / threshold);
  const float32x4_t vratio = vdupq_n_f32(ratio_exp);
  const float32x4_t vone = vdupq_n_f32(1.0f);
  for (uint32_t i = 0; i < 256u; i += 4u) {
    const float32x4_t ev = vld1q_f32(env_buf + i);
    const uint32x4_t above = vcgtq_f32(ev, vthresh);
    float32x4_t g = vone;
    if (vmaxvq_u32(above) != 0u) {
      const float32x4_t x = vbslq_f32(above, vmulq_f32(ev, vinv), vone);
      const float32x4_t pw = Exp2Fast(vmulq_f32(Log2Fast(x), vratio));
      g = vbslq_f32(above, pw, vone);
    }
    if (combine) {
      const float32x4_t prev = vld1q_f32(gain_le + i);
      g = (link_mode == 0u) ? vminq_f32(prev, g) : vmaxq_f32(prev, g);
    }
    vst1q_f32(gain_le + i, g);
  }
#else
  const float inv_thresh = 1.0f / threshold;
  for (uint32_t i = 0; i < 256u; ++i) {
    env = std::fmaf(std::fabs(LoadBeF32(sc_be + (i << 2))), one_minus_alpha, alpha * env) + dc;
    env_buf[i] = env;
  }
  env_io = env;
  for (uint32_t i = 0; i < 256u; ++i) {
    const float ev = env_buf[i];
    float g = (ev > threshold) ? float(std::pow(double(ev * inv_thresh), double(ratio_exp))) : 1.0f;
    if (combine) {
      const float prev = gain_le[i];
      g = (link_mode == 0u) ? (prev < g ? prev : g) : (prev > g ? prev : g);
    }
    gain_le[i] = g;
  }
#endif
}

[[gnu::noinline]] void SmoothGainFast(float* __restrict buf_le, float att_k, float rel_k, float& cur_io) noexcept {
  constexpr uint32_t kDcBits = 0x219392EFu;
  float dc;
  std::memcpy(&dc, &kDcBits, 4);
  float cur = cur_io;
  for (uint32_t i = 0; i < 256u; ++i) {
    const float g = buf_le[i];
    const float k = (g < cur) ? att_k : rel_k;
    cur = std::fmaf(k, g, dc) - (k - 1.0f) * cur;
    buf_le[i] = cur;
  }
  cur_io = cur;
}

[[gnu::noinline]] void ApplyGainBe256(uint8_t* __restrict out_be, const uint8_t* __restrict in_be,
                                      const float* __restrict gain_le) noexcept {
#if defined(__aarch64__)
  for (uint32_t i = 0; i < 256u; i += 16u) {
    const uint32_t off = i << 2;
    const float32x4_t x0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(in_be + off + 0u)));
    const float32x4_t x1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(in_be + off + 16u)));
    const float32x4_t x2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(in_be + off + 32u)));
    const float32x4_t x3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(in_be + off + 48u)));
    const float32x4_t g0 = vld1q_f32(gain_le + i + 0u);
    const float32x4_t g1 = vld1q_f32(gain_le + i + 4u);
    const float32x4_t g2 = vld1q_f32(gain_le + i + 8u);
    const float32x4_t g3 = vld1q_f32(gain_le + i + 12u);
    vst1q_u8(out_be + off + 0u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(x0, g0))));
    vst1q_u8(out_be + off + 16u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(x1, g1))));
    vst1q_u8(out_be + off + 32u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(x2, g2))));
    vst1q_u8(out_be + off + 48u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(x3, g3))));
  }
#else
  for (uint32_t i = 0; i < 256u; ++i) {
    StoreBeF32(out_be + (i << 2), LoadBeF32(in_be + (i << 2)) * gain_le[i]);
  }
#endif
}

inline void RunSelfTest() {
  int failures = 0;
  constexpr uint32_t kDcBits = 0x219392EFu;
  float dc;
  std::memcpy(&dc, &kDcBits, 4);
  alignas(16) uint8_t in_be[2][1024]{}, out_be[2][1024]{};
  for (uint32_t ch = 0; ch < 2u; ++ch) {
    for (uint32_t i = 0; i < 256u; ++i) {
      const float s = (ch == 0u ? 0.9f : 0.6f) * std::sin(0.19f * float(i) + float(ch)) + 0.3f * std::cos(0.47f * float(i));
      StoreBeF32(in_be[ch] + (i << 2), s);
    }
  }
  auto check = [&](const char* label, bool linked, uint32_t link_mode, float threshold, float ratio_exp) {
    const float alpha = 0.85f;
    const float att_k = float(1.0 - double(float(std::pow(2.7182817459106445, double(float(-2.2f / 32.0f))))));
    const float rel_k = float(1.0 - double(float(std::pow(2.7182817459106445, double(float(-2.2f / 256.0f))))));
    alignas(16) float g_nat[2][256]{}, g_ref[2][256]{};
    float env_nat[2] = {0.1f, 0.2f}, env_ref[2] = {0.1f, 0.2f};
    float sm_nat[2] = {0.9f, 0.85f}, sm_ref[2] = {0.9f, 0.85f};
    for (uint32_t ch = 0; ch < 2u; ++ch) {
      float* dst_g = linked ? g_nat[0] : g_nat[ch];
      CompressChannelFast(dst_g, in_be[ch], alpha, env_nat[ch], threshold, ratio_exp, linked && ch > 0u, link_mode);
      if (!linked || ch == 1u) SmoothGainFast(dst_g, att_k, rel_k, sm_nat[ch]);
      // Scalar reference
      float* ref_g = linked ? g_ref[0] : g_ref[ch];
      const float inv_t = 1.0f / threshold;
      for (uint32_t i = 0; i < 256u; ++i) {
        env_ref[ch] = std::fmaf(std::fabs(LoadBeF32(in_be[ch] + (i << 2))), 1.0f - alpha, alpha * env_ref[ch]) + dc;
        float gv = (env_ref[ch] > threshold) ? float(std::pow(double(env_ref[ch] * inv_t), double(ratio_exp))) : 1.0f;
        if (linked && ch > 0u) gv = (link_mode == 0u) ? (ref_g[i] < gv ? ref_g[i] : gv) : (ref_g[i] > gv ? ref_g[i] : gv);
        ref_g[i] = gv;
      }
      if (!linked || ch == 1u) {
        float c = sm_ref[ch];
        for (uint32_t i = 0; i < 256u; ++i) {
          const float k = (ref_g[i] < c) ? att_k : rel_k;
          c = std::fmaf(k, ref_g[i], dc) - (k - 1.0f) * c;
          ref_g[i] = c;
        }
        sm_ref[ch] = c;
      }
    }
    for (uint32_t ch = 0; ch < 2u; ++ch) {
      ApplyGainBe256(out_be[ch], in_be[ch], linked ? g_nat[0] : g_nat[ch]);
      const float* ref_g = linked ? g_ref[0] : g_ref[ch];
      for (uint32_t i = 0; i < 256u; ++i) {
        const float got = LoadBeF32(out_be[ch] + (i << 2));
        const float exp = LoadBeF32(in_be[ch] + (i << 2)) * ref_g[i];
        if (std::isnan(got) || std::fabs(got - exp) > 1e-5f) {
          ++failures;
          REXLOG_ERROR("[hot] DIFFERENCE sub_82A8F798 self-test ({}) ch={} i={} got={} exp={}", label, ch, i, got, exp);
          return;
        }
      }
    }
  };
  check("unlinked_comp", false, 0u, 0.25f, -0.75f);
  check("linked_comp", true, 0u, 0.20f, -0.80f);
  check("linked_exp", true, 1u, 0.20f, 0.50f);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82A8F798 compressor self-test: 3/3 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t state_va = ctx.r3.u32;
  const uint32_t ctx_va = ctx.r4.u32;
  const uint32_t num_ch = ctx.r5.u32;
  const uint32_t sc_va = ctx.r6.u32;
  const uint32_t link_mode = ctx.r8.u32;
  if (state_va < 0x1000u || ctx_va < 0x1000u || num_ch > 8u) {
    ::__imp__sub_82A8F798(ctx, base);
    return;
  }
  if (num_ch == 0u) {
    uint8_t* p = Raw(base, ctx_va + 30860u);
    const uint32_t d0 = LoadBeU32(p);
    const uint32_t d1 = LoadBeU32(p + 4u);
    StoreBeU32(p, d1);
    StoreBeU32(p + 4u, d0);
    Card(base, ctx_va + 30860u, 8u);
    return;
  }
  const uint8_t* st = Raw(base, state_va);
  const float alpha = LoadBeF32(st + 0u);
  const float threshold = LoadBeF32(st + 68u);
  const float ratio_exp = LoadBeF32(st + 72u);
  const int32_t att_samples = int32_t(LoadBeU32(st + 76u));
  const int32_t rel_samples = int32_t(LoadBeU32(st + 80u));
  const bool linked = st[92] != 0u;
  const uint32_t in_desc_va = LoadBeU32(Raw(base, ctx_va + 30860u));
  const uint32_t out_desc_va = LoadBeU32(Raw(base, ctx_va + 30864u));
  if (in_desc_va < 0x1000u || out_desc_va < 0x1000u || threshold <= 0.0f || std::isnan(threshold) ||
      std::isnan(ratio_exp) || std::isnan(alpha) || att_samples <= 0 || rel_samples <= 0) {
    ::__imp__sub_82A8F798(ctx, base);
    return;
  }
  const uint32_t in_data_va = LoadBeU32(Raw(base, in_desc_va + 4u));
  const uint32_t in_stride = uint32_t(LoadBeU16(Raw(base, in_desc_va + 14u))) << 2;
  const uint32_t out_data_va = LoadBeU32(Raw(base, out_desc_va + 4u));
  const uint32_t out_stride = uint32_t(LoadBeU16(Raw(base, out_desc_va + 14u))) << 2;
  const uint64_t in_span = uint64_t(num_ch - 1u) * in_stride + 1024ull;
  const uint64_t out_span = uint64_t(num_ch - 1u) * out_stride + 1024ull;
  const uint64_t sc_span = uint64_t(num_ch) * 1024ull;
  auto valid_span = [](uint32_t va, uint64_t len) {
    return va >= 0x1000u && uint64_t(va) + len <= 0x100000000ull && !(va < 0xE0000000u && uint64_t(va) + len > 0xE0000000u);
  };
  const bool overlap_io = (uint64_t(in_data_va) < uint64_t(out_data_va) + out_span) &&
                          (uint64_t(out_data_va) < uint64_t(in_data_va) + in_span);
  if (!valid_span(in_data_va, in_span) || !valid_span(out_data_va, out_span) || overlap_io ||
      (sc_va != 0u && !valid_span(sc_va, sc_span)) || ((in_data_va | out_data_va | in_stride | out_stride) & 15u) != 0u) {
    ::__imp__sub_82A8F798(ctx, base);
    return;
  }

  const float att_k = float(1.0 - double(float(std::pow(2.7182817459106445, double(float(-2.2f / float(att_samples)))))));
  const float rel_k = float(1.0 - double(float(std::pow(2.7182817459106445, double(float(-2.2f / float(rel_samples)))))));

  alignas(16) float gain_buf[8][256];
  uint8_t* st_mut = Raw(base, state_va);
  Card(base, state_va + 4u, num_ch << 2);
  Card(base, state_va + 36u, num_ch << 2);

  for (uint32_t ch = 0; ch < num_ch; ++ch) {
    const uint32_t sc_ch_va = (sc_va != 0u) ? (sc_va + (ch << 10)) : (in_data_va + ch * in_stride);
    float env = LoadBeF32(st_mut + 4u + (ch << 2));
    float* dst_g = linked ? gain_buf[0] : gain_buf[ch];
    CompressChannelFast(dst_g, Raw(base, sc_ch_va), alpha, env, threshold, ratio_exp, linked && ch > 0u, link_mode);
    StoreBeF32(st_mut + 4u + (ch << 2), env);
    if (!linked || ch + 1u == num_ch) {
      float cur = LoadBeF32(st_mut + 36u + (ch << 2));
      SmoothGainFast(dst_g, att_k, rel_k, cur);
      StoreBeF32(st_mut + 36u + (ch << 2), cur);
    }
    if (sc_va != 0u) {
      std::memset(Raw(base, sc_ch_va), 0, 1024u);
      Card(base, sc_ch_va, 1024u);
    }
  }

  for (uint32_t ch = 0; ch < num_ch; ++ch) {
    const uint32_t in_ch_va = in_data_va + ch * in_stride;
    const uint32_t out_ch_va = out_data_va + ch * out_stride;
    ApplyGainBe256(Raw(base, out_ch_va), Raw(base, in_ch_va), linked ? gain_buf[0] : gain_buf[ch]);
    Card(base, out_ch_va, 1024u);
  }

  // Swap ping-pong audio buffer descriptors at r4+30860 and r4+30864.
  uint8_t* p = Raw(base, ctx_va + 30860u);
  StoreBeU32(p, out_desc_va);
  StoreBeU32(p + 4u, in_desc_va);
  Card(base, ctx_va + 30860u, 8u);
  ctx.fpscr.enableFlushMode();
}

}  // namespace n_82A8F798

// 10. sub_82AA02B0: Audio RMS/Peak meter (4.1% Job 4 / 0.71% total CPU).
namespace n_82AA02B0 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

inline uint32_t LoadBeU32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  return __builtin_bswap32(raw);
}

inline uint16_t LoadBeU16(const uint8_t* p) noexcept {
  uint16_t raw;
  std::memcpy(&raw, p, 2);
  return __builtin_bswap16(raw);
}

inline void StoreBeU16(uint8_t* p, uint16_t v) noexcept {
  const uint16_t raw = __builtin_bswap16(v);
  std::memcpy(p, &raw, 2);
}

[[gnu::noinline]] void ScanChannelFast(const uint8_t* __restrict ch_be, float& out_sum_sq, float& out_peak) noexcept {
#if defined(__aarch64__)
  float32x4_t vsum = vdupq_n_f32(0.0f);
  float32x4_t vmax = vdupq_n_f32(0.0f);
  for (uint32_t k = 0; k < 16u; ++k) {
    const float32x4_t v = vabsq_f32(vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(ch_be + (k << 6)))));
    vsum = vfmaq_f32(vsum, v, v);
    vmax = vmaxq_f32(vmax, v);
  }
  const float s = vgetq_lane_f32(vsum, 0) + vgetq_lane_f32(vsum, 1) + vgetq_lane_f32(vsum, 2) + vgetq_lane_f32(vsum, 3);
  out_sum_sq = 4.0f * s;
  out_peak = vmaxvq_f32(vmax);
#else
  float s = 0.0f, pk = 0.0f;
  for (uint32_t k = 0; k < 16u; ++k) {
    for (uint32_t j = 0; j < 4u; ++j) {
      const float v = std::fabs(LoadBeF32(ch_be + (k << 6) + (j << 2)));
      s = std::fmaf(v, v, s);
      if (v > pk) pk = v;
    }
  }
  out_sum_sq = 4.0f * s;
  out_peak = pk;
#endif
}

inline void Impl(PPCContext& __restrict ctx, uint8_t* base, uint32_t r3, uint32_t r4) noexcept {
  uint8_t* st = Raw(base, r3);
  const uint32_t num_ch = st[42];
  const int32_t win_len = int32_t(LoadBeU32(st + 376u));
  const uint32_t ring_idx = LoadBeU16(st + 388u);
  if (num_ch != 0u) {
    const uint32_t stride_bytes = uint32_t(LoadBeU16(Raw(base, r4 + 14u))) << 2;
    const uint32_t data_va = LoadBeU32(Raw(base, r4 + 4u));
    const uint32_t rms_ring_va = r3 + uint32_t(LoadBeU16(st + 384u));
    const uint32_t peak_ring_va = r3 + uint32_t(LoadBeU16(st + 386u));
    const float inv_win = 1.0f / float(win_len);
    for (uint32_t ch = 0; ch < num_ch; ++ch) {
      float sum_sq = 0.0f, peak = 0.0f;
      ScanChannelFast(Raw(base, (data_va + ch * stride_bytes) & ~0xFu), sum_sq, peak);
      const float val = (sum_sq * (1.0f / 256.0f)) * inv_win;
      const uint32_t slot = (ch * uint32_t(win_len) + ring_idx) << 2;
      const uint32_t ch_off = ch << 2;
      const float old_rms = LoadBeF32(Raw(base, rms_ring_va + slot));
      float cur_rms = LoadBeF32(st + 272u + ch_off) + (val - old_rms);
      if (cur_rms < 0.0f) cur_rms = 0.0f;
      StoreBeF32(st + 272u + ch_off, cur_rms);
      StoreBeF32(st + 240u + ch_off, LoadBeF32(st + 240u + ch_off) + val);
      StoreBeF32(st + 304u + ch_off, peak);
      if (peak > LoadBeF32(st + 336u + ch_off)) {
        StoreBeF32(st + 336u + ch_off, peak);
      }
      StoreBeF32(Raw(base, peak_ring_va + slot), peak);
      StoreBeF32(Raw(base, rms_ring_va + slot), val);
      Card(base, peak_ring_va + slot, 4u);
      Card(base, rms_ring_va + slot, 4u);
    }
    Card(base, r3 + 240u, 128u);
    if (int32_t(ring_idx) == win_len - 1) {
      for (uint32_t ch = 0; ch < num_ch; ++ch) {
        const uint32_t ch_off = ch << 2;
        StoreBeF32(st + 272u + ch_off, LoadBeF32(st + 240u + ch_off));
        StoreBeF32(st + 240u + ch_off, 0.0f);
      }
    }
  }
  const uint32_t next_idx = (ring_idx + 1u) & 0xFFFFu;
  StoreBeU16(st + 388u, uint16_t(int32_t(next_idx) % win_len));
  Card(base, r3 + 388u, 2u);
  ctx.fpscr.disableFlushMode();
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[4096]{}, buf_nat[4096]{};
  int failures = 0;
  auto test_case = [&](const char* label, uint16_t start_idx) {
    std::memset(buf_orig, 0, sizeof(buf_orig));
    constexpr u32 kObj = 0x300u;
    constexpr u32 kDesc = 0x600u;
    constexpr u32 kData = 0x700u;
    buf_orig[kObj + 42u] = 2u;
    const u32 win_be = __builtin_bswap32(4u);
    std::memcpy(buf_orig + kObj + 376u, &win_be, 4);
    StoreBeU16(buf_orig + kObj + 384u, 400u);
    StoreBeU16(buf_orig + kObj + 386u, 464u);
    StoreBeU16(buf_orig + kObj + 388u, start_idx);
    StoreBeF32(buf_orig + kObj + 240u, 0.12f);
    StoreBeF32(buf_orig + kObj + 244u, 0.07f);
    StoreBeF32(buf_orig + kObj + 272u, 0.20f);
    StoreBeF32(buf_orig + kObj + 276u, 0.15f);
    StoreBeF32(buf_orig + kObj + 336u, 0.40f);
    StoreBeF32(buf_orig + kObj + 340u, 0.90f);
    StoreBeF32(buf_orig + kObj + 400u + (start_idx << 2), 0.05f);
    StoreBeF32(buf_orig + kObj + 400u + ((4u + start_idx) << 2), 0.03f);
    const u32 data_be = __builtin_bswap32(kData);
    std::memcpy(buf_orig + kDesc + 4u, &data_be, 4);
    StoreBeU16(buf_orig + kDesc + 14u, 256u);
    for (u32 ch = 0; ch < 2u; ++ch) {
      for (u32 i = 0; i < 256u; ++i) {
        StoreBeF32(buf_orig + kData + ch * 1024u + (i << 2), 0.75f * std::sin(0.23f * float(i) + float(ch)));
      }
    }
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{}, nat{};
    orig.r1.u64 = nat.r1.u64 = 0x200u;
    orig.r3.u64 = nat.r3.u64 = kObj;
    orig.r4.u64 = nat.r4.u64 = kDesc;
    ::__imp__sub_82AA02B0(orig, buf_orig);
    Impl(nat, buf_nat, kObj, kDesc);
    for (u32 off = 240u; off < 528u; off += 4u) {
      if (off == 388u) continue;
      const float e = LoadBeF32(buf_orig + kObj + off);
      const float g = LoadBeF32(buf_nat + kObj + off);
      if (std::isnan(g) || std::fabs(e - g) > 1e-5f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82AA02B0 self-test ({}) off={} got={} exp={}", label, off, g, e);
        return;
      }
    }
    if (LoadBeU16(buf_orig + kObj + 388u) != LoadBeU16(buf_nat + kObj + 388u)) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_82AA02B0 self-test ({}) ring_idx", label);
    }
  };
  test_case("step", 1u);
  test_case("wrap", 3u);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82AA02B0 RMS/peak meter self-test: 2/2 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t r3 = ctx.r3.u32;
  const uint32_t r4 = ctx.r4.u32;
  if (r3 < 0x1000u || r4 < 0x1000u) {
    ::__imp__sub_82AA02B0(ctx, base);
    return;
  }
  const uint8_t* st = Raw(base, r3);
  const int32_t win_len = int32_t(LoadBeU32(st + 376u));
  const uint32_t num_ch = st[42];
  if (win_len <= 0 || num_ch > 8u) {
    ::__imp__sub_82AA02B0(ctx, base);
    return;
  }
  if (num_ch != 0u) {
    const uint32_t data_va = LoadBeU32(Raw(base, r4 + 4u));
    const uint32_t stride_bytes = uint32_t(LoadBeU16(Raw(base, r4 + 14u))) << 2;
    const uint64_t span = uint64_t(num_ch - 1u) * stride_bytes + 1024ull;
    if (data_va < 0x1000u || uint64_t(data_va) + span > 0x100000000ull ||
        (data_va < 0xE0000000u && uint64_t(data_va) + span > 0xE0000000u)) {
      ::__imp__sub_82AA02B0(ctx, base);
      return;
    }
  }
  Impl(ctx, base, r3, r4);
}

}  // namespace n_82AA02B0

// 11. sub_82A8BBC0: Linear audio resampler (4.0% Job 4 / 0.69% total CPU).
namespace n_82A8BBC0 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

[[gnu::noinline]] void ResampleFast(uint8_t* __restrict dst_be, const uint8_t* __restrict src_be, uint32_t count,
                                    uint32_t& idx_io, uint32_t& frac_io, uint32_t step) noexcept {
  constexpr uint32_t kInvBits = 0x377FFC9Cu;
  float inv_65536;
  std::memcpy(&inv_65536, &kInvBits, 4);
  uint32_t idx = idx_io;
  uint32_t frac = frac_io;
  uint32_t i = 0;
  for (; i + 4u <= count; i += 4u) {
    const float a0 = LoadBeF32(src_be + (idx << 2));
    const float b0 = LoadBeF32(src_be + ((idx + 1u) << 2));
    const float y0 = std::fmaf(b0 - a0, float(frac) * inv_65536, a0);
    uint32_t next = frac + step;
    idx += next >> 16;
    frac = next & 0xFFFFu;

    const float a1 = LoadBeF32(src_be + (idx << 2));
    const float b1 = LoadBeF32(src_be + ((idx + 1u) << 2));
    const float y1 = std::fmaf(b1 - a1, float(frac) * inv_65536, a1);
    next = frac + step;
    idx += next >> 16;
    frac = next & 0xFFFFu;

    const float a2 = LoadBeF32(src_be + (idx << 2));
    const float b2 = LoadBeF32(src_be + ((idx + 1u) << 2));
    const float y2 = std::fmaf(b2 - a2, float(frac) * inv_65536, a2);
    next = frac + step;
    idx += next >> 16;
    frac = next & 0xFFFFu;

    const float a3 = LoadBeF32(src_be + (idx << 2));
    const float b3 = LoadBeF32(src_be + ((idx + 1u) << 2));
    const float y3 = std::fmaf(b3 - a3, float(frac) * inv_65536, a3);
    next = frac + step;
    idx += next >> 16;
    frac = next & 0xFFFFu;

#if defined(__aarch64__)
    const float32x4_t yv = {y0, y1, y2, y3};
    vst1q_u8(dst_be + (i << 2), vrev32q_u8(vreinterpretq_u8_f32(yv)));
#else
    StoreBeF32(dst_be + ((i + 0u) << 2), y0);
    StoreBeF32(dst_be + ((i + 1u) << 2), y1);
    StoreBeF32(dst_be + ((i + 2u) << 2), y2);
    StoreBeF32(dst_be + ((i + 3u) << 2), y3);
#endif
  }
  for (; i < count; ++i) {
    const float a = LoadBeF32(src_be + (idx << 2));
    const float b = LoadBeF32(src_be + ((idx + 1u) << 2));
    StoreBeF32(dst_be + (i << 2), std::fmaf(b - a, float(frac) * inv_65536, a));
    const uint32_t next = frac + step;
    idx += next >> 16;
    frac = next & 0xFFFFu;
  }
  idx_io = idx;
  frac_io = frac;
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[4096]{}, buf_nat[4096]{};
  int failures = 0;
  auto test_case = [&](const char* label, u32 count, u32 init_idx, u16 init_frac, u32 step) {
    std::memset(buf_orig, 0, sizeof(buf_orig));
    constexpr u32 kSrc = 0x400u, kDst = 0xA00u, kIdx = 0x300u, kFrac = 0x304u;
    for (u32 i = 0; i < 256u; ++i) {
      StoreBeF32(buf_orig + kSrc + (i << 2), std::sin(0.17f * float(i)) * 0.8f);
    }
    const u32 idx_be = __builtin_bswap32(init_idx);
    const u32 frac_be = __builtin_bswap32(uint32_t(init_frac) << 16);
    std::memcpy(buf_orig + kIdx, &idx_be, 4);
    std::memcpy(buf_orig + kFrac, &frac_be, 4);
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{};
    orig.r1.u64 = 0x200u;
    orig.r3.u64 = count;
    orig.r4.u64 = kSrc;
    orig.r5.u64 = kDst;
    orig.r6.u64 = kIdx;
    orig.r7.u64 = kFrac;
    orig.r8.u64 = step;
    ::__imp__sub_82A8BBC0(orig, buf_orig);

    uint32_t idx = init_idx;
    uint32_t frac = init_frac;
    ResampleFast(buf_nat + kDst, buf_nat + kSrc, count, idx, frac, step);
    const u32 out_idx = __builtin_bswap32(idx);
    const u32 out_frac = __builtin_bswap32(frac << 16);
    std::memcpy(buf_nat + kIdx, &out_idx, 4);
    std::memcpy(buf_nat + kFrac, &out_frac, 4);

    if (std::memcmp(buf_orig + kIdx, buf_nat + kIdx, 8u) != 0) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_82A8BBC0 self-test ({}) idx/frac", label);
      return;
    }
    for (u32 i = 0; i < count; ++i) {
      const float e = LoadBeF32(buf_orig + kDst + (i << 2));
      const float g = LoadBeF32(buf_nat + kDst + (i << 2));
      if (std::isnan(g) || std::fabs(e - g) > 1e-6f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82A8BBC0 self-test ({}) i={} got={} exp={}", label, i, g, e);
        return;
      }
    }
  };
  test_case("tail19_up", 19u, 2u, 0x3456u, 0x00014321u);
  test_case("vec32_down", 32u, 0u, 0x8000u, 0x0000B333u);
  test_case("zero", 0u, 5u, 0x1234u, 0x00010000u);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82A8BBC0 resampler self-test: 3/3 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t count = ctx.r3.u32;
  const uint32_t src_va = ctx.r4.u32;
  const uint32_t dst_va = ctx.r5.u32;
  const uint32_t p_idx = ctx.r6.u32;
  const uint32_t p_frac = ctx.r7.u32;
  const uint32_t step = ctx.r8.u32;
  if (count > 65535u || src_va < 0x1000u || dst_va < 0x1000u || p_idx < 0x1000u || p_frac < 0x1000u) {
    ::__imp__sub_82A8BBC0(ctx, base);
    return;
  }
  uint32_t idx = REX_LOAD_U32(p_idx);
  uint32_t frac = REX_LOAD_U16(p_frac);
  const uint64_t dst_bytes = uint64_t(count) << 2;
  const uint64_t end_pos = (uint64_t(idx) << 16) + frac + uint64_t(count) * step;
  const uint64_t max_src_idx = (end_pos >> 16) + 2ull;
  const uint64_t src_bytes = max_src_idx << 2;
  const bool span_ok = uint64_t(dst_va) + dst_bytes <= 0x100000000ull && uint64_t(src_va) + src_bytes <= 0x100000000ull &&
                       !(dst_va < 0xE0000000u && uint64_t(dst_va) + dst_bytes > 0xE0000000u) &&
                       !(src_va < 0xE0000000u && uint64_t(src_va) + src_bytes > 0xE0000000u) &&
                       (uint64_t(dst_va) + dst_bytes <= src_va || uint64_t(src_va) + src_bytes <= dst_va);
  if (!span_ok) {
    ::__imp__sub_82A8BBC0(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  if (count != 0u) {
    ResampleFast(Raw(base, dst_va), Raw(base, src_va), count, idx, frac, step);
    Card(base, dst_va, uint32_t(dst_bytes));
  }
  REX_STORE_U32(p_frac, frac << 16);
  REX_STORE_U32(p_idx, idx);
  Card(base, p_frac, 4u);
  Card(base, p_idx, 4u);
}

}  // namespace n_82A8BBC0

// 12. sub_82A85B58: Audio gain ramp (first 64 samples) + constant gain scale (2.2% Job 4 / 0.41% total CPU).
namespace n_82A85B58 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

[[gnu::noinline]] void GainRampFast(uint8_t* dst_be, const uint8_t* src_be, uint32_t count, float g0, float dg) noexcept {
#if defined(__aarch64__)
  float32x4_t vg = {g0, g0 + dg, std::fmaf(dg, 2.0f, g0), std::fmaf(dg, 3.0f, g0)};
  const float32x4_t vstep = vdupq_n_f32(dg * 4.0f);
  for (uint32_t i = 0; i < 64u; i += 4u) {
    const uint32_t off = i << 2;
    const float32x4_t s = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be + off)));
    vst1q_u8(dst_be + off, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(s, vg))));
    vg = vaddq_f32(vg, vstep);
  }
  const float32x4_t vc = vdupq_n_f32(std::fmaf(dg, 64.0f, g0));
  for (uint32_t i = 64u; i < count; i += 16u) {
    const uint32_t off = i << 2;
    const float32x4_t s0 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be + off + 0u)));
    const float32x4_t s1 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be + off + 16u)));
    const float32x4_t s2 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be + off + 32u)));
    const float32x4_t s3 = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be + off + 48u)));
    vst1q_u8(dst_be + off + 0u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(s0, vc))));
    vst1q_u8(dst_be + off + 16u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(s1, vc))));
    vst1q_u8(dst_be + off + 32u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(s2, vc))));
    vst1q_u8(dst_be + off + 48u, vrev32q_u8(vreinterpretq_u8_f32(vmulq_f32(s3, vc))));
  }
#else
  for (uint32_t i = 0; i < 64u; ++i) {
    StoreBeF32(dst_be + (i << 2), LoadBeF32(src_be + (i << 2)) * std::fmaf(dg, float(i), g0));
  }
  const float gc = std::fmaf(dg, 64.0f, g0);
  for (uint32_t i = 64u; i < count; ++i) {
    StoreBeF32(dst_be + (i << 2), LoadBeF32(src_be + (i << 2)) * gc);
  }
#endif
}

inline void RunSelfTest() {
  alignas(128) uint8_t buf_orig[4096]{}, buf_nat[4096]{};
  int failures = 0;
  auto test_case = [&](const char* label, u32 dst_va, u32 src_va, u32 count, double g0, double dg) {
    std::memset(buf_orig, 0, sizeof(buf_orig));
    for (u32 i = 0; i < count; ++i) {
      StoreBeF32(buf_orig + src_va + (i << 2), std::sin(0.13f * float(i)) * 0.7f);
    }
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{};
    orig.r1.u64 = 0x200u;
    orig.r3.u64 = dst_va;
    orig.r4.u64 = src_va;
    orig.r7.u64 = count;
    orig.f1.f64 = g0;
    orig.f2.f64 = dg;
    ::__imp__sub_82A85B58(orig, buf_orig);
    GainRampFast(buf_nat + dst_va, buf_nat + src_va, count, float(g0), float(dg));
    for (u32 i = 0; i < count; ++i) {
      const float e = LoadBeF32(buf_orig + dst_va + (i << 2));
      const float g = LoadBeF32(buf_nat + dst_va + (i << 2));
      if (std::isnan(g) || std::fabs(e - g) > 1e-5f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82A85B58 self-test ({}) i={} got={} exp={}", label, i, g, e);
        return;
      }
    }
  };
  test_case("out256", 0x400u, 0x900u, 256u, 0.25, 0.01);
  test_case("inplace96", 0x400u, 0x400u, 96u, 1.0, -0.005);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82A85B58 gain ramp self-test: 2/2 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t dst_va = ctx.r3.u32;
  const uint32_t src_va = ctx.r4.u32;
  const uint32_t count = ctx.r7.u32;
  const uint64_t bytes = uint64_t(count) << 2;
  const bool ok = ((dst_va | src_va) & 15u) == 0u && count >= 96u && (count & 31u) == 0u &&
                  dst_va >= 0x1000u && src_va >= 0x1000u &&
                  uint64_t(dst_va) + bytes <= 0x100000000ull && uint64_t(src_va) + bytes <= 0x100000000ull &&
                  !(dst_va < 0xE0000000u && uint64_t(dst_va) + bytes > 0xE0000000u) &&
                  !(src_va < 0xE0000000u && uint64_t(src_va) + bytes > 0xE0000000u) &&
                  (dst_va == src_va || uint64_t(dst_va) + bytes <= src_va || uint64_t(src_va) + bytes <= dst_va);
  if (!ok) {
    ::__imp__sub_82A85B58(ctx, base);
    return;
  }
  Card(base, dst_va, uint32_t(bytes));
  ctx.fpscr.enableFlushMode();
  GainRampFast(Raw(base, dst_va), Raw(base, src_va), count, float(ctx.f1.f64), float(ctx.f2.f64));
}

}  // namespace n_82A85B58

// 13. sub_82440038: SQT transform compose (0.71-1.12% total CPU across FGGameSim & Job workers).
namespace n_82440038 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

#if defined(__aarch64__)
inline float32x4_t LoadBeF4(const uint8_t* p) noexcept {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
}

inline void StoreBeF4(uint8_t* p, float32x4_t v) noexcept {
  vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v)));
}

inline float32x4_t Yzx(float32x4_t v) noexcept {
  const float32x4_t yzwx = vextq_f32(v, v, 1);
  return vcombine_f32(vget_low_f32(yzwx), vrev64_f32(vget_high_f32(yzwx)));
}

inline float32x4_t Cross3(float32x4_t a, float32x4_t b) noexcept {
  const float32x4_t a_yzx = Yzx(a);
  const float32x4_t b_yzx = Yzx(b);
  return Yzx(vfmsq_f32(vmulq_f32(a, b_yzx), a_yzx, b));
}
#endif

[[gnu::noinline]] void ComposeFast(uint8_t* dst, const uint8_t* a, const uint8_t* b) noexcept {
#if defined(__aarch64__)
  const float32x4_t sA = LoadBeF4(a + 0u);
  const float32x4_t qA = LoadBeF4(a + 16u);
  const float32x4_t pA = LoadBeF4(a + 32u);
  const float32x4_t sB = LoadBeF4(b + 0u);
  const float32x4_t qB = LoadBeF4(b + 16u);
  const float32x4_t pB = LoadBeF4(b + 32u);

  const float32x4_t out_s = vmulq_f32(sB, sA);
  const float32x4_t q_xyz = vfmaq_laneq_f32(vfmaq_laneq_f32(Cross3(qB, qA), qA, qB, 3), qB, qA, 3);
  const float32x4_t q_prod = vmulq_f32(qB, qA);
  const float dot3 = vgetq_lane_f32(q_prod, 0) + vgetq_lane_f32(q_prod, 1) + vgetq_lane_f32(q_prod, 2);
  const float32x4_t out_q = vsetq_lane_f32(vgetq_lane_f32(qB, 3) * vgetq_lane_f32(qA, 3) - dot3, q_xyz, 3);

  const float32x4_t t = vmulq_f32(pA, sB);
  const float32x4_t c1 = vfmaq_laneq_f32(Cross3(qB, t), t, qB, 3);
  const float32x4_t c2 = Cross3(qB, c1);
  const float32x4_t out_p = vaddq_f32(pB, vfmaq_f32(t, vdupq_n_f32(2.0f), c2));

  StoreBeF4(dst + 0u, out_s);
  StoreBeF4(dst + 16u, out_q);
  StoreBeF4(dst + 32u, out_p);
#else
  float sA[4], qA[4], pA[4], sB[4], qB[4], pB[4];
  for (int i = 0; i < 4; ++i) {
    sA[i] = LoadBeF32(a + i * 4);
    qA[i] = LoadBeF32(a + 16 + i * 4);
    pA[i] = LoadBeF32(a + 32 + i * 4);
    sB[i] = LoadBeF32(b + i * 4);
    qB[i] = LoadBeF32(b + 16 + i * 4);
    pB[i] = LoadBeF32(b + 32 + i * 4);
  }
  auto cross = [](const float u[3], const float v[3], float o[3]) {
    o[0] = u[1] * v[2] - u[2] * v[1];
    o[1] = u[2] * v[0] - u[0] * v[2];
    o[2] = u[0] * v[1] - u[1] * v[0];
  };
  float out_s[4], out_q[4], out_p[4];
  for (int i = 0; i < 4; ++i) out_s[i] = sB[i] * sA[i];
  float cq[3];
  cross(qB, qA, cq);
  for (int i = 0; i < 3; ++i) out_q[i] = cq[i] + qB[3] * qA[i] + qA[3] * qB[i];
  out_q[3] = qB[3] * qA[3] - (qB[0] * qA[0] + qB[1] * qA[1] + qB[2] * qA[2]);
  float t[4], c1[3], c2[3];
  for (int i = 0; i < 4; ++i) t[i] = pA[i] * sB[i];
  cross(qB, t, c1);
  for (int i = 0; i < 3; ++i) c1[i] += qB[3] * t[i];
  cross(qB, c1, c2);
  for (int i = 0; i < 3; ++i) out_p[i] = pB[i] + (2.0f * c2[i] + t[i]);
  out_p[3] = pB[3] + t[3];
  for (int i = 0; i < 4; ++i) {
    StoreBeF32(dst + i * 4, out_s[i]);
    StoreBeF32(dst + 16 + i * 4, out_q[i]);
    StoreBeF32(dst + 32 + i * 4, out_p[i]);
  }
#endif
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[512]{}, buf_nat[512]{};
  int failures = 0;
  auto test_case = [&](const char* label, u32 dst_va, u32 a_va, u32 b_va) {
    std::memset(buf_orig, 0, sizeof(buf_orig));
    const float a_vals[12] = {1.2f, 0.8f, 1.5f, 1.0f, 0.1f, -0.2f, 0.3f, 0.9273618f, 3.5f, -1.25f, 2.0f, 1.0f};
    const float b_vals[12] = {0.9f, 1.1f, 0.7f, 1.0f, -0.3f, 0.4f, -0.1f, 0.8602325f, -4.0f, 2.5f, 1.75f, 0.5f};
    for (u32 i = 0; i < 12u; ++i) {
      StoreBeF32(buf_orig + a_va + (i << 2), a_vals[i]);
      StoreBeF32(buf_orig + b_va + (i << 2), b_vals[i]);
    }
    std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));
    PPCContext orig{};
    orig.r3.u64 = dst_va;
    orig.r4.u64 = a_va;
    orig.r5.u64 = b_va;
    ::__imp__sub_82440038(orig, buf_orig);
    ComposeFast(buf_nat + dst_va, buf_nat + a_va, buf_nat + b_va);
    for (u32 i = 0; i < 12u; ++i) {
      const float e = LoadBeF32(buf_orig + dst_va + (i << 2));
      const float g = LoadBeF32(buf_nat + dst_va + (i << 2));
      if (std::isnan(g) || std::fabs(e - g) > 1e-5f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_82440038 self-test ({}) i={} got={} exp={}", label, i, g, e);
        return;
      }
    }
  };
  test_case("separate", 0x100u, 0x040u, 0x080u);
  test_case("alias_a", 0x040u, 0x040u, 0x080u);
  test_case("alias_b", 0x080u, 0x040u, 0x080u);
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82440038 transform compose self-test: 3/3 passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint32_t dst_va = ctx.r3.u32 & ~0xFu;
  const uint32_t a_va = ctx.r4.u32 & ~0xFu;
  const uint32_t b_va = ctx.r5.u32 & ~0xFu;
  if (dst_va < 0x1000u || a_va < 0x1000u || b_va < 0x1000u) {
    ::__imp__sub_82440038(ctx, base);
    return;
  }
  ComposeFast(Raw(base, dst_va), Raw(base, a_va), Raw(base, b_va));
  Card(base, dst_va, 48u);
  ctx.fpscr.enableFlushMode();
}

}  // namespace n_82440038

// 14. sub_82B85E10: 4-wide cloth/physics distance constraint solver (2.10% total CPU).
namespace n_82B85E10 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

inline int16_t LoadBeS16(const uint8_t* p) noexcept {
  uint16_t raw;
  std::memcpy(&raw, p, 2);
  return int16_t(__builtin_bswap16(raw));
}

#if defined(__aarch64__)
inline float32x4_t LoadBeF4(const uint8_t* p) noexcept {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(p)));
}

inline void StoreBeF4(uint8_t* p, float32x4_t v) noexcept {
  vst1q_u8(p, vrev32q_u8(vreinterpretq_u8_f32(v)));
}

inline void Transpose4x4(float32x4_t r0, float32x4_t r1, float32x4_t r2, float32x4_t r3, float32x4_t& c0,
                         float32x4_t& c1, float32x4_t& c2, float32x4_t& c3) noexcept {
  const float32x4_t t0 = vtrn1q_f32(r0, r1);
  const float32x4_t t1 = vtrn2q_f32(r0, r1);
  const float32x4_t t2 = vtrn1q_f32(r2, r3);
  const float32x4_t t3 = vtrn2q_f32(r2, r3);
  c0 = vreinterpretq_f32_f64(vzip1q_f64(vreinterpretq_f64_f32(t0), vreinterpretq_f64_f32(t2)));
  c2 = vreinterpretq_f32_f64(vzip2q_f64(vreinterpretq_f64_f32(t0), vreinterpretq_f64_f32(t2)));
  c1 = vreinterpretq_f32_f64(vzip1q_f64(vreinterpretq_f64_f32(t1), vreinterpretq_f64_f32(t3)));
  c3 = vreinterpretq_f32_f64(vzip2q_f64(vreinterpretq_f64_f32(t1), vreinterpretq_f64_f32(t3)));
}
#endif

[[gnu::noinline]] void SolveConstraintsFast(uint8_t* base, uint32_t posA_va, uint32_t posB_va, uint32_t con_va,
                                            int32_t count4, float stiff_stretch, float stiff_compress) noexcept {
  const uint8_t* cp = Raw(base, con_va);
#if defined(__aarch64__)
  const float32x4_t vzero = vdupq_n_f32(0.0f);
  const float32x4_t vone = vdupq_n_f32(1.0f);
  const float32x4_t vf1 = vdupq_n_f32(stiff_stretch);
  const float32x4_t vf2 = vdupq_n_f32(stiff_compress);
  for (int32_t iter = 0; iter < count4; ++iter, cp += 32u) {
    const uint32_t eaA0 = (posA_va + uint32_t(int32_t(LoadBeS16(cp + 0u)) << 4)) & ~0xFu;
    const uint32_t eaB0 = (posB_va + uint32_t(int32_t(LoadBeS16(cp + 2u)) << 4)) & ~0xFu;
    const uint32_t eaA1 = (posA_va + uint32_t(int32_t(LoadBeS16(cp + 8u)) << 4)) & ~0xFu;
    const uint32_t eaB1 = (posB_va + uint32_t(int32_t(LoadBeS16(cp + 10u)) << 4)) & ~0xFu;
    const uint32_t eaA2 = (posA_va + uint32_t(int32_t(LoadBeS16(cp + 16u)) << 4)) & ~0xFu;
    const uint32_t eaB2 = (posB_va + uint32_t(int32_t(LoadBeS16(cp + 18u)) << 4)) & ~0xFu;
    const uint32_t eaA3 = (posA_va + uint32_t(int32_t(LoadBeS16(cp + 24u)) << 4)) & ~0xFu;
    const uint32_t eaB3 = (posB_va + uint32_t(int32_t(LoadBeS16(cp + 26u)) << 4)) & ~0xFu;

    const float32x4_t rest = {LoadBeF32(cp + 4u), LoadBeF32(cp + 12u), LoadBeF32(cp + 20u), LoadBeF32(cp + 28u)};
    float32x4_t bx, by, bz, bw, ax, ay, az, aw;
    Transpose4x4(LoadBeF4(Raw(base, eaB0)), LoadBeF4(Raw(base, eaB1)), LoadBeF4(Raw(base, eaB2)),
                 LoadBeF4(Raw(base, eaB3)), bx, by, bz, bw);
    Transpose4x4(LoadBeF4(Raw(base, eaA0)), LoadBeF4(Raw(base, eaA1)), LoadBeF4(Raw(base, eaA2)),
                 LoadBeF4(Raw(base, eaA3)), ax, ay, az, aw);

    const float32x4_t dx = vsubq_f32(bx, ax);
    const float32x4_t dy = vsubq_f32(by, ay);
    const float32x4_t dz = vsubq_f32(bz, az);
    const uint32x4_t sum_pos = vcgtq_f32(vaddq_f32(aw, bw), vzero);
    const float32x4_t eff_wA = vbslq_f32(sum_pos, aw, vone);
    const float32x4_t eff_wB = vbslq_f32(sum_pos, bw, vone);
    const float32x4_t inv_sum = vdivq_f32(vone, vaddq_f32(eff_wA, eff_wB));

    const float32x4_t dist2 = vfmaq_f32(vfmaq_f32(vmulq_f32(dz, dz), dy, dy), dx, dx);
    const uint32x4_t is_zero = vceqq_f32(dist2, vzero);
    const float32x4_t inv_dist = vdivq_f32(vone, vsqrtq_f32(dist2));
    const float32x4_t diff = vmulq_f32(vsubq_f32(rest, vmulq_f32(dist2, inv_dist)), inv_dist);
    const float32x4_t stiff = vbslq_f32(vcgtq_f32(diff, vzero), vf2, vf1);
    const float32x4_t lambda = vbslq_f32(is_zero, vzero, vmulq_f32(diff, stiff));
    const float32x4_t corr = vmulq_f32(lambda, inv_sum);
    const float32x4_t cA = vmulq_f32(eff_wA, corr);
    const float32x4_t cB = vmulq_f32(eff_wB, corr);

    const float32x4_t nbx = vfmaq_f32(bx, dx, cB);
    const float32x4_t nby = vfmaq_f32(by, dy, cB);
    const float32x4_t nbz = vfmaq_f32(bz, dz, cB);
    const float32x4_t nax = vfmsq_f32(ax, dx, cA);
    const float32x4_t nay = vfmsq_f32(ay, dy, cA);
    const float32x4_t naz = vfmsq_f32(az, dz, cA);

    float32x4_t ob0, ob1, ob2, ob3, oa0, oa1, oa2, oa3;
    Transpose4x4(nbx, nby, nbz, bw, ob0, ob1, ob2, ob3);
    Transpose4x4(nax, nay, naz, aw, oa0, oa1, oa2, oa3);

    StoreBeF4(Raw(base, eaB0), ob0);
    StoreBeF4(Raw(base, eaB1), ob1);
    StoreBeF4(Raw(base, eaB2), ob2);
    StoreBeF4(Raw(base, eaB3), ob3);
    StoreBeF4(Raw(base, eaA0), oa0);
    StoreBeF4(Raw(base, eaA1), oa1);
    StoreBeF4(Raw(base, eaA2), oa2);
    StoreBeF4(Raw(base, eaA3), oa3);
    Card(base, eaB0, 16u);
    Card(base, eaB1, 16u);
    Card(base, eaB2, 16u);
    Card(base, eaB3, 16u);
    Card(base, eaA0, 16u);
    Card(base, eaA1, 16u);
    Card(base, eaA2, 16u);
    Card(base, eaA3, 16u);
  }
#else
  for (int32_t iter = 0; iter < count4; ++iter, cp += 32u) {
    uint32_t eaA[4], eaB[4];
    float A[4][4], B[4][4], rest[4];
    for (int k = 0; k < 4; ++k) {
      eaA[k] = (posA_va + uint32_t(int32_t(LoadBeS16(cp + k * 8 + 0)) << 4)) & ~0xFu;
      eaB[k] = (posB_va + uint32_t(int32_t(LoadBeS16(cp + k * 8 + 2)) << 4)) & ~0xFu;
      rest[k] = LoadBeF32(cp + k * 8 + 4);
      for (int c = 0; c < 4; ++c) {
        B[k][c] = LoadBeF32(Raw(base, eaB[k]) + c * 4);
        A[k][c] = LoadBeF32(Raw(base, eaA[k]) + c * 4);
      }
    }
    for (int k = 0; k < 4; ++k) {
      const float dx = B[k][0] - A[k][0], dy = B[k][1] - A[k][1], dz = B[k][2] - A[k][2];
      const bool pos_w = (A[k][3] + B[k][3]) > 0.0f;
      const float ewA = pos_w ? A[k][3] : 1.0f;
      const float ewB = pos_w ? B[k][3] : 1.0f;
      const float inv_sum = 1.0f / (ewA + ewB);
      const float dist2 = dx * dx + dy * dy + dz * dz;
      float lam = 0.0f;
      if (dist2 != 0.0f) {
        const float inv_d = 1.0f / std::sqrt(dist2);
        const float diff = (rest[k] - dist2 * inv_d) * inv_d;
        lam = diff * ((diff > 0.0f) ? stiff_compress : stiff_stretch);
      }
      const float corr = lam * inv_sum;
      B[k][0] += dx * (ewB * corr);
      B[k][1] += dy * (ewB * corr);
      B[k][2] += dz * (ewB * corr);
      A[k][0] -= dx * (ewA * corr);
      A[k][1] -= dy * (ewA * corr);
      A[k][2] -= dz * (ewA * corr);
    }
    for (int k = 0; k < 4; ++k) {
      for (int c = 0; c < 4; ++c) StoreBeF32(Raw(base, eaB[k]) + c * 4, B[k][c]);
      Card(base, eaB[k], 16u);
    }
    for (int k = 0; k < 4; ++k) {
      for (int c = 0; c < 4; ++c) StoreBeF32(Raw(base, eaA[k]) + c * 4, A[k][c]);
      Card(base, eaA[k], 16u);
    }
  }
#endif
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf_orig[2048]{}, buf_nat[2048]{};
  constexpr u32 kPos = 0x400u, kCon = 0x600u;
  const float pts[8][4] = {
      {0.0f, 0.0f, 0.0f, 1.0f},  {2.0f, 0.5f, -0.5f, 0.5f},
      {1.0f, 1.0f, 1.0f, 0.25f}, {1.2f, 1.1f, 0.9f, 0.75f},
      {3.0f, 3.0f, 3.0f, 1.0f},  {3.0f, 3.0f, 3.0f, 1.0f},   // dist2 == 0
      {-1.0f, 0.0f, 2.0f, 0.0f}, {0.5f, -1.0f, 2.0f, 0.0f},  // Aw + Bw == 0
  };
  for (u32 i = 0; i < 8u; ++i) {
    for (u32 c = 0; c < 4u; ++c) StoreBeF32(buf_orig + kPos + i * 16u + c * 4u, pts[i][c]);
  }
  auto put_con = [&](u32 idx, int16_t a, int16_t b, float rest) {
    const uint16_t ab = __builtin_bswap16(uint16_t(a));
    const uint16_t bb = __builtin_bswap16(uint16_t(b));
    std::memcpy(buf_orig + kCon + idx * 8u + 0u, &ab, 2);
    std::memcpy(buf_orig + kCon + idx * 8u + 2u, &bb, 2);
    StoreBeF32(buf_orig + kCon + idx * 8u + 4u, rest);
  };
  put_con(0, 0, 1, 1.0f);   // stretch
  put_con(1, 2, 3, 1.5f);   // compress
  put_con(2, 4, 5, 0.5f);   // zero distance
  put_con(3, 6, 7, 1.0f);   // zero invMass sum
  put_con(4, 0, 2, 1.2f);   // shared/aliased vertices across batch
  put_con(5, 1, 2, 0.9f);
  put_con(6, 3, 0, 1.8f);
  put_con(7, 2, 1, 1.1f);
  std::memcpy(buf_nat, buf_orig, sizeof(buf_orig));

  PPCContext orig{};
  orig.r1.u64 = 0x200u;
  orig.r3.u64 = kPos;
  orig.r4.u64 = kPos;
  orig.r5.u64 = kCon;
  orig.r6.u64 = 8u;
  orig.f1.f64 = 0.8;
  orig.f2.f64 = 0.3;
  ::__imp__sub_82B85E10(orig, buf_orig);
  SolveConstraintsFast(buf_nat, kPos, kPos, kCon, 2, 0.8f, 0.3f);

  int failures = 0;
  for (u32 i = 0; i < 32u; ++i) {
    const float e = LoadBeF32(buf_orig + kPos + (i << 2));
    const float g = LoadBeF32(buf_nat + kPos + (i << 2));
    if (std::isnan(g) || std::fabs(e - g) > 1e-5f) {
      ++failures;
      REXLOG_ERROR("[hot] DIFFERENCE sub_82B85E10 self-test idx={} got={} exp={}", i, g, e);
      break;
    }
  }
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_82B85E10 distance constraints self-test: passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const int32_t count4 = ctx.r6.s32 / 4;
  if (count4 <= 0) return;
  const uint32_t posA_va = ctx.r3.u32;
  const uint32_t posB_va = ctx.r4.u32;
  const uint32_t con_va = ctx.r5.u32;
  if (posA_va < 0x1000u || posB_va < 0x1000u || con_va < 0x1000u) {
    ::__imp__sub_82B85E10(ctx, base);
    return;
  }
  ctx.fpscr.enableFlushMode();
  SolveConstraintsFast(base, posA_va, posB_va, con_va, count4, float(ctx.f1.f64), float(ctx.f2.f64));
}

}  // namespace n_82B85E10

// 15. sub_826D61A0: DMA morph-target / blendshape vertex accumulator (0.91-1.08% total CPU).
namespace n_826D61A0 {

inline float LoadBeF32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  raw = __builtin_bswap32(raw);
  float f;
  std::memcpy(&f, &raw, 4);
  return f;
}

inline void StoreBeF32(uint8_t* p, float f) noexcept {
  uint32_t raw;
  std::memcpy(&raw, &f, 4);
  raw = __builtin_bswap32(raw);
  std::memcpy(p, &raw, 4);
}

inline uint32_t LoadBeU32(const uint8_t* p) noexcept {
  uint32_t raw;
  std::memcpy(&raw, p, 4);
  return __builtin_bswap32(raw);
}

inline uint16_t LoadBeU16(const uint8_t* p) noexcept {
  uint16_t raw;
  std::memcpy(&raw, p, 2);
  return __builtin_bswap16(raw);
}

[[gnu::noinline]] void AccumSingleStream(uint8_t* __restrict dst_be, const uint8_t* __restrict src_be, int32_t stride,
                                         int32_t count, const float w[4]) noexcept {
#if defined(__aarch64__)
  const float32x4_t vw = vld1q_f32(w);
  for (int32_t i = 0; i < count; ++i) {
    const float32x4_t s = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(src_be)));
    const float32x4_t d = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(dst_be)));
    vst1q_u8(dst_be, vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(d, vw, s))));
    dst_be += 16;
    src_be += stride;
  }
#else
  for (int32_t i = 0; i < count; ++i) {
    for (int c = 0; c < 4; ++c) {
      const float s = LoadBeF32(src_be + c * 4);
      const float d = LoadBeF32(dst_be + c * 4);
      StoreBeF32(dst_be + c * 4, std::fmaf(w[c], s, d));
    }
    dst_be += 16;
    src_be += stride;
  }
#endif
}

[[gnu::noinline]] void AccumMultiStream(uint8_t* base, const uint32_t* out_streams, const uint32_t* src_offs,
                                        const uint32_t* dst_idxs, int32_t num_streams, uint32_t dst_byte_off,
                                        uint32_t src_va, int32_t stride, int32_t count, const float w[4]) noexcept {
#if defined(__aarch64__)
  const float32x4_t vw = vld1q_f32(w);
  for (int32_t i = 0; i < count; ++i) {
    for (int32_t s = 0; s < num_streams; ++s) {
      const uint32_t sea = (src_va + src_offs[s]) & ~0xFu;
      const uint32_t dea = (out_streams[dst_idxs[s]] + dst_byte_off) & ~0xFu;
      const float32x4_t sv = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(Raw(base, sea))));
      const float32x4_t dv = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(Raw(base, dea))));
      vst1q_u8(Raw(base, dea), vrev32q_u8(vreinterpretq_u8_f32(vfmaq_f32(dv, vw, sv))));
    }
    dst_byte_off += 16u;
    src_va += uint32_t(stride);
  }
#else
  for (int32_t i = 0; i < count; ++i) {
    for (int32_t s = 0; s < num_streams; ++s) {
      const uint8_t* sp = Raw(base, (src_va + src_offs[s]) & ~0xFu);
      uint8_t* dp = Raw(base, (out_streams[dst_idxs[s]] + dst_byte_off) & ~0xFu);
      for (int c = 0; c < 4; ++c) StoreBeF32(dp + c * 4, std::fmaf(w[c], LoadBeF32(sp + c * 4), LoadBeF32(dp + c * 4)));
    }
    dst_byte_off += 16u;
    src_va += uint32_t(stride);
  }
#endif
}

inline void RunSelfTest() {
  alignas(64) uint8_t buf[1024]{};
  const float w[4] = {0.35f, -0.5f, 0.75f, 0.25f};
  for (u32 v = 0; v < 4u; ++v) {
    for (u32 c = 0; c < 4u; ++c) {
      StoreBeF32(buf + 0x100u + v * 16u + c * 4u, float(v + 1u) * 0.5f + float(c));
      StoreBeF32(buf + 0x180u + v * 16u + c * 4u, -float(v + 1u) * 0.25f + float(c) * 0.5f);
      StoreBeF32(buf + 0x200u + v * 32u + c * 4u, 0.1f * float(v * 4u + c + 1u));
      StoreBeF32(buf + 0x200u + v * 32u + 16u + c * 4u, -0.2f * float(v * 4u + c + 1u));
    }
  }
  uint8_t ref[1024];
  std::memcpy(ref, buf, sizeof(buf));
  AccumSingleStream(buf + 0x100u, buf + 0x200u, 32, 4, w);
  const uint32_t out_s[2] = {0x100u, 0x180u};
  const uint32_t src_o[2] = {0u, 16u};
  const uint32_t dst_i[2] = {0u, 1u};
  AccumMultiStream(buf, out_s, src_o, dst_i, 2, 0u, 0x200u, 32, 4, w);

  // Scalar reference: single stream adds w*src0 to s0; multi adds w*src0 to s0 again and w*src1 to s1.
  int failures = 0;
  for (u32 v = 0; v < 4u; ++v) {
    for (u32 c = 0; c < 4u; ++c) {
      const float s0 = LoadBeF32(ref + 0x200u + v * 32u + c * 4u);
      const float s1 = LoadBeF32(ref + 0x200u + v * 32u + 16u + c * 4u);
      const float e0 = std::fmaf(w[c], s0, std::fmaf(w[c], s0, LoadBeF32(ref + 0x100u + v * 16u + c * 4u)));
      const float e1 = std::fmaf(w[c], s1, LoadBeF32(ref + 0x180u + v * 16u + c * 4u));
      const float g0 = LoadBeF32(buf + 0x100u + v * 16u + c * 4u);
      const float g1 = LoadBeF32(buf + 0x180u + v * 16u + c * 4u);
      if (std::isnan(g0) || std::isnan(g1) || std::fabs(g0 - e0) > 1e-5f || std::fabs(g1 - e1) > 1e-5f) {
        ++failures;
        REXLOG_ERROR("[hot] DIFFERENCE sub_826D61A0 self-test v={} c={}", v, c);
        break;
      }
    }
  }
  if (failures == 0) {
    REXLOG_INFO("[hot] sub_826D61A0 morph accumulator self-test: passed");
  } else {
    sa2::enh::VerifyDifference();
  }
}

inline void Native(PPCContext& __restrict ctx, uint8_t* base) noexcept {
  static std::atomic<bool> tested_done{false};
  if (!tested_done.load(std::memory_order_acquire)) [[unlikely]] {
    static std::once_flag tested;
    std::call_once(tested, [] {
      RunSelfTest();
      tested_done.store(true, std::memory_order_release);
    });
  }
  const uint64_t old_sp = ctx.r1.u64;
  const uint32_t r3_in = ctx.r3.u32;
  const uint32_t r4_in = ctx.r4.u32;
  const uint32_t r5_in = ctx.r5.u32;
  if (uint32_t(old_sp) < 0x4000u || r5_in < 0x1000u) {
    ::__imp__sub_826D61A0(ctx, base);
    return;
  }
  const uint8_t* params = Raw(base, r5_in);
  const int32_t num_io_streams = int32_t(LoadBeU32(params + 11424u));
  if (num_io_streams < 0 || num_io_streams > 8) {
    ::__imp__sub_826D61A0(ctx, base);
    return;
  }

  // Parse active targets (non-zero weights and blocks_remaining != 0) into compact L1-resident host arrays.
  struct TargetState {
    float weight[4];
    int32_t num_streams;
    uint32_t src_offs[8];
    uint32_t dst_idxs[8];
    int32_t blocks_rem;
    int32_t stride;
    uint32_t block_ptr;
  };
  TargetState active[119];
  uint32_t num_active = 0;
  for (uint32_t m = 0; m < 119u; ++m) {
    const uint8_t* wp = params + (m << 4);
    const float w0 = LoadBeF32(wp + 0u);
    const float w1 = LoadBeF32(wp + 4u);
    const float w2 = LoadBeF32(wp + 8u);
    const float w3 = LoadBeF32(wp + 12u);
    if (w0 == 0.0f && w1 == 0.0f && w2 == 0.0f && w3 == 0.0f) continue;
    const uint8_t* tp = params + 1904u + m * 80u;
    const int32_t blocks_rem = int32_t(LoadBeU32(tp + 68u));
    if (blocks_rem == 0) continue;
    const int32_t n_str = int32_t(LoadBeU32(tp + 0u));
    if (n_str < 0 || n_str > 8) {
      ::__imp__sub_826D61A0(ctx, base);
      return;
    }
    TargetState& ts = active[num_active++];
    ts.weight[0] = w0;
    ts.weight[1] = w1;
    ts.weight[2] = w2;
    ts.weight[3] = w3;
    ts.num_streams = n_str;
    for (int32_t s = 0; s < n_str; ++s) {
      ts.src_offs[s] = LoadBeU32(tp + 4u + uint32_t(s << 2));
      ts.dst_idxs[s] = LoadBeU32(tp + 36u + uint32_t(s << 2));
      if (ts.dst_idxs[s] >= 8u) {
        ::__imp__sub_826D61A0(ctx, base);
        return;
      }
    }
    ts.blocks_rem = blocks_rem;
    ts.stride = int32_t(LoadBeU32(tp + 72u));
    ts.block_ptr = LoadBeU32(tp + 76u);
  }

  const uint32_t sp = uint32_t(old_sp) - 12208u;
  ctx.r1.u64 = uint64_t(sp);
  REX_STORE_U32(sp, uint32_t(old_sp));

  ctx.r3.u64 = sp + 160u;
  ctx.r4.u64 = r3_in;
  ctx.r5.u64 = r4_in;
  ctx.r6.u64 = 0u;
  ctx.r7.u64 = 262144u;
  ::__imp__sub_8283C4F8(ctx, base);

  ctx.r3.u64 = sp + 160u;
  ::__imp__sub_8283C600(ctx, base);

  auto get_stream = [&](uint32_t idx) -> uint32_t {
    ctx.r3.u64 = sp + 160u;
    ctx.r4.u64 = idx;
    ::__imp__sub_8283C4D8(ctx, base);
    return ctx.r3.u32;
  };

  for (;;) {
    ctx.r3.u64 = sp + 160u;
    ::__imp__sub_8283C280(ctx, base);
    if ((ctx.r3.u32 & 0xFFu) != 0u) break;

    ctx.r3.u64 = sp + 160u;
    ::__imp__sub_8283C288(ctx, base);
    const uint32_t chunk_count = ctx.r3.u32;

    ctx.r3.u64 = sp + 160u;
    ::__imp__sub_8283C2B8(ctx, base);
    const uint32_t chunk_start = ctx.r3.u32;
    const int32_t chunk_end = int32_t(chunk_start + chunk_count) - 1;

    uint32_t out_streams[8]{};
    const uint32_t copy_bytes = chunk_count << 4;
    for (int32_t s = 0; s < num_io_streams; ++s) {
      const uint32_t in_va = get_stream(uint32_t(s * 2));
      const uint32_t out_va = get_stream(uint32_t(s * 2 + 1));
      out_streams[s] = out_va;
      if (copy_bytes != 0u) {
        std::memcpy(Raw(base, out_va), Raw(base, in_va), copy_bytes);
        Card(base, out_va, copy_bytes);
      }
    }

    for (uint32_t a = 0; a < num_active; ++a) {
      TargetState& ts = active[a];
      while (ts.blocks_rem != 0) {
        const uint8_t* hdr = Raw(base, ts.block_ptr);
        const uint32_t blk_start = LoadBeU16(hdr + 0u);
        const uint32_t blk_count = LoadBeU16(hdr + 2u);
        const int32_t lo = (int32_t(blk_start - chunk_start) < 0) ? int32_t(chunk_start) : int32_t(blk_start);
        const int32_t blk_end = int32_t(blk_start + blk_count) - 1;
        const int32_t hi = (blk_end > chunk_end) ? chunk_end : blk_end;
        const int32_t n = hi - lo + 1;
        if (n <= 0) break;
        const uint32_t src_va = ts.block_ptr + 16u + uint32_t((lo - int32_t(blk_start)) * ts.stride);
        const uint32_t dst_byte_off = uint32_t(lo - int32_t(chunk_start)) << 4;
        if (ts.num_streams == 1) {
          const uint32_t sea = (src_va + ts.src_offs[0]) & ~0xFu;
          const uint32_t dea = (out_streams[ts.dst_idxs[0]] + dst_byte_off) & ~0xFu;
          AccumSingleStream(Raw(base, dea), Raw(base, sea), ts.stride, n, ts.weight);
        } else if (ts.num_streams > 1) {
          AccumMultiStream(base, out_streams, ts.src_offs, ts.dst_idxs, ts.num_streams, dst_byte_off, src_va, ts.stride,
                           n, ts.weight);
        }
        if (hi == blk_end) {
          ts.block_ptr += 16u + uint32_t(ts.stride * int32_t(blk_count));
          ts.blocks_rem -= 1;
        } else {
          break;
        }
      }
    }

    ctx.r3.u64 = sp + 160u;
    ::__imp__sub_8283C690(ctx, base);
  }

  ctx.r3.u64 = sp + 160u;
  ::__imp__sub_8283C580(ctx, base);
  ctx.fpscr.enableFlushMode();
  ctx.r1.u64 = old_sp;
}

}  // namespace n_826D61A0

}  // namespace sa2::hot

SA2_HOT_HOOK_COPY(8229B520, sa2_cpu_native_memcpy, true, "Native CRT memcpy (sub_8229B520): host memcpy",
                  sa2::hot::n_8229B520, 0, 0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82A8B590, sa2_cpu_native_82A8B590, true,
                  "Native ARM64 NEON audio biquad IIR filter (sub_82A8B590)", sa2::hot::n_82A8B590, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82A8C308, sa2_cpu_native_82A8C308, true,
                  "Native float array multiply-accumulate (sub_82A8C308)", sa2::hot::n_82A8C308, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(8283B828, sa2_cpu_native_8283B828, true, "Native lock-free job queue scan (sub_8283B828, 11.2% x3 workers + FGGameSim)",
                  sa2::hot::n_8283B828, 0, 0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(826D6D18, sa2_cpu_native_826D6D18, true,
                  "Native NEON vertex quad5 packer (sub_826D6D18)", sa2::hot::n_826D6D18, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(826D6B18, sa2_cpu_native_sub_826D6B18, true,
                  "Native NEON vertex quad4 packer (sub_826D6B18)", sa2::hot::n_826D6B18, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82C61450, sa2_cpu_native_sub_82C61450, true,
                  "Native XMemCpy (sub_82C61450): host memcpy", sa2::hot::n_82C61450, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82789120, sa2_cpu_native_82789120, true,
                  "Native SIMD skinning loop (sub_82789120, barrier-free copy)", sa2::hot::n_82789120, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82A87020, sa2_cpu_native_82A87020, true,
                  "Native ARM64 NEON FFT convolution complex MAC (sub_82A87020)", sa2::hot::n_82A87020, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(826D8178, sa2_cpu_native_sub_826D8178, true,
                  "Native ARM64 NEON matrix-palette skinning (sub_826D8178)", sa2::hot::n_826D8178, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82A8F798, sa2_cpu_native_sub_82A8F798, true,
                  "Native ARM64 NEON audio dynamics compressor (sub_82A8F798)", sa2::hot::n_82A8F798, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82AA02B0, sa2_cpu_native_sub_82AA02B0, true,
                  "Native ARM64 NEON audio RMS/peak meter (sub_82AA02B0)", sa2::hot::n_82AA02B0, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82A8BBC0, sa2_cpu_native_sub_82A8BBC0, true,
                  "Native float32 linear audio resampler (sub_82A8BBC0)", sa2::hot::n_82A8BBC0, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82A85B58, sa2_cpu_native_sub_82A85B58, true,
                  "Native ARM64 NEON audio gain ramp/scale (sub_82A85B58)", sa2::hot::n_82A85B58, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82440038, sa2_cpu_native_sub_82440038, true,
                  "Native ARM64 NEON SQT transform compose (sub_82440038)", sa2::hot::n_82440038, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(82B85E10, sa2_cpu_native_sub_82B85E10, true,
                  "Native ARM64 NEON 4-wide distance constraints (sub_82B85E10)", sa2::hot::n_82B85E10, 0,
                  0xCBF29CE484222325ull)
SA2_HOT_HOOK_COPY(826D61A0, sa2_cpu_native_sub_826D61A0, true,
                  "Native ARM64 NEON morph-target accumulator (sub_826D61A0)", sa2::hot::n_826D61A0, 0,
                  0xCBF29CE484222325ull)
