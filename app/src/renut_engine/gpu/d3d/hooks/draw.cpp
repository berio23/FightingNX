/**
 * @file    gpu/d3d/hooks/draw.cpp
 * @brief   Draw, clear and resolve entry points, and the shader setters.
 *
 * The draws replace N&B's D3D entirely: the originals would flush the device's
 * dirty state into GPU packets, including binding shaders by reading their
 * microcode, which renderer-owned shader objects do not carry. Clear, Resolve
 * and tiling queue their host work and do the device work of N&B's D3D
 * natively (d3d_native.h): dirty masks, the tiling state, fences on the
 * destination.
 *
 * The host work runs on the render thread (queued.h); a device recording a
 * command buffer records its draws, clears and resolves (command_buffers.h)
 * instead. Neither takes State().mutex here.
 */
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

// A float argument also takes the GPR (or stack) slot of its position, which
// the marshaling does not skip: the u32 after each double in the hooks'
// parameters stands in for it.

namespace {

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;
// D3DVIEWPORT9 {0, 0, 0xFFFF, 0xFFFF, 0.0, 1.0} in N&B's data (unk_820018DC):
// the whole surface.
constexpr u32 kFullViewport = 0x820018DC;
// D3DRS_HIZWRITEENABLE's index in the render state table.
constexpr u32 kRenderStateHiZWriteEnable = 97;

// Guest memory Begin*Vertices hands out; the game writes one draw's vertices
// into it before its next D3D call. One per device, as devices record on
// different threads.
constexpr u32 kVerticesScratchSize = 4u << 20;
std::mutex g_vertices_scratch_mutex;  // devices call from their own threads
std::unordered_map<u32, u32> g_vertices_scratch;

u32 VerticesScratch(u32 device) {
  std::lock_guard lock(g_vertices_scratch_mutex);
  u32 &scratch = g_vertices_scratch[device];
  if (!scratch)
    scratch = records::AllocPhysicalBlock(kVerticesScratchSize);
  return scratch;
}

bool Active() {
  auto &s = State();
  return s.host_ready && !s.shutting_down.load(std::memory_order_acquire);
}

void StoreU32(u32 va, u32 value) {
  if (auto *p = GuestPtr<be_u32>(va))
    *p = value;
}

u32 LoadU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

bool ReadRect(u32 va, ClearRect &rect) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  rect = {i32(u32(p[0])), i32(u32(p[1])), i32(u32(p[2])), i32(u32(p[3]))};
  return true;
}

bool ReadVector4(u32 va, float out[4]) {
  const auto *p = GuestPtr<const be_u32>(va);
  if (!p)
    return false;
  for (int i = 0; i < 4; ++i) {
    const u32 bits = p[i];
    std::memcpy(&out[i], &bits, 4);
  }
  return true;
}

// void D3DDevice_DrawVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT StartVertex,
//                             UINT VertexCount) [0x8222C7A0]
void D3DDevice_DrawVertices_hook(u32 device, u32 primitive, u32 start_vertex, u32 vertex_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.start_vertex = start_vertex;
  call.count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedVertices(D3DDevice*, D3DPRIMITIVETYPE, INT BaseVertexIndex,
//                                    UINT StartIndex, UINT IndexCount) [0x826567C8]
void D3DDevice_DrawIndexedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                        u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawIndexedTessellatedVertices(D3DDevice*, D3DTESSPRIMITIVETYPE,
//     INT BaseVertexIndex, UINT StartIndex, UINT IndexCount) [0x82230838]
// Replaced like the other draws: the original's state flush binds the shader
// from its microcode, and bugchecks (0xF5) on a tessellation shader without
// the tessellation state the original SetVertexShader keeps. The draw is
// tessellated on the host (DrawCall::tessellation).
void D3DDevice_DrawIndexedTessellatedVertices_hook(u32 device, u32 primitive, u32 base_vertex,
                                                   u32 start_index, u32 index_count) {
  if (!Active())
    return;
  DrawCall call;
  call.primitive = primitive;
  call.tessellation = true;
  call.indexed = true;
  call.base_vertex = i32(base_vertex);
  call.start_index = start_index;
  call.count = index_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, 0, 0);
  else
    QueueDraw(device, call, 0, 0);
}

// void D3DDevice_DrawVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               const void *pVertexStreamZeroData, UINT Stride) [0x8222E520]
void D3DDevice_DrawVerticesUP_hook(u32 device, u32 primitive, u32 vertex_count, u32 data,
                                   u32 stride) {
  if (!Active() || !data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(data);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_count * stride, 0);
  else
    QueueDraw(device, call, vertex_count * stride, 0);
}

// void D3DDevice_DrawIndexedVerticesUP(D3DDevice*, D3DPRIMITIVETYPE, UINT MinVertexIndex,
//     UINT NumVertices, UINT IndexCount, const void *pIndexData, D3DFORMAT IndexDataFormat,
//     const void *pVertexStreamZeroData, UINT VertexStreamZeroStride) [0x82656728]
void D3DDevice_DrawIndexedVerticesUP_hook(u32 device, u32 primitive, u32 min_vertex,
                                          u32 num_vertices, u32 index_count, u32 index_data,
                                          u32 index_format, u32 vertex_data, u32 stride) {
  if (!Active() || !index_data || !vertex_data)
    return;
  DrawCall call;
  call.primitive = primitive;
  call.indexed = true;
  call.count = index_count;
  call.up_indices = GuestPtr<const u8>(index_data);
  call.up_indices_32 = (index_format & 4) != 0;  // D3DFMT_INDEX32
  call.up_vertices = GuestPtr<const u8>(vertex_data);
  call.up_stride = stride;
  call.up_vertex_count = min_vertex + num_vertices;
  const u32 vertex_bytes = call.up_vertex_count * stride;
  const u32 index_bytes = index_count * (call.up_indices_32 ? 4 : 2);
  if (IsRecordingCommandBuffer(device))
    RecordDraw(device, call, vertex_bytes, index_bytes);
  else
    QueueDraw(device, call, vertex_bytes, index_bytes);
}

// void *D3DDevice_BeginVertices(D3DDevice*, D3DPRIMITIVETYPE, UINT VertexCount,
//                               UINT VertexStreamZeroStride) [0x8222CC50]
// Callers write the vertices, then end inline by restoring m_pRing from the
// device, so the draw's state is captured now and its vertices copied at the
// device's next hook (FlushDeferredVertices).
u32 D3DDevice_BeginVertices_hook(u32 device, u32 primitive, u32 vertex_count, u32 stride) {
  const u64 bytes = u64(vertex_count) * stride;
  if (!bytes || bytes > kVerticesScratchSize)
    return 0;
  // The inline end: m_pRing = [device + kDeviceOffsetVerticesRingEnd].
  StoreU32(device + d3d::kDeviceOffsetVerticesRingEnd,
           LoadU32(device + u32(offsetof(d3d::Device, m_pRing))));

  const u32 scratch = VerticesScratch(device);
  if (!scratch)
    return 0;
  DrawCall call;
  call.primitive = primitive;
  call.count = vertex_count;
  call.up_vertices = GuestPtr<const u8>(scratch);
  call.up_stride = stride;
  call.up_vertex_count = vertex_count;
  // Either way the device's previous Begin*Vertices data leaves the scratch
  // before the game writes this draw's over it.
  if (IsRecordingCommandBuffer(device))
    RecordDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else if (Active())
    QueueDeferredDraw(device, call, call.up_vertices, u32(bytes));
  else
    FlushDeferredVertices(device);
  return scratch;
}

// A clear: the renderer's, recorded or queued, then N&B's device work.
// whole_target: BeginTiling's, of one tile's worth of EDRAM, which on the
// host is the whole tiled target.
void Clear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z,
           u32 stencil, bool whole_target) {
  const ClearRect *host_rect = whole_target ? nullptr : rect;
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, host_rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, host_rect, color, z, stencil);
  const native::Rect native_rect = rect ? native::Rect{rect->x1, rect->y1, rect->x2, rect->y2}
                                        : native::Rect{};
  native::Clear(native::DeviceAt(device), flags, rect ? &native_rect : nullptr);
}

// A resolve: the renderer's copy (and clear), then N&B's device work.
void Resolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x,
             i32 dest_y, u32 dest_level, u32 dest_slice, u32 color_va, float clear_z,
             u32 clear_stencil, u32 parameters) {
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
  native::Resolve(native::DeviceAt(device), flags, dest_texture, clear_z, clear_stencil,
                  parameters);
}

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect,
//                       const D3DVECTOR4 *pColor, float Z, DWORD Stencil) [0x822811A0]
// Z takes r7's slot, so Stencil arrives in r8.
void D3DDevice_ClearF_hook(u32 device, u32 flags, u32 rect_va, u32 color_va, double z,
                           u32 /*z_slot*/, u32 stencil) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};  // unk_82001160 without one
  if (color_va)
    ReadVector4(color_va, color);
  Clear(device, flags, has_rect ? &rect : nullptr, color, float(z), stencil, false);
}

// void D3DDevice_Clear(D3DDevice*, DWORD Count, const D3DRECT *pRects, DWORD Flags,
//     D3DCOLOR Color, float Z, DWORD Stencil) [0x822812C8]: ClearF with the
// color as a vector (XMLoadColor: each byte as 1 + b / 2^23, scaled by
// 2^23 / 255 and offset), per rect. Z takes r8's slot.
void D3DDevice_Clear_hook(u32 device, u32 count, u32 rects_va, u32 flags, u32 color_argb,
                          double z, u32 /*z_slot*/, u32 stencil) {
  const float scale = std::bit_cast<float>(0x47008081u);  // aG, unk_82001150
  auto channel = [scale](u32 byte) {
    return std::fmaf(std::bit_cast<float>(0x3F800000u | (byte & 0xFF)), scale, -scale);
  };
  const float color[4] = {channel(color_argb >> 16), channel(color_argb >> 8),
                          channel(color_argb), channel(color_argb >> 24)};
  if (!rects_va) {
    Clear(device, flags, nullptr, color, float(z), stencil, false);
    return;
  }
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (ReadRect(rects_va + i * 16, rect))
      Clear(device, flags, &rect, color, float(z), stencil, false);
  }
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect,
//     D3DBaseTexture *pDestTexture, const D3DPOINT *pDestPoint, UINT DestLevel,
//     UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [0x8222D6C8]
// ClearZ is in f1 and takes the first stack slot; ClearStencil and pParameters
// are in the two after it.
void D3DDevice_Resolve_hook(u32 device, u32 flags, u32 rect_va, u32 dest_texture, u32 point_va,
                            u32 dest_level, u32 dest_slice, u32 color_va, double clear_z,
                            u32 /*z_slot*/, u32 clear_stencil, u32 parameters) {
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = GuestPtr<const be_u32>(point_va)) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  Resolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level,
          dest_slice, color_va, float(clear_z), clear_stencil, parameters);
}

// void D3DDevice_BeginTiling(D3DDevice*, DWORD Flags, DWORD Count,
//     const D3DRECT *pTileRects, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil) [0x822788A0]
// The surfaces bound from here to EndTiling are backed by targets covering
// every tile (SetTilingExtentLocked). N&B's BeginTiling clears with Z 0.0 and
// stencil 0 (its ClearZ and ClearStencil go unused), unless the device records
// a one-pass Z pass (Flags3 0x20); its ring segment for the tiles is left out.
void D3DDevice_BeginTiling_hook(u32 device, u32 /*flags*/, u32 count, u32 rects_va, u32 color_va,
                                double /*clear_z*/, u32 /*z_slot*/, u32 /*clear_stencil*/) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  std::vector<ClearRect> tiles;
  for (u32 i = 0; i < count; ++i) {
    ClearRect rect;
    if (!ReadRect(rects_va + i * 16, rect))
      break;
    tiles.push_back(rect);
  }
  count = u32(tiles.size());
  if (!IsRecordingCommandBuffer(device)) {
    FlushDeferredVertices(device);
    u32 width = 0, height = 0;
    for (const ClearRect &rect : tiles) {
      width = std::max(width, u32(rect.x2));
      height = std::max(height, u32(rect.y2));
    }
    render_thread::Queue([tiles, width, height] {
      SetTilingExtentLocked(width, height);
      SetTilesLocked(tiles.data(), u32(tiles.size()));
    });
  }

  d.SetW(native::off::TileCount, count);
  u32 extent_x = 0, extent_y = 0;
  for (u32 i = 0; i < count; ++i) {
    const ClearRect &rect = tiles[i];
    extent_x = std::max(extent_x, u32(rect.x2));
    extent_y = std::max(extent_y, u32(rect.y2));
    d.SetW(native::off::TileRects + i * 16, u32(rect.x1));
    d.SetW(native::off::TileRects + i * 16 + 4, u32(rect.y1));
    d.SetW(native::off::TileRects + i * 16 + 8, u32(rect.x2));
    d.SetW(native::off::TileRects + i * 16 + 12, u32(rect.y2));
    d.SetW(native::off::TileOrigins + i * 8, u32(rect.x1) & ~31u);
    d.SetW(native::off::TileOrigins + i * 8 + 4, u32(rect.y1) & ~31u);
  }
  d.SetW(native::off::TilingExtent, extent_x);
  d.SetW(native::off::TilingExtent + 4, extent_y);
  d.SetW(0x3384, 0);

  if (!(d.B(native::off::Flags3) & 0x20)) {
    // The largest tile, from its origin: its clear covers a tile of EDRAM.
    u32 width = 0, height = 0;
    for (u32 i = 0; i < count; ++i) {
      width = std::max(width, u32(tiles[i].x2) - d.W(native::off::TileOrigins + i * 8));
      height = std::max(height, u32(tiles[i].y2) - d.W(native::off::TileOrigins + i * 8 + 4));
    }
    // The viewport (x, y, width, height, min z, max z) and scissor rect, for
    // after the clear.
    float viewport[6];
    for (u32 i = 0; i < 6; ++i)
      viewport[i] = d.F(0x3168 + i * 4);
    i32 scissor[4];
    for (u32 i = 0; i < 4; ++i)
      scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
    native::SetViewport(d, kFullViewport);
    native::SetScissorRect(d, 0, 0, 0xFFFF, 0xFFFF);  // dword_82001190
    const ClearRect rect{0, 0, i32(width), i32(height)};
    float color[4] = {0, 0, 0, 0};
    if (color_va)
      ReadVector4(color_va, color);
    Clear(device, 0x3F, &rect, color, 0.0f, 0, true);
    native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                        viewport[5]);
    native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  }
  if (const auto *color = color_va ? GuestPtr<const u8>(color_va) : nullptr)
    std::memcpy(d.bytes + 0x3390, color, 16);
  else
    std::memset(d.bytes + 0x3390, 0, 16);
  d.SetF(0x33A0, 0.0f);  // flt_82000D18
  d.SetW(0x33A4, 0);

  const u32 target = d.W(native::off::RenderTargets);
  d.SetW(0x31B4, target ? target : d.W(native::off::DepthStencil));
  for (u32 i = 0; i < 5; ++i)  // the 4 targets, then depth
    d.SetW(native::off::TilingSurfaces + i * 4, d.W(native::off::RenderTargets + i * 4));
  d.SetW(0x31B0, 2);
  d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x20);
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The tiles' commands go into a ring segment of their own from a KickOff.
  native::KickOff(device);
  native::SetViewport(d, kFullViewport);
  native::ResetAllState(d);
}

// HRESULT D3DDevice_EndTiling(D3DDevice*, DWORD ResolveFlags,
//     const D3DRECT *pResolveRects, D3DBaseTexture *pDestTexture,
//     const D3DVECTOR4 *pClearColor, float ClearZ, DWORD ClearStencil,
//     const D3DRESOLVE_PARAMETERS *pParameters) [0x82212328]
// N&B's EndTiling resolves each tile's rect (ResolveFlags 0x300: color and
// depth cleared) with BeginTiling's clear values, the last one with
// pClearColor, Z 1.0 and stencil 0; ResolveFlags, pResolveRects, ClearZ,
// ClearStencil and pParameters go unused. The host resolve reads each rect
// from the full-size target. Left out: the tiles' ring segment, D3D's
// WorkerDpc callback that replays it per tile, and its fence's packets.
u32 D3DDevice_EndTiling_hook(u32 device, u32 /*resolve_flags*/, u32 /*rects_va*/,
                             u32 dest_texture, u32 color_va, double /*clear_z*/, u32 /*z_slot*/,
                             u32 /*clear_stencil*/, u32 /*parameters*/) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return 0;
  const u32 count = d.W(native::off::TileCount);
  if (dest_texture) {
    for (u32 i = 0; i < count; ++i) {
      const u32 tile = native::off::TileRects + i * 16;
      ClearRect rect{i32(d.W(tile)), i32(d.W(tile + 4)), i32(d.W(tile + 8)), i32(d.W(tile + 12))};
      // A tile at the surface's edge resolves to a multiple of 8.
      const u32 surface = d.W(0x31B4);
      const auto *s = surface ? GuestPtr<const be_u32>(surface) : nullptr;
      const u32 size = s ? u32(s[9]) : 0;
      if (u32(rect.x2) == (size >> 18) + 1)
        rect.x2 = i32((u32(rect.x2) + 7) & ~7u);
      if (u32(rect.y2) == native::Extract(size, 3, 15) + 1)
        rect.y2 = i32((u32(rect.y2) + 7) & ~7u);
      native::SetPredication(d, 3u << (i * 2));
      native::SetRenderState(d, kRenderStateHiZWriteEnable,
                             (d.B(native::off::Flags3) & 0x20) ? 0 : 2);
      const i32 dest_x = i32(d.W(tile)), dest_y = i32(d.W(tile + 4));
      if (i + 1 < count) {
        Resolve(device, 0x300, &rect, dest_texture, dest_x, dest_y, 0, 0, device + 0x3390,
                d.F(0x33A0), d.W(0x33A4), 0);
      } else {
        Resolve(device, 0x300, &rect, dest_texture, dest_x, dest_y, 0, 0, color_va,
                1.0f /*flt_82000D0C*/, 0, 0);
      }
    }
  }
  native::SetPredication(d, 0);
  native::SetRenderState(d, kRenderStateHiZWriteEnable, 2);
  if (count != 1) {
    // Around the WorkerDpc callback.
    native::SetPredication(d, 0x80000000u);
    native::SetPredication(d, 0);
  }
  native::KickOff(device);

  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0xDF);
  d.SetW(0x31B4, 0);
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // sub_82210138: the segment's fence, unless the device's command memory ran
  // out (EndCommandBuffer marks a device so until the next Begin).
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (out_of_memory) {
    d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
    native::RewindRing(d);
  } else {
    native::SetFence(device);
  }
  native::SetViewport(d, kFullViewport);

  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
  if (out_of_memory) {
    native::ResetAllState(d);
    return kE_OUTOFMEMORY;
  }
  return 0;
}

// void D3DDevice_BeginExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x82269618]. The renderer finds a draw's memexport streams in its shaders.
void D3DDevice_BeginExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::BeginExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_EndExport(D3DDevice*, DWORD Index, D3DResource*, DWORD Flags)
// [0x822693B0]: the resource is busy until the current fence, which completes
// once the render thread has written the stream's results.
void D3DDevice_EndExport_hook(u32 device, u32 index, u32 resource, u32 flags) {
  native::EndExport(native::DeviceAt(device), index, resource, flags);
}

// void D3DDevice_Present(D3DDevice*) [0x82654778]: the automatic back buffer
// resolved into the automatic front buffer, then Swap. N&B creates its device
// without an automatic front buffer, so its callers (XUI, the threaded movie
// player) have nothing to resolve to.
void D3DDevice_Present_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (!d)
    return;
  FlushDeferredVertices(device);
  float viewport[6];
  for (u32 i = 0; i < 6; ++i)
    viewport[i] = d.F(0x3168 + i * 4);
  i32 scissor[4];
  for (u32 i = 0; i < 4; ++i)
    scissor[i] = i32(d.W(native::off::ScissorRect + i * 4));
  native::SetRenderTarget(d, 0, d.W(native::off::AutoBackBuffer));
  // SynchronizeToPresentationInterval (0x8228A748): its swap callback and wait
  // go into the ring; the swap is pending.
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
  const u32 front_buffer = d.W(native::off::AutoFrontBuffer);
  if (front_buffer)
    Resolve(device, 0, nullptr, front_buffer, 0, 0, 0, 0, 0, 1.0f, 0, 0);
  native::KickOff(device);
  native::SetViewport(d, viewport[0], viewport[1], viewport[2], viewport[3], viewport[4],
                      viewport[5]);
  native::SetScissorRect(d, scissor[0], scissor[1], scissor[2], scissor[3]);
  native::Swap(device, front_buffer, device + 0x3548);
}

// void D3DDevice_SetVertexShader(D3DDevice*, D3DVertexShader*) [0x8222A0A8]
void D3DDevice_SetVertexShader_hook(u32 device, u32 shader) {
  native::SetVertexShader(native::DeviceAt(device), shader);
}

// void D3DDevice_SetPixelShader(D3DDevice*, D3DPixelShader*) [0x82226730]
void D3DDevice_SetPixelShader_hook(u32 device, u32 shader) {
  native::SetPixelShader(native::DeviceAt(device), shader);
}

} // namespace

// ---------------------------------------------------------------------------
// sa2-nx (2026-10-20): SA2's hooks (N&B's addresses in brackets)
// ---------------------------------------------------------------------------
// The draws are Renut's at SA2's addresses (same signatures; sa2-nx's own hooks there, src/me/sa2_me_hooks.cpp and
// src/me/sa2_d3d_api.cpp, hand over to these when Renut's renderer is chosen). SA2 does not tile, export, record
// command buffers or draw tessellated, and its Present is left to its own code (its Resolve and Swap are hooked).
//
// Clear and Resolve: the host work is queued from the device as it is at the call (the same snapshot N&B's hooks
// take), then SA2's own function does the device work N&B's hooks did natively (d3d_native.h is N&B's layout): its
// packets go into the ring sink, its inline data into the scratch (device.cpp), its fences through the hooked
// BlockOnFence and KickOff. SA2's Clear (sub_82563AB0) calls ClearF once per rect, and the game calls ClearF directly
// too, so ClearF is the hook.

namespace {

void QueueHostClear(u32 device, u32 flags, const ClearRect *rect, const float color[4], float z, u32 stencil) {
  if (IsRecordingCommandBuffer(device))
    RecordClear(device, flags, rect, color, z, stencil);
  else if (Active())
    QueueClear(device, flags, rect, color, z, stencil);
}

void QueueHostResolve(u32 device, u32 flags, const ClearRect *rect, u32 dest_texture, i32 dest_x, i32 dest_y,
                      u32 dest_level, u32 dest_slice, u32 color_va, float clear_z, u32 clear_stencil) {
  float color[4] = {0, 0, 0, 0};
  const bool has_color = color_va && ReadVector4(color_va, color);
  if (IsRecordingCommandBuffer(device)) {
    RecordResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                  has_color ? color : nullptr, clear_z, clear_stencil);
  } else if (Active()) {
    QueueResolve(device, flags, rect, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                 has_color ? color : nullptr, clear_z, clear_stencil);
  }
}

}  // namespace

#if !RENUT_GAME_FNC  // fnc-nx: SA2's draw hooks below; FNC's at the end of the file
BANJO_RENUT_HOOK_SHARED(sub_82565C60, D3DDevice_DrawVertices_hook);  // D3DDevice_DrawVertices [0x8222C7A0]
BANJO_RENUT_HOOK_SHARED(sub_82566078, D3DDevice_DrawIndexedVertices_hook);  // D3DDevice_DrawIndexedVertices [0x826567C8]
BANJO_RENUT_HOOK_SHARED(sub_82565C18, D3DDevice_DrawVerticesUP_hook);  // D3DDevice_DrawVerticesUP [0x8222E520]
BANJO_RENUT_HOOK_SHARED(sub_82565758, D3DDevice_BeginVertices_hook);  // D3DDevice_BeginVertices [0x8222CC50]

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect, const D3DVECTOR4 *pColor, float Z,
//     DWORD Stencil) [sub_82563978; N&B 0x822811A0]: Z in f1 (r7's slot), Stencil in r8.
BANJO_RENUT_HOOK_RAW(sub_82563978) {
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect_va = ctx.r5.u32, color_va = ctx.r6.u32;
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};
  if (color_va)
    ReadVector4(color_va, color);
  QueueHostClear(device, flags, has_rect ? &rect : nullptr, color, float(ctx.f1.f64), ctx.r8.u32);
  __imp__sub_82563978(ctx, base);
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect, D3DBaseTexture *pDestTexture,
//     const D3DPOINT *pDestPoint, UINT DestLevel, UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [sub_825656F8; N&B 0x8222D6C8]: ClearZ in f1,
// ClearStencil and pParameters in the caller's argument words at +92 and +100 (sub_825656F8 reads them there). Every
// resolve of SA2 comes here: the game's, the Present's and Direct3D's own (sub_825645A0, the core, has no other
// caller).
BANJO_RENUT_HOOK_RAW(sub_825656F8) {
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect_va = ctx.r5.u32, dest_texture = ctx.r6.u32;
  const u32 point_va = ctx.r7.u32, dest_level = ctx.r8.u32, dest_slice = ctx.r9.u32, color_va = ctx.r10.u32;
  const u32 clear_stencil = LoadU32(ctx.r1.u32 + 92);
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = point_va ? GuestPtr<const be_u32>(point_va) : nullptr) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  QueueHostResolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                   color_va, float(ctx.f1.f64), clear_stencil);
  __imp__sub_825656F8(ctx, base);
}
#else  // RENUT_GAME_FNC
// ---------------------------------------------------------------------------
// fnc-nx: Fight Night Champion's draw, clear and resolve hooks (Direct3D 2.0.11626)
// ---------------------------------------------------------------------------
// Each address and its evidence: docs/fnc/stage2.md. FNC has no src/me hooks sharing these addresses, so SA2's
// BANJO_RENUT_HOOK_SHARED draws are plain hooks here. Unlike SA2, FNC tiles every frame (BeginTiling sub_82CDA978 /
// EndTiling sub_82CDAFE0): the predicated-tiling path of these hooks (N&B's) runs, and must be checked against FNC
// Reborn frame by frame.
//   DrawVertices (type, start, count): sub_82CE5280, about 54 per frame in a fight (quad lists).
//   DrawIndexedVertices (type, base, start, count): sub_82CE5670, about 450 per frame in a fight.
//   DrawVerticesUP (type, count, data, stride): sub_82CE5238 (BeginVertices + memcpy of the caller's vertices).
//   BeginVertices (type, count, stride): sub_82CE4DA0, the callers write the vertices.
BANJO_RENUT_HOOK(sub_82CE5280, D3DDevice_DrawVertices_hook);  // D3DDevice_DrawVertices [0x8222C7A0]
BANJO_RENUT_HOOK(sub_82CE5670, D3DDevice_DrawIndexedVertices_hook);  // D3DDevice_DrawIndexedVertices [0x826567C8]
BANJO_RENUT_HOOK(sub_82CE5238, D3DDevice_DrawVerticesUP_hook);  // D3DDevice_DrawVerticesUP [0x8222E520]
BANJO_RENUT_HOOK(sub_82CE4DA0, D3DDevice_BeginVertices_hook);  // D3DDevice_BeginVertices [0x8222CC50]

// void D3DDevice_ClearF(D3DDevice*, DWORD Flags, const D3DRECT *pRect, const D3DVECTOR4 *pColor, float Z,
//     DWORD Stencil) [sub_82CE6D10; N&B 0x822811A0]: Z in f1, Stencil in r8. FNC's D3DDevice_Clear (sub_82CE6E38)
// turns its D3DCOLOR into the float color and calls this, so ClearF is the hook, as in SA2.
BANJO_RENUT_HOOK_RAW(sub_82CE6D10) {
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect_va = ctx.r5.u32, color_va = ctx.r6.u32;
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  float color[4] = {0, 0, 0, 0};
  if (color_va)
    ReadVector4(color_va, color);
  QueueHostClear(device, flags, has_rect ? &rect : nullptr, color, float(ctx.f1.f64), ctx.r8.u32);
  __imp__sub_82CE6D10(ctx, base);
}

// void D3DDevice_Resolve(D3DDevice*, DWORD Flags, const D3DRECT *pSourceRect, D3DBaseTexture *pDestTexture,
//     const D3DPOINT *pDestPoint, UINT DestLevel, UINT DestSliceOrFace, const D3DVECTOR4 *pClearColor, float ClearZ,
//     DWORD ClearStencil, const D3DRESOLVE_PARAMETERS *pParameters) [sub_82CDD200; N&B 0x8222D6C8]. FNC's frame is
// 160 bytes and it loads 252(r1) / 260(r1): ClearStencil and pParameters are the caller's words at +92 / +100, as in
// SA2. It reorders the arguments for the worker sub_82CDC130 (the EDRAM copy registers). The game's resolves, the
// Present's and EndTiling's (sub_82CDAFE0, twice per frame) all come here.
BANJO_RENUT_HOOK_RAW(sub_82CDD200) {
  const u32 device = ctx.r3.u32, flags = ctx.r4.u32, rect_va = ctx.r5.u32, dest_texture = ctx.r6.u32;
  const u32 point_va = ctx.r7.u32, dest_level = ctx.r8.u32, dest_slice = ctx.r9.u32, color_va = ctx.r10.u32;
  const u32 clear_stencil = LoadU32(ctx.r1.u32 + 92);
  ClearRect rect;
  const bool has_rect = rect_va && ReadRect(rect_va, rect);
  i32 dest_x = 0, dest_y = 0;
  if (const auto *p = point_va ? GuestPtr<const be_u32>(point_va) : nullptr) {
    dest_x = i32(u32(p[0]));
    dest_y = i32(u32(p[1]));
  }
  QueueHostResolve(device, flags, has_rect ? &rect : nullptr, dest_texture, dest_x, dest_y, dest_level, dest_slice,
                   color_va, float(ctx.f1.f64), clear_stencil);
  __imp__sub_82CDD200(ctx, base);
}

// fnc-nx (2026-11-21): the render thread's tiling state. FNC's own BeginTiling (sub_82CDA978) and EndTiling
// (sub_82CDAFE0) run as recompiled code and write the device's tiling words (TileCount, TileRects, TilingExtent at
// FNC's offsets), but only Renut's BeginTiling hook told the render thread (SetTilingExtentLocked / SetTilesLocked),
// so a host target stayed the size of one tile's surface. Found in the prison opening's knockdown (the first console
// run's broken scene): 4x MSAA at 1280x720 tiles as three 1280x256 bands, the scene was drawn into a 1280x256 target
// and the bands at rows 256 and 512 resolved nothing, a strip at the top and black below, where FNC Reborn draws the
// whole view. These wrappers keep the game's own device writes and add only the render thread's state, as Renut's
// hooks do around N&B's (BeginTiling: the deferred vertices flushed first, then the extent and tiles; EndTiling: after
// its resolves, which reach the hooked resolve above while the extent is still set, back to none).
BANJO_RENUT_HOOK_RAW(sub_82CDA978) {
  const u32 device = ctx.r3.u32;
  if (!IsRecordingCommandBuffer(device))
    FlushDeferredVertices(device);
  __imp__sub_82CDA978(ctx, base);
  native::Device d = native::DeviceAt(device);
  if (!d || IsRecordingCommandBuffer(device))
    return;
  std::vector<ClearRect> tiles;
  // At most 15: FNC's TileRects (12928) holds 15 rects of 16 bytes before TileOrigins (13168).
  const u32 count = std::min<u32>(d.W(native::off::TileCount), 15);
  u32 width = 0, height = 0;
  for (u32 i = 0; i < count; ++i) {
    const u32 tile = native::off::TileRects + i * 16;
    ClearRect rect{i32(d.W(tile)), i32(d.W(tile + 4)), i32(d.W(tile + 8)), i32(d.W(tile + 12))};
    tiles.push_back(rect);
    width = std::max(width, u32(rect.x2));
    height = std::max(height, u32(rect.y2));
  }
  static std::atomic<uint32_t> logged{0};
  if (logged.fetch_add(1, std::memory_order_relaxed) < 8) {
    RGPU_INFO("[fnc tiling] BeginTiling: {} tiles, extent {}x{}, first ({},{})-({},{}); predication {:08X}, "
              "run {:08X} {:08X}, set {:08X}, tile/run pred {:08X} {:08X}, flags {:02X} {:02X} {:02X} {:02X}",
              count, width, height, count ? tiles[0].x1 : 0, count ? tiles[0].y1 : 0, count ? tiles[0].x2 : 0,
              count ? tiles[0].y2 : 0, d.W(native::off::Predication), d.W(native::off::RunPredicationState),
              d.W(native::off::RunPredicationState + 4), d.W(native::off::PredicationSet),
              d.W(native::off::TilePredication), d.W(native::off::RunPredication), d.B(native::off::Flags),
              d.B(native::off::Flags1), d.B(native::off::Flags2), d.B(native::off::Flags3));
  }
  render_thread::Queue([tiles, width, height] {
    SetTilingExtentLocked(width, height);
    SetTilesLocked(tiles.data(), u32(tiles.size()));
  });
}

BANJO_RENUT_HOOK_RAW(sub_82CDAFE0) {
  const u32 device = ctx.r3.u32;
  __imp__sub_82CDAFE0(ctx, base);
  if (!IsRecordingCommandBuffer(device)) {
    render_thread::Queue([] {
      SetTilingExtentLocked(0, 0);
      SetTilesLocked(nullptr, 0);
    });
  }
}
#endif  // RENUT_GAME_FNC
