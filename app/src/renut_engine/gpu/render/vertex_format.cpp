/**
 * @file    gpu/render/vertex_format.cpp
 */
#include "renut_engine/gpu/render/vertex_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include <rex/cvar.h>

#include "renut_engine/gpu/backend/backend.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"
#include "shader_common.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

REXCVAR_DEFINE_BOOL(fnc_deko3d_vtx_10_10_10_2, true, "FNC/Renderer",
                    "Pass 2_10_10_10 vertex attributes natively on deko3d (DkVtxAttribSize_10_10_10_2)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(fnc_deko3d_vtx_8_8_8_8_scaled, true, "FNC/Renderer",
                    "Pass 8_8_8_8 integer vertex attributes natively on deko3d (DkVtxAttribType_Uscaled/Sscaled)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace renut::gpu {

namespace {

// D3DDECLUSAGE values.
enum DeclUsage : u8 {
  Position = 0,
  BlendWeight = 1,
  BlendIndices = 2,
  Normal = 3,
  PSize = 4,
  TexCoord = 5,
  Tangent = 6,
  Binormal = 7,
  TessFactor = 8,
  PositionT = 9,
  Color = 10,
  Fog = 11,
  Depth = 12,
  Sample = 13,
};

const char *const kUsageSemantics[] = {
    "POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD", "TANGENT",
    "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE",
};

// Shared with the shaders through shader_common.h.
bool InputLocation(u8 usage, u8 usage_index, u32 &location) {
  struct Row {
    u8 usage;
    u8 index;
    u32 location;
  };
  static const Row kRows[] = {
#define RENUT_ROW(usage, index, location) {DeclUsage::usage, index, location},
      RENUT_VERTEX_INPUT_LOCATIONS(RENUT_ROW)
#undef RENUT_ROW
  };
  for (const Row &row : kRows) {
    if (row.usage == usage && row.index == usage_index) {
      location = row.location;
      return true;
    }
  }
  return false;
}

// Xenos vertex formats (GPUVERTEXFORMAT).
enum VertexFormat : u32 {
  k8_8_8_8 = 6,
  k2_10_10_10 = 7,
  k10_11_11 = 16,
  k11_11_10 = 17,
  k16_16 = 25,
  k16_16_16_16 = 26,
  k16_16_Float = 31,
  k16_16_16_16_Float = 32,
  k32 = 33,
  k32_32 = 34,
  k32_32_32_32 = 35,
  k32_Float = 36,
  k32_32_Float = 37,
  k32_32_32_32_Float = 38,
  k32_32_32_Float = 57,
};

struct FormatInfo {
  u32 dwords;      // guest size
  u32 components;  // components the format supplies
  bool is_float;
};

bool GetFormatInfo(u32 format, FormatInfo &info) {
  switch (format) {
  case k8_8_8_8: info = {1, 4, false}; return true;
  case k2_10_10_10: info = {1, 4, false}; return true;
  case k10_11_11: info = {1, 3, false}; return true;
  case k11_11_10: info = {1, 3, false}; return true;
  case k16_16: info = {1, 2, false}; return true;
  case k16_16_16_16: info = {2, 4, false}; return true;
  case k16_16_Float: info = {1, 2, true}; return true;
  case k16_16_16_16_Float: info = {2, 4, true}; return true;
  case k32: info = {1, 1, false}; return true;
  case k32_32: info = {2, 2, false}; return true;
  case k32_32_32_32: info = {4, 4, false}; return true;
  case k32_Float: info = {1, 1, true}; return true;
  case k32_32_Float: info = {2, 2, true}; return true;
  case k32_32_32_32_Float: info = {4, 4, true}; return true;
  case k32_32_32_Float: info = {3, 3, true}; return true;
  default: return false;
  }
}

// The swizzle equals what the input assembler does anyway: the format's
// components in order, then 0 for missing y/z and 1 for a missing w.
bool IsNativeSwizzle(const DeclType &type, u32 components) {
  for (u32 i = 0; i < 4; ++i) {
    const u8 expected = i < components ? u8(i) : (i == 3 ? 5 : 4);
    if (type.swizzle[i] != expected)
      return false;
  }
  return true;
}

bool Is16BitFormat(u32 format) {
  return format == k16_16 || format == k16_16_16_16 || format == k16_16_Float ||
         format == k16_16_16_16_Float;
}

// A 16-bit element bound with each pair of components exchanged (BoundDeclType)
// reads natively once the halves of its words are swapped: makes 'type' the
// native swizzle and returns true if so.
bool UnswapHalves(DeclType &type, u32 components) {
  if (!Is16BitFormat(type.format))
    return false;
  for (u32 i = 0; i < 4; ++i) {
    const u8 expected = i < components ? u8(i ^ 1) : (i == 3 ? 5 : 4);
    if (type.swizzle[i] != expected)
      return false;
  }
  for (u32 i = 0; i < components; ++i)
    type.swizzle[i] = u8(i);
  return true;
}

// Host format when the swapped guest words can be read as-is.
plume::RenderFormat PassthroughFormat(const DeclType &type, const FormatInfo &info, u8 usage, u8 usage_index) {
  using F = plume::RenderFormat;
  const bool native = IsNativeSwizzle(type, info.components);
  switch (type.format) {
  case k32_Float: return native ? F::R32_FLOAT : F::UNKNOWN;
  case k32_32_Float: return native ? F::R32G32_FLOAT : F::UNKNOWN;
  case k32_32_32_Float: return native ? F::R32G32B32_FLOAT : F::UNKNOWN;
  case k32_32_32_32_Float: return native ? F::R32G32B32A32_FLOAT : F::UNKNOWN;
  case k16_16_Float: return native ? F::R16G16_FLOAT : F::UNKNOWN;
  case k16_16_16_16_Float: return native ? F::R16G16B16A16_FLOAT : F::UNKNOWN;
  case k16_16:
    if (!native || type.is_integer)
      return F::UNKNOWN;
    return type.is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
  case k16_16_16_16:
    if (!native || type.is_integer)
      return F::UNKNOWN;
    return type.is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
  case k8_8_8_8:
    if (type.is_integer) {
      static const bool passthrough_scaled = REXCVAR_GET(fnc_deko3d_vtx_8_8_8_8_scaled);
      if (native && IsDeko3D() && passthrough_scaled)
        return type.is_signed ? F::R8G8B8A8_SSCALED : F::R8G8B8A8_USCALED;
      return F::UNKNOWN;
    }
    if (native)
      return type.is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
    // D3DCOLOR: ZYXW over the swapped word is exactly BGRA.
    if (!type.is_signed && type.swizzle[0] == 2 && type.swizzle[1] == 1 && type.swizzle[2] == 0 &&
        type.swizzle[3] == 3)
      return F::B8G8R8A8_UNORM;
    return F::UNKNOWN;
  case k2_10_10_10: {
    static const bool passthrough_10_10_10_2 = REXCVAR_GET(fnc_deko3d_vtx_10_10_10_2);
    if (type.is_integer || !IsDeko3D() || !passthrough_10_10_10_2)
      return F::UNKNOWN;
    if (native)
      return type.is_signed ? F::R10G10B10A2_SNORM : F::R10G10B10A2_UNORM;
    if (type.swizzle[0] == 2 && type.swizzle[1] == 1 && type.swizzle[2] == 0 && type.swizzle[3] == 3)
      return type.is_signed ? F::B10G10R10A2_SNORM : F::B10G10R10A2_UNORM;
    // All FNC VS reading NORMAL0/TANGENT0 ignore .w; allow constant 0/1 swizzle on w (e.g. 0125).
    const bool ignore_w_normal_tangent = (usage == DeclUsage::Normal || usage == DeclUsage::Tangent) &&
                                         usage_index == 0 && type.swizzle[0] == 0 && type.swizzle[1] == 1 &&
                                         type.swizzle[2] == 2 && (type.swizzle[3] == 4 || type.swizzle[3] == 5);
    if (ignore_w_normal_tangent)
      return type.is_signed ? F::R10G10B10A2_SNORM : F::R10G10B10A2_UNORM;
    return F::UNKNOWN;
  }
  default:  // 10/11-bit formats have no plume equivalent
    return F::UNKNOWN;
  }
}

u32 SwapDword(u32 value, u32 endian) {
  switch (endian) {
  case 1: // 8in16
    return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
  case 2: // 8in32
    return (value << 24) | ((value << 8) & 0x00FF0000u) | ((value >> 8) & 0x0000FF00u) |
           (value >> 24);
  case 3: // 16in32
    return (value << 16) | (value >> 16);
  default:
    return value;
  }
}

// Guest words are stored big-endian in memory; the fetch constant's endian
// mode describes the swap from that storage to the GPU's word value.
template <u32 kEndian>
u32 LoadGuestWord(const u8 *p) {
  u32 raw;
  std::memcpy(&raw, p, 4);  // host (little-endian) view of the stored bytes
  return SwapDword(raw, kEndian);
}

float HalfToFloat(u16 h) {
#if defined(__aarch64__)
  const uint16x4_t v = vdup_n_u16(h);
  return vgetq_lane_f32(vcvt_f32_f16(vreinterpret_f16_u16(v)), 0);
#else
  const u32 sign = u32(h >> 15) << 31;
  const u32 exponent = (h >> 10) & 0x1F;
  const u32 mantissa = h & 0x3FF;
  u32 bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {  // subnormal
      float f = std::ldexp(float(mantissa), -24);
      return (h >> 15) ? -f : f;
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000u | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
#endif
}

template <u32 kShift, u32 kBits, bool kSigned, bool kInteger>
inline float DecodeComponentFast(u32 word) {
  const u32 raw = kBits == 32 ? word : (word >> kShift) & ((1u << kBits) - 1u);
  if constexpr (kSigned) {
    const int32_t value = kBits == 32 ? int32_t(raw) : int32_t(raw << (32 - kBits)) >> (32 - kBits);
    if constexpr (kInteger)
      return float(value);
    constexpr float kInvMax = 1.0f / (kBits == 32 ? 2147483647.0f : float((1u << (kBits - 1)) - 1u));
    return std::max(float(value) * kInvMax, -1.0f);
  } else {
    if constexpr (kInteger)
      return float(raw);
    constexpr float kInvMax = 1.0f / (kBits == 32 ? 4294967295.0f : float((1u << kBits) - 1u));
    return float(raw) * kInvMax;
  }
}

float DecodeComponent(u32 word, u32 shift, u32 bits, const DeclType &type) {
  const u32 raw = bits == 32 ? word : (word >> shift) & ((1u << bits) - 1u);
  if (type.is_signed) {
    const int32_t value = bits == 32 ? int32_t(raw) : int32_t(raw << (32 - bits)) >> (32 - bits);
    if (type.is_integer)
      return float(value);
    const float max_value = bits == 32 ? 2147483647.0f : float((1u << (bits - 1)) - 1u);
    return std::max(float(value) / max_value, -1.0f);
  }
  if (type.is_integer)
    return float(raw);
  return float(raw) / (bits == 32 ? 4294967295.0f : float((1u << bits) - 1u));
}

// Expands one element of 'count' vertices to four floats: 'decode' fills the
// format's components (before swizzle) of the first slots of c[6], which start
// as 0, 0, 0, 1 followed by the swizzle constants 0 and 1.
template <typename Decode>
void ExpandLoop(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count,
                const u8 swizzle[4], Decode decode) {
  u8 pick[4];
  for (u32 i = 0; i < 4; ++i)
    pick[i] = swizzle[i] < 6 ? swizzle[i] : 4;  // 4: 0, 5: 1; the rest read 0
  if (pick[0] == 0 && pick[1] == 1 && pick[2] == 2 && pick[3] == 3) {
    for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride) {
      float c[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
      decode(src, c);
      std::memcpy(dst, c, 16);
    }
    return;
  }
  const u8 p0 = pick[0], p1 = pick[1], p2 = pick[2], p3 = pick[3];
  for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride) {
    float c[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f};
    decode(src, c);
    const float out[4] = {c[p0], c[p1], c[p2], c[p3]};
    std::memcpy(dst, out, sizeof(out));
  }
}

#if defined(__aarch64__)
template <bool kSigned, u32 kFillMode, typename LoadFn>
void NeonExpandPackedNormalized(const u8 swizzle[4], const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride,
                                u32 count, const int32_t lshift_arr[4], const int32_t rshift_arr[4],
                                const float inv_max_arr[4], LoadFn load_words) {
  u8 pick[4];
  for (u32 i = 0; i < 4; ++i)
    pick[i] = swizzle[i] < 6 ? swizzle[i] : 4;
  const bool identity = pick[0] == 0 && pick[1] == 1 && pick[2] == 2 && pick[3] == 3;
  uint8_t tbl_bytes[16];
  for (u32 i = 0; i < 4; ++i) {
    const u8 s = pick[i] < 4 ? pick[i] : 0;
    for (u32 b = 0; b < 4; ++b)
      tbl_bytes[i * 4 + b] = u8(s * 4 + b);
  }
  const uint8x16_t tbl = vld1q_u8(tbl_bytes);
  const uint32x4_t const_mask = {pick[0] >= 4 ? ~0u : 0u, pick[1] >= 4 ? ~0u : 0u,
                                 pick[2] >= 4 ? ~0u : 0u, pick[3] >= 4 ? ~0u : 0u};
  const float32x4_t const_vals = {pick[0] == 5 ? 1.0f : 0.0f, pick[1] == 5 ? 1.0f : 0.0f,
                                  pick[2] == 5 ? 1.0f : 0.0f, pick[3] == 5 ? 1.0f : 0.0f};
  const int32x4_t lshift = vld1q_s32(lshift_arr);
  const int32x4_t rshift = vld1q_s32(rshift_arr);
  const float32x4_t inv_max = vld1q_f32(inv_max_arr);
  const float32x4_t minus_one = vdupq_n_f32(-1.0f);
  const float32x4_t fill_z0w1 = {0.0f, 0.0f, 0.0f, 1.0f};
  const uint32x4_t fill_mask = {0u, 0u, kFillMode == 2 ? ~0u : 0u, kFillMode >= 1 ? ~0u : 0u};

  auto emit = [&](const uint32x4_t words, u8 *out_ptr) {
    const uint32x4_t top = vshlq_u32(words, lshift);
    float32x4_t f;
    if constexpr (kSigned) {
      const int32x4_t sval = vshlq_s32(vreinterpretq_s32_u32(top), rshift);
      f = vmaxq_f32(vmulq_f32(vcvtq_f32_s32(sval), inv_max), minus_one);
    } else {
      const uint32x4_t uval = vshlq_u32(top, rshift);
      f = vmulq_f32(vcvtq_f32_u32(uval), inv_max);
    }
    if constexpr (kFillMode > 0)
      f = vbslq_f32(fill_mask, fill_z0w1, f);
    if (!identity) {
      const float32x4_t perm = vreinterpretq_f32_u8(vqtbl1q_u8(vreinterpretq_u8_f32(f), tbl));
      f = vbslq_f32(const_mask, const_vals, perm);
    }
    float tmp[4];
    vst1q_f32(tmp, f);
    std::memcpy(out_ptr, tmp, sizeof(tmp));
  };

  for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
    emit(load_words(src), dst);
}
#endif

template <u32 kEndian, bool kSigned, bool kInteger>
void ExpandElementNormalized(u32 format, const u8 swizzle[4], const u8 *src, u32 src_stride,
                             u8 *dst, u32 dst_stride, u32 count) {
#if defined(__aarch64__)
  if constexpr (!kInteger) {
    auto word1 = [](const u8 *p) { return vdupq_n_u32(LoadGuestWord<kEndian>(p)); };
    auto word2 = [](const u8 *p) {
      const u32 w0 = LoadGuestWord<kEndian>(p);
      const u32 w1 = LoadGuestWord<kEndian>(p + 4);
      const uint32_t arr[4] = {w0, w0, w1, w1};
      return vld1q_u32(arr);
    };
    switch (format) {
    case k8_8_8_8: {
      static constexpr int32_t l[4] = {24, 16, 8, 0};
      static constexpr int32_t r[4] = {-24, -24, -24, -24};
      static constexpr float inv[4] = {
          kSigned ? 1.0f / 127.0f : 1.0f / 255.0f, kSigned ? 1.0f / 127.0f : 1.0f / 255.0f,
          kSigned ? 1.0f / 127.0f : 1.0f / 255.0f, kSigned ? 1.0f / 127.0f : 1.0f / 255.0f};
      NeonExpandPackedNormalized<kSigned, 0>(swizzle, src, src_stride, dst, dst_stride, count, l, r, inv, word1);
      return;
    }
    case k2_10_10_10: {
      static constexpr int32_t l[4] = {22, 12, 2, 0};
      static constexpr int32_t r[4] = {-22, -22, -22, -30};
      static constexpr float inv[4] = {
          kSigned ? 1.0f / 511.0f : 1.0f / 1023.0f, kSigned ? 1.0f / 511.0f : 1.0f / 1023.0f,
          kSigned ? 1.0f / 511.0f : 1.0f / 1023.0f, kSigned ? 1.0f : 1.0f / 3.0f};
      NeonExpandPackedNormalized<kSigned, 0>(swizzle, src, src_stride, dst, dst_stride, count, l, r, inv, word1);
      return;
    }
    case k10_11_11: {
      static constexpr int32_t l[4] = {21, 10, 0, 0};
      static constexpr int32_t r[4] = {-21, -21, -22, 0};
      static constexpr float inv[4] = {
          kSigned ? 1.0f / 1023.0f : 1.0f / 2047.0f, kSigned ? 1.0f / 1023.0f : 1.0f / 2047.0f,
          kSigned ? 1.0f / 511.0f : 1.0f / 1023.0f, 0.0f};
      NeonExpandPackedNormalized<kSigned, 1>(swizzle, src, src_stride, dst, dst_stride, count, l, r, inv, word1);
      return;
    }
    case k11_11_10: {
      static constexpr int32_t l[4] = {22, 11, 0, 0};
      static constexpr int32_t r[4] = {-22, -21, -21, 0};
      static constexpr float inv[4] = {
          kSigned ? 1.0f / 511.0f : 1.0f / 1023.0f, kSigned ? 1.0f / 1023.0f : 1.0f / 2047.0f,
          kSigned ? 1.0f / 1023.0f : 1.0f / 2047.0f, 0.0f};
      NeonExpandPackedNormalized<kSigned, 1>(swizzle, src, src_stride, dst, dst_stride, count, l, r, inv, word1);
      return;
    }
    case k16_16: {
      static constexpr int32_t l[4] = {16, 0, 0, 0};
      static constexpr int32_t r[4] = {-16, -16, 0, 0};
      static constexpr float inv[4] = {
          kSigned ? 1.0f / 32767.0f : 1.0f / 65535.0f, kSigned ? 1.0f / 32767.0f : 1.0f / 65535.0f, 0.0f, 0.0f};
      NeonExpandPackedNormalized<kSigned, 2>(swizzle, src, src_stride, dst, dst_stride, count, l, r, inv, word1);
      return;
    }
    case k16_16_16_16: {
      static constexpr int32_t l[4] = {16, 0, 16, 0};
      static constexpr int32_t r[4] = {-16, -16, -16, -16};
      static constexpr float inv[4] = {
          kSigned ? 1.0f / 32767.0f : 1.0f / 65535.0f, kSigned ? 1.0f / 32767.0f : 1.0f / 65535.0f,
          kSigned ? 1.0f / 32767.0f : 1.0f / 65535.0f, kSigned ? 1.0f / 32767.0f : 1.0f / 65535.0f};
      NeonExpandPackedNormalized<kSigned, 0>(swizzle, src, src_stride, dst, dst_stride, count, l, r, inv, word2);
      return;
    }
    default:
      break;
    }
  }
#endif
  auto word = [](const u8 *p, u32 i) { return LoadGuestWord<kEndian>(p + i * 4); };
  auto expand = [&](auto decode) {
    ExpandLoop(src, src_stride, dst, dst_stride, count, swizzle, decode);
  };
  switch (format) {
  case k8_8_8_8:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = DecodeComponentFast<0, 8, kSigned, kInteger>(w);
      c[1] = DecodeComponentFast<8, 8, kSigned, kInteger>(w);
      c[2] = DecodeComponentFast<16, 8, kSigned, kInteger>(w);
      c[3] = DecodeComponentFast<24, 8, kSigned, kInteger>(w);
    });
    break;
  case k2_10_10_10:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = DecodeComponentFast<0, 10, kSigned, kInteger>(w);
      c[1] = DecodeComponentFast<10, 10, kSigned, kInteger>(w);
      c[2] = DecodeComponentFast<20, 10, kSigned, kInteger>(w);
      c[3] = DecodeComponentFast<30, 2, kSigned, kInteger>(w);
    });
    break;
  case k10_11_11:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = DecodeComponentFast<0, 11, kSigned, kInteger>(w);
      c[1] = DecodeComponentFast<11, 11, kSigned, kInteger>(w);
      c[2] = DecodeComponentFast<22, 10, kSigned, kInteger>(w);
    });
    break;
  case k11_11_10:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = DecodeComponentFast<0, 10, kSigned, kInteger>(w);
      c[1] = DecodeComponentFast<10, 11, kSigned, kInteger>(w);
      c[2] = DecodeComponentFast<21, 11, kSigned, kInteger>(w);
    });
    break;
  case k16_16:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
      c[0] = DecodeComponentFast<0, 16, kSigned, kInteger>(w);
      c[1] = DecodeComponentFast<16, 16, kSigned, kInteger>(w);
    });
    break;
  case k16_16_16_16:
    expand([&](const u8 *p, float c[6]) {
      const u32 w0 = word(p, 0), w1 = word(p, 1);
      c[0] = DecodeComponentFast<0, 16, kSigned, kInteger>(w0);
      c[1] = DecodeComponentFast<16, 16, kSigned, kInteger>(w0);
      c[2] = DecodeComponentFast<0, 16, kSigned, kInteger>(w1);
      c[3] = DecodeComponentFast<16, 16, kSigned, kInteger>(w1);
    });
    break;
  default:
    break;
  }
}

// Decodes an element's components with the format and endian mode fixed for
// the whole loop. Packed components count from the low bits of the swapped
// word, 16-bit ones too: with the usual 8in32 fetch, x is the second halfword
// in memory. The bound swizzle (BoundDeclType) or the shader's own (.yx,
// .yxwz) swaps them back.
template <u32 kEndian>
void ExpandElement(const HostVertexElement &e, const u8 *src, u32 src_stride, u8 *dst,
                   u32 dst_stride, u32 count) {
  const DeclType &type = e.type;
  auto word = [](const u8 *p, u32 i) { return LoadGuestWord<kEndian>(p + i * 4); };
  auto expand = [&](auto decode) {
    ExpandLoop(src, src_stride, dst, dst_stride, count, type.swizzle, decode);
  };
  switch (type.format) {
  case k8_8_8_8:
  case k2_10_10_10:
  case k10_11_11:
  case k11_11_10:
  case k16_16:
  case k16_16_16_16:
    if (type.is_signed) {
      if (type.is_integer)
        ExpandElementNormalized<kEndian, true, true>(type.format, type.swizzle, src, src_stride, dst, dst_stride, count);
      else
        ExpandElementNormalized<kEndian, true, false>(type.format, type.swizzle, src, src_stride, dst, dst_stride, count);
    } else {
      if (type.is_integer)
        ExpandElementNormalized<kEndian, false, true>(type.format, type.swizzle, src, src_stride, dst, dst_stride, count);
      else
        ExpandElementNormalized<kEndian, false, false>(type.format, type.swizzle, src, src_stride, dst, dst_stride, count);
    }
    break;
  case k16_16_Float:
    expand([&](const u8 *p, float c[6]) {
      const u32 w = word(p, 0);
#if defined(__aarch64__)
      const uint32x2_t v32 = vdup_n_u32(w);
      const float32x4_t f = vcvt_f32_f16(vreinterpret_f16_u32(v32));
      vst1_f32(c, vget_low_f32(f));
#else
      c[0] = HalfToFloat(u16(w));
      c[1] = HalfToFloat(u16(w >> 16));
#endif
    });
    break;
  case k16_16_16_16_Float:
    expand([&](const u8 *p, float c[6]) {
      const u32 w0 = word(p, 0), w1 = word(p, 1);
#if defined(__aarch64__)
      const uint32_t pair[2] = {w0, w1};
      const uint16x4_t h = vreinterpret_u16_u32(vld1_u32(pair));
      vst1q_f32(c, vcvt_f32_f16(vreinterpret_f16_u16(h)));
#else
      c[0] = HalfToFloat(u16(w0));
      c[1] = HalfToFloat(u16(w0 >> 16));
      c[2] = HalfToFloat(u16(w1));
      c[3] = HalfToFloat(u16(w1 >> 16));
#endif
    });
    break;
  case k32:
  case k32_32:
  case k32_32_32_32: {
    FormatInfo info;
    GetFormatInfo(type.format, info);
    const u32 components = info.components;
    expand([&](const u8 *p, float c[6]) {
      for (u32 i = 0; i < components; ++i)
        c[i] = DecodeComponent(word(p, i), 0, 32, type);
    });
    break;
  }
  case k32_Float:
  case k32_32_Float:
  case k32_32_32_Float:
  case k32_32_32_32_Float: {
    FormatInfo info;
    GetFormatInfo(type.format, info);
    const u32 components = info.components;
    expand([&](const u8 *p, float c[6]) {
      for (u32 i = 0; i < components; ++i) {
        const u32 bits = word(p, i);
        std::memcpy(&c[i], &bits, 4);
      }
    });
    break;
  }
  default:
    expand([](const u8 *, float *) {});
    break;
  }
}

} // namespace

u32 BoundDeclType(u32 type) {
  if (((type >> 6) & 3) != 1)  // not 8in16 data
    return type;
  for (u32 i = 0; i < 4; ++i) {
    const u32 shift = 10 + 3 * i;
    const u32 swizzle = (type >> shift) & 7;
    if (swizzle <= 3)  // a component, not a constant 0 or 1
      type ^= 1u << shift;
  }
  return type;
}

DeclType DeclType::Decode(u32 type) {
  type = BoundDeclType(type);
  DeclType out;
  out.format = type & 0x3F;
  out.is_signed = (type >> 8) & 1;
  out.is_integer = (type >> 9) & 1;
  for (u32 i = 0; i < 4; ++i)
    out.swizzle[i] = u8((type >> (10 + 3 * i)) & 7);
  return out;
}

u32 FetchDeclType(u32 format, bool is_signed, bool is_integer) {
  FormatInfo info;
  if (!GetFormatInfo(format, info))
    return kDeclTypeUnused;
  u32 type = (format & 0x3F) | (is_signed ? 1u << 8 : 0u) | (is_integer ? 1u << 9 : 0u);
  for (u32 i = 0; i < 4; ++i) {
    const u32 swizzle = i < info.components ? i : (i == 3 ? 5u : 4u);  // 4: 0, 5: 1
    type |= swizzle << (10 + 3 * i);
  }
  return type;
}

const char *UsageSemantic(u8 usage) {
  return usage < std::size(kUsageSemantics) ? kUsageSemantics[usage] : "TEXCOORD";
}

HostVertexLayout BuildHostVertexLayout(const GuestVertexElement *elements, u32 count) {
  HostVertexLayout layout;
  for (u32 i = 0; i < count; ++i) {
    const GuestVertexElement &element = elements[i];
    const u32 stream = u16(element.Stream);
    if (u32(element.Type) == kDeclTypeUnused || stream >= kMaxVertexStreams)
      continue;

    HostVertexElement host;
    host.usage = element.Usage;
    host.usage_index = element.UsageIndex;
    if (!InputLocation(host.usage, host.usage_index, host.location))
      continue;  // not an input any N&B shader declares
    // A repeated semantic would make the input layout invalid; the first wins.
    const bool repeated = std::any_of(
        std::begin(layout.streams), std::end(layout.streams), [&](const HostVertexStream &s) {
          return std::any_of(s.elements.begin(), s.elements.end(),
                             [&](const HostVertexElement &e) { return e.location == host.location; });
        });
    if (repeated)
      continue;
    host.type = DeclType::Decode(u32(element.Type));
    FormatInfo info;
    if (!GetFormatInfo(host.type.format, info)) {
      RGPU_WARN("Vertex element {}{} uses unhandled vertex format {} (type {:08X})",
                UsageSemantic(host.usage), host.usage_index, host.type.format, u32(element.Type));
      continue;
    }
    host.guest_offset = u16(element.Offset);
    DeclType copy_type = host.type;
    host.swap_halves = UnswapHalves(copy_type, info.components);
    host.host_format = PassthroughFormat(copy_type, info, host.usage, host.usage_index);
    if (host.host_format == plume::RenderFormat::UNKNOWN)
      host.swap_halves = false;
    if (host.host_format != plume::RenderFormat::UNKNOWN) {
      host.conversion = ElementConversion::Copy32;
      host.host_size = info.dwords * 4;
      host.components = info.components;
    } else {
      host.conversion = ElementConversion::ExpandFloat;
      host.host_format = plume::RenderFormat::R32G32B32A32_FLOAT;
      host.host_size = 16;
      host.components = 4;
      // Log each unique non-passthrough vertex format once per run for Switch conversion diagnosis.
      const uint32_t vtx_key = u32(host.type.format) | (u32(host.type.is_signed) << 6) |
                               (u32(host.type.is_integer) << 7) | (u32(host.type.swizzle[0]) << 8) |
                               (u32(host.type.swizzle[1]) << 11) | (u32(host.type.swizzle[2]) << 14) |
                               (u32(host.type.swizzle[3]) << 17) | (u32(host.usage) << 20) |
                               (u32(host.usage_index) << 24);
      static std::mutex s_expand_log_mutex;
      static uint32_t s_seen_keys[64];
      static uint32_t s_seen_count = 0;
      bool log_now = false;
      {
        std::lock_guard<std::mutex> lock(s_expand_log_mutex);
        bool found = false;
        for (uint32_t k = 0; k < s_seen_count; ++k) {
          if (s_seen_keys[k] == vtx_key) {
            found = true;
            break;
          }
        }
        if (!found && s_seen_count < std::size(s_seen_keys)) {
          s_seen_keys[s_seen_count++] = vtx_key;
          log_now = true;
        }
      }
      if (log_now) {
        RGPU_INFO("[vtx expand] {}{} fmt={} signed={} int={} swz={}{}{}{}",
                  UsageSemantic(host.usage), host.usage_index, host.type.format, host.type.is_signed ? 1 : 0,
                  host.type.is_integer ? 1 : 0, host.type.swizzle[0], host.type.swizzle[1],
                  host.type.swizzle[2], host.type.swizzle[3]);
      }
    }
    layout.streams[stream].used = true;
    layout.used_mask |= 1u << stream;  // sa2-nx
    layout.streams[stream].elements.push_back(host);
  }

  // Pack each stream's host elements in declaration order.
  for (u32 stream = 0; stream < kMaxVertexStreams; ++stream) {
    HostVertexStream &s = layout.streams[stream];
    u32 offset = 0;
    for (HostVertexElement &element : s.elements) {
      element.host_offset = offset;
      offset += element.host_size;
      layout.input_elements.emplace_back(UsageSemantic(element.usage), element.usage_index,
                                         element.location, element.host_format, stream,
                                         element.host_offset);
    }
    s.host_stride = offset;
  }

  // D3D12 rejects a pipeline whose vertex shader reads an input the layout
  // lacks, and which inputs a shader reads is not known here, so every other
  // location reads zeros from a stride-0 stream.
  struct Row {
    u8 usage;
    u8 index;
    u32 location;
  };
  static const Row kRows[] = {
#define RENUT_ROW(usage, index, location) {DeclUsage::usage, index, location},
      RENUT_VERTEX_INPUT_LOCATIONS(RENUT_ROW)
#undef RENUT_ROW
  };
  for (const Row &row : kRows) {
    const bool present =
        std::any_of(layout.input_elements.begin(), layout.input_elements.end(),
                    [&](const plume::RenderInputElement &e) { return e.location == row.location; });
    if (!present) {
      layout.input_elements.emplace_back(UsageSemantic(row.usage), row.index, row.location,
                                         plume::RenderFormat::R32G32B32A32_FLOAT, kZeroStreamSlot,
                                         0);
    }
  }
  return layout;
}

namespace {

template <u32 kEndian>
void SwapWordsAs(const u8 *src, u8 *dst, size_t words) {
  for (size_t i = 0; i < words; ++i) {
    u32 value;
    std::memcpy(&value, src + i * 4, 4);
    value = SwapDword(value, kEndian);
    std::memcpy(dst + i * 4, &value, 4);
  }
}

#if defined(__aarch64__)
// banjo-nx (banjo_renut_neon_swap): 16 bytes per NEON byte reverse, the same bytes SwapDword gives each word:
// 8in16 = rev16 of bytes, 8in32 = rev32 of bytes, 16in32 = rev32 of halfwords.
template <u32 kEndian>
inline uint8x16_t SwapVector(uint8x16_t v) {
  if constexpr (kEndian == 1)
    return vrev16q_u8(v);
  else if constexpr (kEndian == 2)
    return vrev32q_u8(v);
  else
    return vreinterpretq_u8_u16(vrev32q_u16(vreinterpretq_u16_u8(v)));
}

// The words a vector loop did not take are left to the caller.
template <u32 kEndian>
size_t SwapWordsNeon(const u8 *src, u8 *dst, size_t words) {
  size_t i = 0;
  for (; i + 8 <= words; i += 8) {
    const uint8x16_t a = vld1q_u8(src + i * 4), b = vld1q_u8(src + i * 4 + 16);
    vst1q_u8(dst + i * 4, SwapVector<kEndian>(a));
    vst1q_u8(dst + i * 4 + 16, SwapVector<kEndian>(b));
  }
  for (; i + 4 <= words; i += 4)
    vst1q_u8(dst + i * 4, SwapVector<kEndian>(vld1q_u8(src + i * 4)));
  return i;
}
#endif

// LoadGuestWord over 'words' consecutive words, with the endian mode hoisted
// out of the loop.
void SwapWords(const u8 *src, u8 *dst, size_t words, u32 endian) {
#if defined(__aarch64__)
  if (endian && banjo::renut_gpu::NeonSwapOn()) {
    const size_t done = endian == 1   ? SwapWordsNeon<1>(src, dst, words)
                        : endian == 2 ? SwapWordsNeon<2>(src, dst, words)
                                      : SwapWordsNeon<3>(src, dst, words);
    src += done * 4;
    dst += done * 4;
    words -= done;
  }
#endif
  switch (endian) {
  case 1:
    SwapWordsAs<1>(src, dst, words);
    break;
  case 2:
    SwapWordsAs<2>(src, dst, words);
    break;
  case 3:
    SwapWordsAs<3>(src, dst, words);
    break;
  default:
    std::memcpy(dst, src, words * 4);
    break;
  }
}

template <u32 kEndian, u32 kWords>
void CopyWordsLoop(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count) {
#if defined(__aarch64__)
  // banjo-nx (banjo_renut_neon_swap): a four-word element (positions, normals as float4) is one vector.
  if constexpr (kWords == 4 && kEndian != 0) {
    if (banjo::renut_gpu::NeonSwapOn()) {
      for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
        vst1q_u8(dst, SwapVector<kEndian>(vld1q_u8(src)));
      return;
    }
  }
#endif
  for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
    SwapWordsAs<kEndian>(src, dst, kWords);
}

template <u32 kEndian>
void CopyElementWordsAs(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count,
                        u32 words) {
  switch (words) {
  case 1:
    CopyWordsLoop<kEndian, 1>(src, src_stride, dst, dst_stride, count);
    break;
  case 2:
    CopyWordsLoop<kEndian, 2>(src, src_stride, dst, dst_stride, count);
    break;
  case 3:
    CopyWordsLoop<kEndian, 3>(src, src_stride, dst, dst_stride, count);
    break;
  case 4:
    CopyWordsLoop<kEndian, 4>(src, src_stride, dst, dst_stride, count);
    break;
  default:
    for (u32 v = 0; v < count; ++v, src += src_stride, dst += dst_stride)
      SwapWordsAs<kEndian>(src, dst, words);
    break;
  }
}

// SwapWords of one element of 'count' vertices.
void CopyElementWords(const u8 *src, u32 src_stride, u8 *dst, u32 dst_stride, u32 count,
                      u32 words, u32 endian) {
  switch (endian & 3) {
  case 1:
    CopyElementWordsAs<1>(src, src_stride, dst, dst_stride, count, words);
    break;
  case 2:
    CopyElementWordsAs<2>(src, src_stride, dst, dst_stride, count, words);
    break;
  case 3:
    CopyElementWordsAs<3>(src, src_stride, dst, dst_stride, count, words);
    break;
  default:
    CopyElementWordsAs<0>(src, src_stride, dst, dst_stride, count, words);
    break;
  }
}

} // namespace

void ConvertVertices(const HostVertexStream &stream, const u8 *guest, u32 guest_stride,
                     u32 vertex_count, u32 endian, u8 *host) {
  bool copy_only = true, same_layout = stream.host_stride == guest_stride && guest_stride % 4 == 0;
  for (const HostVertexElement &element : stream.elements) {
    copy_only &= element.conversion == ElementConversion::Copy32 && !element.swap_halves;
    same_layout &= element.host_offset == element.guest_offset;
  }
  // Swapping the halves after any endian swap is the endian mode ^ 3
  // (8in32 <-> 8in16, none <-> 16in32).
  constexpr u32 kSwapHalves = 3;
  if (copy_only && same_layout) {
    // The host vertices are the guest vertices with their words swapped.
    // (Elements whose halves swap too take the chunked path below: a second
    // pass would read back upload memory, which is uncached.)
    SwapWords(guest, host, size_t(vertex_count) * guest_stride / 4, endian);
    return;
  }
  // One element of every vertex at a time, each in a loop with its format and
  // endian mode fixed: deciding them per vertex cost more than the conversion.
  // A chunk of vertices is built in cache and then copied out in one go: the
  // destination is upload memory, write-combined, where the scattered writes
  // of one element would be slow.
  constexpr u32 kChunkBytes = 16 * 1024;
  alignas(64) u8 chunk[kChunkBytes];
  const u32 host_stride = stream.host_stride;
  if (!host_stride)
    return;
  const u32 per_chunk = std::max(1u, kChunkBytes / host_stride);
  for (u32 first = 0; first < vertex_count; first += per_chunk) {
    const u32 count = std::min(per_chunk, vertex_count - first);
    const u8 *chunk_src = guest + size_t(first) * guest_stride;
    for (const HostVertexElement &element : stream.elements) {
      const u8 *src = chunk_src + element.guest_offset;
      u8 *dst = chunk + element.host_offset;
      if (element.conversion == ElementConversion::ExpandFloat) {
        switch (endian & 3) {
        case 1:
          ExpandElement<1>(element, src, guest_stride, dst, host_stride, count);
          break;
        case 2:
          ExpandElement<2>(element, src, guest_stride, dst, host_stride, count);
          break;
        case 3:
          ExpandElement<3>(element, src, guest_stride, dst, host_stride, count);
          break;
        default:
          ExpandElement<0>(element, src, guest_stride, dst, host_stride, count);
          break;
        }
      } else {
        // host little-endian words
        CopyElementWords(src, guest_stride, dst, host_stride, count, element.host_size / 4,
                         element.swap_halves ? endian ^ kSwapHalves : endian);
      }
    }
    std::memcpy(host + size_t(first) * host_stride, chunk, size_t(count) * host_stride);
  }
}

} // namespace renut::gpu
