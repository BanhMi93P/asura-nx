/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/banjo_renut_tuning.h"  // blur-nx (2026-11-10, port 76; its key 213): OcclusionQueriesOn
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/render/surfaces.h"  // blur-nx (2026-11-10, port 76; its key 215): NoteSurfaceHeader
#include <algorithm>

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x82636ED0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x82636F10]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x82636E40]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82244B18]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x32E0 + i * 4)));
  viewport[4] = d.W(0x32F0);
  viewport[5] = d.W(0x32F4);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x826370E8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x82637138]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.TouchRange(constant, 24);  // banjo-nx (2026-10-26, banjo_renut_tags)
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x821F0B40], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x8263ED90]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3614));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x821FE558]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x821F9438]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x821DE438]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x821D5060]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x821F2090]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x821F2110]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x821F2368]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x821F2460]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x82248BD0]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  NoteSurfaceHeader(surface);  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers)
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x82248FB0]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  NoteSurfaceHeader(surface);  // blur-nx (2026-11-10, port 76; its key 215: blur_renut_stack_surface_headers)
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x82248F20]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x821F9A48]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x826411C0]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x821EFD38]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x82637208]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x826414C0]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x82634C10]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82255C98]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x82634B90]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82640010] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// blur-nx (2026-11-10/11, port 76; its key 213: blur_renut_occlusion_queries): D3DQuery_GetData's answer. Renut runs no
// GPU packets, so the counts an occlusion query waits for never come: under Renut, Issue(END) leaves the sample blocks
// as they were (zeros, then whatever the memory held) and GetData said "not ready" to every query for ever, so whatever
// a game hides by a query was never drawn (Blur's light glows, the tail lights' red glow). AnswerOcclusionQuery, called
// before the game's own GetData runs, writes into every block of an issued occlusion query what the reference's GPU
// writes at the ZPASS event (the SDK's command_processor.cpp, ExecutePacketType3_EVENT_WRITE_ZPD): the block zeroed,
// Total_A and ZPass_A = 1000 samples passed (little-endian, as the GPU writes them), and clears the issue's fence word,
// which GetData compares with the GPU's read pointer: the counts are in.
// The query object as XDK 2.0.11164's GetData reads it (blur-nx, sub_8224CD70; D3DQuery_Issue is sub_8224CAE8, the two
// functions holding the 0xFFFFFEED sentinel): type 9 (occlusion) at +4, issued (bit 0x40) in the byte at +20, the fence
// at +24, up to 30 sample-block pointers from +28 and their number at +148, 64-byte blocks (END counts at +0, BEGIN at
// +32). A game at another XDK checks its own GetData against this before it registers it.
// Not hooked in the base: Beautiful Katamari's XDK 2.0.5632 image links no D3DQuery_Issue / GetData (no instruction or
// data word of it holds 0xFFFFFEED). A game whose image has GetData adds RENUT_D3DQUERY_GETDATA_HOOK(sub_<address>) to
// the hook list below (or calls AnswerOcclusionQuery from a GetData hook of its own), and runs post-codegen again
// (tools/post_codegen.py: direct_calls.py --redo) so GetData's callers go through the hooked entry.
namespace renut::gpu::native {
namespace {
constexpr u32 kQueryFakeSamples = 1000;  // the SDK's query_occlusion_fake_sample_count
u32 QueryBe(const u8 *p) { return u32(p[0]) << 24 | u32(p[1]) << 16 | u32(p[2]) << 8 | u32(p[3]); }
void QueryLe(u8 *p, u32 v) {
  p[0] = u8(v);
  p[1] = u8(v >> 8);
  p[2] = u8(v >> 16);
  p[3] = u8(v >> 24);
}
}  // namespace

// blur-nx (2026-11-11, port 76; its key 213): a query block's physical address as the game's GetData forms it from a
// pointer at query +28: (p & 0x1FFFFFFF) + (((p >> 20) + 0x200) & 0x1000), read through the 0xC0000000 mapping. A
// pointer in the 0xE0000000 range (Blur's blocks: E6D8DFC0) is a page further on (the console's 4 KB offset of that
// range). Answers written at the pointer's own page were never read: GetData read the sentinel there and said not ready.
u32 QueryBlockPhysical(u32 p) { return (p & 0x1FFFFFFFu) + ((((p >> 20) + 0x200u) & 0x1000u)); }

bool AnswerOcclusionQuery(u32 query) {
  if (!banjo::renut_gpu::OcclusionQueriesOn())
    return false;
  u8 *q = GuestPtr<u8>(query);
  if (!q || QueryBe(q + 4) != 9 || !(q[20] & 0x40))
    return false;
  const u32 blocks = std::min<u32>(QueryBe(q + 148), 30);
  bool answered = false;
  for (u32 i = 0; i < blocks; ++i) {
    const u32 pointer = QueryBe(q + 28 + 4 * i);
    if (!pointer)
      continue;
    // Every issued query, not only blocks holding the sentinel (under Renut they hold zeros or leftover memory): the
    // reference's answer for every query is 1000 passed.
    u8 *block = GuestPhysicalPtr<u8>(QueryBlockPhysical(pointer));
    std::memset(block, 0, 64);
    QueryLe(block + 0, kQueryFakeSamples);   // Total_A
    QueryLe(block + 16, kQueryFakeSamples);  // ZPass_A
    answered = true;
  }
  if (answered) {
    std::memset(q + 24, 0, 4);
    static uint32_t logged = 0;
    if (logged++ < 4)
      RGPU_INFO("[renut] occlusion query {:08X}: {} block(s) answered with {} samples passed, as the reference's GPU "
                "does (blur_renut_occlusion_queries)", query, blocks, kQueryFakeSamples);
  }
  return answered;
}
}  // namespace renut::gpu::native

// blur-nx (2026-11-10/11, port 76; its key 213): D3DQuery_GetData(query, data, size, flags) at the game's own address:
// the answer above, then the game's GetData (its __imp__), whose result the game gets. Under the emulated renderer the
// game's GetData alone (BANJO_RENUT_HOOK). One line in the hook list: RENUT_D3DQUERY_GETDATA_HOOK(sub_8224CD70);
// (blur-nx's address; the base registers none, see AnswerOcclusionQuery).
#define RENUT_D3DQUERY_GETDATA_HOOK(subroutine)                                                                   \
  REX_IMPORT(__imp__##subroutine, renut_query_get_data_original_##subroutine, u32(u32, u32, u32, u32));          \
  static u32 renut_query_get_data_##subroutine(u32 query, u32 data, u32 size, u32 flags) {                        \
    ::renut::gpu::native::AnswerOcclusionQuery(query);                                                            \
    return renut_query_get_data_original_##subroutine(query, data, size, flags);                                  \
  }                                                                                                               \
  BANJO_RENUT_HOOK(subroutine, renut_query_get_data_##subroutine)

// Render states, by their index in Asura's Wrath's table (0x831FE158).
BANJO_RENUT_HOOK(sub_829923F8, SetRenderStateHook<10>);
BANJO_RENUT_HOOK(sub_82992468, SetRenderStateHook<11>);
BANJO_RENUT_HOOK(sub_82992438, SetRenderStateHook<12>);
BANJO_RENUT_HOOK(sub_82991CF0, SetRenderStateHook<13>);
BANJO_RENUT_HOOK(sub_82991CC0, SetRenderStateHook<14>);
BANJO_RENUT_HOOK(sub_82991D58, SetRenderStateHook<15>);
BANJO_RENUT_HOOK(sub_829920E8, SetRenderStateHook<16>);
BANJO_RENUT_HOOK(sub_82992210, SetRenderStateHook<17>);
BANJO_RENUT_HOOK(sub_82991E78, SetRenderStateHook<18>);
BANJO_RENUT_HOOK(sub_82991F08, SetRenderStateHook<19>);
BANJO_RENUT_HOOK(sub_82991DE8, SetRenderStateHook<20>);
BANJO_RENUT_HOOK(sub_82992008, SetRenderStateHook<21>);
BANJO_RENUT_HOOK(sub_82992078, SetRenderStateHook<22>);
BANJO_RENUT_HOOK(sub_82991F98, SetRenderStateHook<23>);
BANJO_RENUT_HOOK(sub_82991D20, SetRenderStateHook<24>);
BANJO_RENUT_HOOK(sub_82992180, SetRenderStateHook<25>);
BANJO_RENUT_HOOK(sub_829921E0, SetRenderStateHook<26>);
BANJO_RENUT_HOOK(sub_829924A0, SetRenderStateHook<27>);
BANJO_RENUT_HOOK(sub_829924E0, SetRenderStateHook<28>);
BANJO_RENUT_HOOK(sub_82992548, SetRenderStateHook<29>);
BANJO_RENUT_HOOK(sub_82992580, SetRenderStateHook<30>);
BANJO_RENUT_HOOK(sub_829925B8, SetRenderStateHook<31>);
BANJO_RENUT_HOOK(sub_82992518, SetRenderStateHook<32>);
BANJO_RENUT_HOOK(sub_829926B8, SetRenderStateHook<33>);
BANJO_RENUT_HOOK(sub_829926D8, SetRenderStateHook<34>);
BANJO_RENUT_HOOK(sub_829926F8, SetRenderStateHook<35>);
BANJO_RENUT_HOOK(sub_82992618, SetRenderStateHook<36>);
BANJO_RENUT_HOOK(sub_82992650, SetRenderStateHook<37>);
BANJO_RENUT_HOOK(sub_82992688, SetRenderStateHook<38>);
BANJO_RENUT_HOOK(sub_829925E8, SetRenderStateHook<39>);
BANJO_RENUT_HOOK(sub_82992718, SetRenderStateHook<40>);
BANJO_RENUT_HOOK(sub_82992738, SetRenderStateHook<41>);
BANJO_RENUT_HOOK(sub_82992758, SetRenderStateHook<42>);
BANJO_RENUT_HOOK(sub_82992778, SetRenderStateHook<43>);
BANJO_RENUT_HOOK(sub_82992AC0, SetRenderStateHook<44>);
BANJO_RENUT_HOOK(sub_82992B20, SetRenderStateHook<45>);
BANJO_RENUT_HOOK(sub_82992AB0, SetRenderStateHook<46>);
BANJO_RENUT_HOOK(sub_82992B78, SetRenderStateHook<47>);
BANJO_RENUT_HOOK(sub_82992960, SetRenderStateHook<48>);
BANJO_RENUT_HOOK(sub_82992990, SetRenderStateHook<49>);
BANJO_RENUT_HOOK(sub_829927D0, SetRenderStateHook<50>);
BANJO_RENUT_HOOK(sub_829927E8, SetRenderStateHook<51>);
BANJO_RENUT_HOOK(sub_829928B0, SetRenderStateHook<52>);
BANJO_RENUT_HOOK(sub_829929B0, SetRenderStateHook<53>);
BANJO_RENUT_HOOK(sub_829929F0, SetRenderStateHook<54>);
BANJO_RENUT_HOOK(sub_82992A30, SetRenderStateHook<55>);
BANJO_RENUT_HOOK(sub_82992A70, SetRenderStateHook<56>);
BANJO_RENUT_HOOK(sub_82993250, SetRenderStateHook<57>);
BANJO_RENUT_HOOK(sub_829931E0, SetRenderStateHook<58>);
BANJO_RENUT_HOOK(sub_82993218, SetRenderStateHook<59>);
BANJO_RENUT_HOOK(sub_82992BD0, SetRenderStateHook<60>);
BANJO_RENUT_HOOK(sub_82992BF0, SetRenderStateHook<61>);
BANJO_RENUT_HOOK(sub_82992C18, SetRenderStateHook<62>);
BANJO_RENUT_HOOK(sub_82992C40, SetRenderStateHook<63>);
BANJO_RENUT_HOOK(sub_82992C68, SetRenderStateHook<64>);
BANJO_RENUT_HOOK(sub_82992C90, SetRenderStateHook<65>);
BANJO_RENUT_HOOK(sub_82992CB8, SetRenderStateHook<66>);
BANJO_RENUT_HOOK(sub_82992CE0, SetRenderStateHook<67>);
BANJO_RENUT_HOOK(sub_82992D00, SetRenderStateHook<68>);
BANJO_RENUT_HOOK(sub_82992D20, SetRenderStateHook<69>);
BANJO_RENUT_HOOK(sub_82992D48, SetRenderStateHook<70>);
BANJO_RENUT_HOOK(sub_82992D70, SetRenderStateHook<71>);
BANJO_RENUT_HOOK(sub_82992D98, SetRenderStateHook<72>);
BANJO_RENUT_HOOK(sub_82992DC0, SetRenderStateHook<73>);
BANJO_RENUT_HOOK(sub_82992DE8, SetRenderStateHook<74>);
BANJO_RENUT_HOOK(sub_82992E10, SetRenderStateHook<75>);
BANJO_RENUT_HOOK(sub_82992F30, SetRenderStateHook<76>);
BANJO_RENUT_HOOK(sub_82992F80, SetRenderStateHook<77>);
BANJO_RENUT_HOOK(sub_82993018, SetRenderStateHook<78>);
BANJO_RENUT_HOOK(sub_829930B0, SetRenderStateHook<79>);
BANJO_RENUT_HOOK(sub_82993148, SetRenderStateHook<80>);
BANJO_RENUT_HOOK(sub_82993288, SetRenderStateHook<81>);
BANJO_RENUT_HOOK(sub_829932C0, SetRenderStateHook<82>);
BANJO_RENUT_HOOK(sub_829932F0, SetRenderStateHook<83>);
BANJO_RENUT_HOOK(sub_82993318, SetRenderStateHook<84>);
BANJO_RENUT_HOOK(sub_82993348, SetRenderStateHook<85>);
BANJO_RENUT_HOOK(sub_82993370, SetRenderStateHook<86>);
BANJO_RENUT_HOOK(sub_829933A0, SetRenderStateHook<87>);
BANJO_RENUT_HOOK(sub_829933D0, SetRenderStateHook<88>);
BANJO_RENUT_HOOK(sub_829933F8, SetRenderStateHook<89>);
BANJO_RENUT_HOOK(sub_829934C0, SetRenderStateHook<90>);
BANJO_RENUT_HOOK(sub_829934F0, SetRenderStateHook<91>);
BANJO_RENUT_HOOK(sub_82993520, SetRenderStateHook<92>);
BANJO_RENUT_HOOK(sub_82993550, SetRenderStateHook<93>);
BANJO_RENUT_HOOK(sub_82993570, SetRenderStateHook<94>);
BANJO_RENUT_HOOK(sub_82993580, SetRenderStateHook<95>);
BANJO_RENUT_HOOK(sub_82993428, SetRenderStateHook<96>);
BANJO_RENUT_HOOK(sub_82993458, SetRenderStateHook<97>);
BANJO_RENUT_HOOK(sub_82992318, SetRenderStateHook<98>);
BANJO_RENUT_HOOK(sub_82992350, SetRenderStateHook<99>);
BANJO_RENUT_HOOK(sub_829923C8, SetRenderStateHook<100>);

// Sampler states, by their index in Asura's Wrath's table (0x831FE618).
BANJO_RENUT_HOOK(sub_82993D20, SetSamplerStateHook<0>);
BANJO_RENUT_HOOK(sub_82993D70, SetSamplerStateHook<1>);
BANJO_RENUT_HOOK(sub_82993DC0, SetSamplerStateHook<2>);
BANJO_RENUT_HOOK(sub_82993CB0, SetSamplerStateHook<3>);
BANJO_RENUT_HOOK(sub_82993748, SetSamplerStateHook<4>);
BANJO_RENUT_HOOK(sub_829935A0, SetSamplerStateHook<5>);
BANJO_RENUT_HOOK(sub_829938E8, SetSamplerStateHook<6>);
BANJO_RENUT_HOOK(sub_82993B08, SetSamplerStateHook<7>);
BANJO_RENUT_HOOK(sub_82993BB0, SetSamplerStateHook<8>);
BANJO_RENUT_HOOK(sub_829939E8, SetSamplerStateHook<9>);
BANJO_RENUT_HOOK(sub_82993848, SetSamplerStateHook<10>);
BANJO_RENUT_HOOK(sub_829936A0, SetSamplerStateHook<11>);
BANJO_RENUT_HOOK(sub_82993940, SetSamplerStateHook<12>);
BANJO_RENUT_HOOK(sub_82993C30, SetSamplerStateHook<13>);
BANJO_RENUT_HOOK(sub_82993E10, SetSamplerStateHook<14>);
BANJO_RENUT_HOOK(sub_82993A60, SetSamplerStateHook<15>);
BANJO_RENUT_HOOK(sub_82993E68, SetSamplerStateHook<16>);
BANJO_RENUT_HOOK(sub_82993EC0, SetSamplerStateHook<17>);
BANJO_RENUT_HOOK(sub_82993F18, SetSamplerStateHook<18>);
BANJO_RENUT_HOOK(sub_82993F70, SetSamplerStateHook<19>);

BANJO_RENUT_HOOK(sub_829969D0, D3DDevice_SetTexture_hook);  // D3DDevice_SetTexture
BANJO_RENUT_UNHOOKED_HOOK(sub_824A3D40, D3DDevice_SetTexture_hook);
BANJO_RENUT_HOOK(sub_82994148, D3DDevice_SetStreamSource_hook);  // D3DDevice_SetStreamSource
BANJO_RENUT_HOOK(sub_82994268, D3DDevice_SetIndices_hook);  // D3DDevice_SetIndices
BANJO_RENUT_UNHOOKED_HOOK(sub_824D4B48, D3DDevice_SetVertexDeclaration_hook);
BANJO_RENUT_HOOK(sub_829940C8, D3DDevice_SetViewport_hook);  // D3DDevice_SetViewport
BANJO_RENUT_HOOK(sub_82991A60, D3D_SetViewport_hook);  // D3D_SetViewport
BANJO_RENUT_HOOK(sub_82993FD0, D3DDevice_SetScissorRect_hook);  // D3DDevice_SetScissorRect
BANJO_RENUT_HOOK(sub_82991778, D3D_SetSurfaceClip_hook);  // D3D_SetSurfaceClip
BANJO_RENUT_HOOK(sub_829942F8, D3DDevice_SetRenderTarget_hook);  // D3DDevice_SetRenderTarget
BANJO_RENUT_HOOK(sub_82994688, D3DDevice_SetDepthStencilSurface_hook);  // D3DDevice_SetDepthStencilSurface
BANJO_RENUT_HOOK(sub_829916E8, D3D_SetSurfaceInfo_hook);  // D3D_SetSurfaceInfo
BANJO_RENUT_UNHOOKED_HOOK(sub_824EFA58, D3DDevice_SetPredication_hook);
BANJO_RENUT_UNHOOKED_HOOK_SHARED(sub_826411C0, D3DDevice_SetCommandBufferPredication_hook);
BANJO_RENUT_HOOK(sub_8299EB08, D3DDevice_SetShaderGPRAllocation_hook);  // D3DDevice_SetShaderGPRAllocation
BANJO_RENUT_HOOK(sub_8299E1D8, D3D_CleanupGprAllocationSetup_hook);  // D3D_CleanupGprAllocationSetup
BANJO_RENUT_HOOK(sub_8299FE48, D3DDevice_SetScreenExtentQueryMode_hook);  // D3DDevice_SetScreenExtentQueryMode
BANJO_RENUT_UNHOOKED_HOOK(sub_824D0488, D3DDevice_UnsetAll_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_829913F8, D3D_ResetAllState_hook);  // D3D_ResetAllState (label inside sub_829911F8)
BANJO_RENUT_HOOK(sub_8299F228, D3D_InvalidateConstants_hook);  // D3D_InvalidateConstants
BANJO_RENUT_HOOK(sub_82998650, D3DDevice_BlockUntilIdle_hook);  // D3DDevice_BlockUntilIdle
BANJO_RENUT_UNHOOKED_HOOK(sub_82636EC8, D3DDevice_SetRenderTarget_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_82636ED0, D3DDevice_GetRenderTarget_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_82636F10, D3DDevice_GetDepthStencilSurface_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_82636E40, D3DDevice_GetStreamSource_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_82244B18, D3DDevice_GetViewport_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_826370E8, D3DDevice_SetClipPlane_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_82637138, D3DDevice_SetVertexFetchConstant_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_824D4470, D3DDevice_SetVertexShaderConstantB_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_824D44D0, D3DDevice_SetPixelShaderConstantB_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_826374B0, D3DDevice_SetVertexShaderConstantI_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_82637500, D3DDevice_SetPixelShaderConstantI_hook);
BANJO_RENUT_UNHOOKED_HOOK(sub_8263ED90, D3DDevice_EndIndexedVertices_hook);
// Asura's Wrath D3DQuery_GetData (sub_8299FAE8: r3 = query, r4 = data):
BANJO_RENUT_HOOK_RAW(sub_8299FAE8) {
  ::renut::gpu::native::AnswerOcclusionQuery(ctx.r3.u32);
  __imp__sub_8299FAE8(ctx, base);
}

// Render state getters (0x831FE158, field 0).
BANJO_RENUT_HOOK(sub_82992430, GetRenderStateHook<10>);
BANJO_RENUT_HOOK(sub_82992490, GetRenderStateHook<11>);
BANJO_RENUT_HOOK(sub_82992458, GetRenderStateHook<12>);
BANJO_RENUT_HOOK(sub_82991D10, GetRenderStateHook<13>);
BANJO_RENUT_HOOK(sub_82991CE0, GetRenderStateHook<14>);
BANJO_RENUT_HOOK(sub_82991DD8, GetRenderStateHook<15>);
BANJO_RENUT_HOOK(sub_82992170, GetRenderStateHook<16>);
BANJO_RENUT_HOOK(sub_829922A8, GetRenderStateHook<17>);
BANJO_RENUT_HOOK(sub_82991EF8, GetRenderStateHook<18>);
BANJO_RENUT_HOOK(sub_82991F88, GetRenderStateHook<19>);
BANJO_RENUT_HOOK(sub_82991E68, GetRenderStateHook<20>);
BANJO_RENUT_HOOK(sub_82992068, GetRenderStateHook<21>);
BANJO_RENUT_HOOK(sub_829920D8, GetRenderStateHook<22>);
BANJO_RENUT_HOOK(sub_82991FF8, GetRenderStateHook<23>);
BANJO_RENUT_HOOK(sub_82991D48, GetRenderStateHook<24>);
BANJO_RENUT_HOOK(sub_829921B8, GetRenderStateHook<25>);
BANJO_RENUT_HOOK(sub_82992200, GetRenderStateHook<26>);
BANJO_RENUT_HOOK(sub_829924D8, GetRenderStateHook<27>);
BANJO_RENUT_HOOK(sub_82992508, GetRenderStateHook<28>);
BANJO_RENUT_HOOK(sub_82992570, GetRenderStateHook<29>);
BANJO_RENUT_HOOK(sub_829925A8, GetRenderStateHook<30>);
BANJO_RENUT_HOOK(sub_829925D8, GetRenderStateHook<31>);
BANJO_RENUT_HOOK(sub_82992538, GetRenderStateHook<32>);
BANJO_RENUT_HOOK(sub_829926D0, GetRenderStateHook<33>);
BANJO_RENUT_HOOK(sub_829926F0, GetRenderStateHook<34>);
BANJO_RENUT_HOOK(sub_82992710, GetRenderStateHook<35>);
BANJO_RENUT_HOOK(sub_82992640, GetRenderStateHook<36>);
BANJO_RENUT_HOOK(sub_82992678, GetRenderStateHook<37>);
BANJO_RENUT_HOOK(sub_829926A8, GetRenderStateHook<38>);
BANJO_RENUT_HOOK(sub_82992608, GetRenderStateHook<39>);
BANJO_RENUT_HOOK(sub_82992730, GetRenderStateHook<40>);
BANJO_RENUT_HOOK(sub_82992750, GetRenderStateHook<41>);
BANJO_RENUT_HOOK(sub_82992770, GetRenderStateHook<42>);
BANJO_RENUT_HOOK(sub_829927C0, GetRenderStateHook<43>);
BANJO_RENUT_HOOK(sub_82992B10, GetRenderStateHook<44>);
BANJO_RENUT_HOOK(sub_82992B68, GetRenderStateHook<45>);
BANJO_RENUT_HOOK(sub_82992AB8, GetRenderStateHook<46>);
BANJO_RENUT_HOOK(sub_82992BC0, GetRenderStateHook<47>);
BANJO_RENUT_HOOK(sub_82992980, GetRenderStateHook<48>);
BANJO_RENUT_HOOK(sub_829929A8, GetRenderStateHook<49>);
BANJO_RENUT_HOOK(sub_829927E0, GetRenderStateHook<50>);
BANJO_RENUT_HOOK(sub_82992890, GetRenderStateHook<51>);
BANJO_RENUT_HOOK(sub_82992950, GetRenderStateHook<52>);
BANJO_RENUT_HOOK(sub_829929E8, GetRenderStateHook<53>);
BANJO_RENUT_HOOK(sub_82992A28, GetRenderStateHook<54>);
BANJO_RENUT_HOOK(sub_82992A68, GetRenderStateHook<55>);
BANJO_RENUT_HOOK(sub_82992AA8, GetRenderStateHook<56>);
BANJO_RENUT_HOOK(sub_82993278, GetRenderStateHook<57>);
BANJO_RENUT_HOOK(sub_82993208, GetRenderStateHook<58>);
BANJO_RENUT_HOOK(sub_82993240, GetRenderStateHook<59>);
BANJO_RENUT_HOOK(sub_82992E30, GetRenderStateHook<60>);
BANJO_RENUT_HOOK(sub_82992E40, GetRenderStateHook<61>);
BANJO_RENUT_HOOK(sub_82992E50, GetRenderStateHook<62>);
BANJO_RENUT_HOOK(sub_82992E60, GetRenderStateHook<63>);
BANJO_RENUT_HOOK(sub_82992E70, GetRenderStateHook<64>);
BANJO_RENUT_HOOK(sub_82992E80, GetRenderStateHook<65>);
BANJO_RENUT_HOOK(sub_82992E90, GetRenderStateHook<66>);
BANJO_RENUT_HOOK(sub_82992EA0, GetRenderStateHook<67>);
BANJO_RENUT_HOOK(sub_82992EB0, GetRenderStateHook<68>);
BANJO_RENUT_HOOK(sub_82992EC0, GetRenderStateHook<69>);
BANJO_RENUT_HOOK(sub_82992ED0, GetRenderStateHook<70>);
BANJO_RENUT_HOOK(sub_82992EE0, GetRenderStateHook<71>);
BANJO_RENUT_HOOK(sub_82992EF0, GetRenderStateHook<72>);
BANJO_RENUT_HOOK(sub_82992F00, GetRenderStateHook<73>);
BANJO_RENUT_HOOK(sub_82992F10, GetRenderStateHook<74>);
BANJO_RENUT_HOOK(sub_82992F20, GetRenderStateHook<75>);
BANJO_RENUT_HOOK(sub_82992F70, GetRenderStateHook<76>);
BANJO_RENUT_HOOK(sub_82993010, GetRenderStateHook<77>);
BANJO_RENUT_HOOK(sub_829930A8, GetRenderStateHook<78>);
BANJO_RENUT_HOOK(sub_82993140, GetRenderStateHook<79>);
BANJO_RENUT_HOOK(sub_829931D8, GetRenderStateHook<80>);
BANJO_RENUT_HOOK(sub_829932B0, GetRenderStateHook<81>);
BANJO_RENUT_HOOK(sub_829932E0, GetRenderStateHook<82>);
BANJO_RENUT_HOOK(sub_82993310, GetRenderStateHook<83>);
BANJO_RENUT_HOOK(sub_82993338, GetRenderStateHook<84>);
BANJO_RENUT_HOOK(sub_82993368, GetRenderStateHook<85>);
BANJO_RENUT_HOOK(sub_82993398, GetRenderStateHook<86>);
BANJO_RENUT_HOOK(sub_829933C8, GetRenderStateHook<87>);
BANJO_RENUT_HOOK(sub_829933F0, GetRenderStateHook<88>);
BANJO_RENUT_HOOK(sub_82993420, GetRenderStateHook<89>);
BANJO_RENUT_HOOK(sub_829934E0, GetRenderStateHook<90>);
BANJO_RENUT_HOOK(sub_82993510, GetRenderStateHook<91>);
BANJO_RENUT_HOOK(sub_82993540, GetRenderStateHook<92>);
BANJO_RENUT_HOOK(sub_82993568, GetRenderStateHook<93>);
BANJO_RENUT_HOOK(sub_82993578, GetRenderStateHook<94>);
BANJO_RENUT_HOOK(sub_82993590, GetRenderStateHook<95>);
BANJO_RENUT_HOOK(sub_82993448, GetRenderStateHook<96>);
BANJO_RENUT_HOOK(sub_829934B0, GetRenderStateHook<97>);
BANJO_RENUT_HOOK(sub_82992340, GetRenderStateHook<98>);
BANJO_RENUT_HOOK(sub_82992390, GetRenderStateHook<99>);
BANJO_RENUT_HOOK(sub_829923E8, GetRenderStateHook<100>);

// Sampler state getters (0x831FE618, field 0).
BANJO_RENUT_HOOK(sub_82993D58, GetSamplerStateHook<0>);
BANJO_RENUT_HOOK(sub_82993DA8, GetSamplerStateHook<1>);
BANJO_RENUT_HOOK(sub_82993DF8, GetSamplerStateHook<2>);
BANJO_RENUT_HOOK(sub_82993CF8, GetSamplerStateHook<3>);
BANJO_RENUT_HOOK(sub_82993810, GetSamplerStateHook<4>);
BANJO_RENUT_HOOK(sub_82993668, GetSamplerStateHook<5>);
BANJO_RENUT_HOOK(sub_82993928, GetSamplerStateHook<6>);
BANJO_RENUT_HOOK(sub_82993B68, GetSamplerStateHook<7>);
BANJO_RENUT_HOOK(sub_82993C20, GetSamplerStateHook<8>);
BANJO_RENUT_HOOK(sub_82993A50, GetSamplerStateHook<9>);
BANJO_RENUT_HOOK(sub_829938D8, GetSamplerStateHook<10>);
BANJO_RENUT_HOOK(sub_82993738, GetSamplerStateHook<11>);
BANJO_RENUT_HOOK(sub_829939D8, GetSamplerStateHook<12>);
BANJO_RENUT_HOOK(sub_82993CA0, GetSamplerStateHook<13>);
BANJO_RENUT_HOOK(sub_82993E50, GetSamplerStateHook<14>);
BANJO_RENUT_HOOK(sub_82993AC0, GetSamplerStateHook<15>);
BANJO_RENUT_HOOK(sub_82993EA8, GetSamplerStateHook<16>);
BANJO_RENUT_HOOK(sub_82993F00, GetSamplerStateHook<17>);
BANJO_RENUT_HOOK(sub_82993F58, GetSamplerStateHook<18>);
BANJO_RENUT_HOOK(sub_82993FB8, GetSamplerStateHook<19>);
