// asura_keys.cpp: stub cvars and functions for subsystems not built for Asura's
// Wrath.

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/types.h>

#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include <rex/platform.h>

#include "profiler/profiler.h"
#include "renut_engine/display_sync.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/d3d/d3d_native.h"

// XAudio natives (not in this port)
REXCVAR_DEFINE_BOOL(banjo_native_reverb, false, "Asura/Not built",
                    "native XAudio reverb (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_reverb_verify, false, "Asura/Not built",
                    "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src, false, "Asura/Not built",
                    "native XAudio SRC (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_native_xaudio_src_verify, false, "Asura/Not built",
                    "Verify mode of the above (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// MSAA switch
REXCVAR_DEFINE_BOOL(
    disable_msaa, false, "Asura/Graphics",
    "Not read in this port: msaa_level sets the samples (4, 2 or 1)");

// Frame cap
REXCVAR_DEFINE_STRING(frame_cap, "Off", "Asura/Performance",
                      "Frame rate cap: Off (the game's own, uncapped as on the "
                      "Xbox 360) or a number of frames a second")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

static std::atomic<int> g_frame_cap_live{-1};
void BlurSetFrameCapLive(int fps) {
  g_frame_cap_live.store(fps < 0 ? 0 : fps, std::memory_order_relaxed);
}
int BlurFrameCapNow() {
  const int live = g_frame_cap_live.load(std::memory_order_relaxed);
  if (live >= 0)
    return live;
  const std::string &v = REXCVAR_GET(frame_cap);
  return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
}

void renutFrameLimit() {
  using clock = std::chrono::steady_clock;
  static const int configured = [] {
    const std::string &v = REXCVAR_GET(frame_cap);
    return v.empty() || v == "Off" ? 0 : std::atoi(v.c_str());
  }();
  const int live = g_frame_cap_live.load(std::memory_order_relaxed);
  const int fps = live >= 0 ? live : configured;
  if (fps <= 0) {
    static const bool pacing = banjo::display_sync::FramePacingOn();
    if (pacing)
      banjo::display_sync::WaitForFrameStart();
    return;
  }
  static const bool drain = banjo::display_sync::QueueDrainOn();
  const bool hold = drain && banjo::display_sync::TakeHold();
  constexpr uint64_t kRefreshNs = 16'666'667ull;
  banjo::display_sync::Start();
  static clock::time_point next = clock::now();
  static uint64_t previous_release = 0;
  const clock::time_point wait_start = clock::now();
  clock::time_point now = wait_start;
  const auto frame = std::chrono::duration_cast<clock::duration>(
      std::chrono::duration<double>(1.0 / fps));
  constexpr auto kTail = std::chrono::microseconds(400);
  const auto wait_until = [&](clock::time_point target) {
    for (;;) {
      now = clock::now();
      if (now >= target)
        break;
      if (target - now > kTail)
        std::this_thread::sleep_for(target - now - kTail);
      else
        std::this_thread::yield();
    }
  };
  bool anchored = false;
  uint64_t release = banjo::display_sync::NextRelease(
      uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch())
                   .count()),
      previous_release,
      uint64_t(
          std::chrono::duration_cast<std::chrono::nanoseconds>(frame).count()),
      &anchored);
  if (release && hold)
    release += kRefreshNs;
  if (release) {
    previous_release = release;
    next = clock::time_point(std::chrono::duration_cast<clock::duration>(
        std::chrono::nanoseconds(release)));
    wait_until(next);
    banjo::display_sync::NoteRelease(release, anchored);
  } else {
    previous_release = 0;
    next += frame;
    if (next <= now)
      next = now;
    else
      wait_until(next);
  }
  banjo::profiler::RecordLimiterWait(
      std::chrono::duration<double, std::milli>(clock::now() - wait_start)
          .count());
}

// PM4 renderer D3D-level fast path stubs
REXCVAR_DEFINE_BOOL(banjo_d3d_api_clear, false, "Asura/Not built",
                    "PM4 renderer: Clear markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_api_resolve, false, "Asura/Not built",
                    "PM4 renderer: Resolve markers (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_indexed, false, "Asura/Not built",
                    "PM4 renderer: native DrawIndexed (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(banjo_d3d_native_draw_vertices, false, "Asura/Not built",
                    "PM4 renderer: native DrawVertices (not in this port)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Game frames presented
std::atomic<uint64_t> g_banjo_game_frames{0};

namespace katamari::stage_diag {
void Start() {}
} // namespace katamari::stage_diag

void RenutSaveConfig() {}

// =============================================================================
// Asura's Wrath Direct3D & Runtime/Audio Hooks for ReNut / Deko3D
// =============================================================================

#if REX_PLATFORM_WIN32 || (REX_PLATFORM_MAC && REX_ARCH_ARM64)
constexpr bool kAsuraPhysOffset = true;
#else
constexpr bool kAsuraPhysOffset = false;
#endif

static inline uint8_t *GuestToHost(uint8_t *base, uint32_t addr) {
  if (!base || !addr)
    return nullptr;
  return base + static_cast<uint64_t>(addr) +
         ((kAsuraPhysOffset && addr >= 0xE0000000u) ? 0x1000ull : 0ull);
}

static inline bool ContiguousGuestRange(uint32_t addr, uint32_t len) {
  if (len == 0)
    return true;
  const uint64_t end = uint64_t(addr) + uint64_t(len);
  if (end > 0x100000000ull)
    return false;
  if (kAsuraPhysOffset && addr < 0xE0000000u && end > 0xE0000000u)
    return false;
  return true;
}

static inline void MarkStoreCardsRange(uint8_t *base, uint32_t addr,
                                       uint32_t len) {
  if (!base || len == 0)
    return;
  const uint32_t first_page = addr >> 12;
  const uint32_t last_page = (addr + len - 1u) >> 12;
  for (uint32_t page = first_page; page <= last_page; ++page) {
    if (page - 0x9FFFFu <= 0x60000u || page - 0x7EFFFu <= 0x1000u) {
      if (base[page])
        base[page] = 0;
    }
    if (page == 0xFFFFFu)
      break;
  }
}

#if REX_PLATFORM_SWITCH
extern "C" bool RexSwitchMoveCurrentThreadToSystemCore(int priority);
extern "C" int RexSwitchSystemCoreMixerPriority(void);
extern "C" void RexSwitchLockCurrentGuestThreadLayout(void);
extern "C" bool RexSwitchSetCustomGuestThreadPlacement(int preferred_core,
                                                       uint32_t core_mask,
                                                       int priority);
#else
inline bool RexSwitchMoveCurrentThreadToSystemCore(int) { return false; }
inline int RexSwitchSystemCoreMixerPriority() { return -1; }
inline void RexSwitchLockCurrentGuestThreadLayout() {}
inline bool RexSwitchSetCustomGuestThreadPlacement(int, uint32_t, int) {
  return false;
}
#endif

// XAudio2 engine processing thread entry: move XThread7E81A820 off game cores
// to Core 3.
REX_EXTERN(__imp__sub_829EC6C0);
REX_HOOK_RAW(sub_829EC6C0) {
  if (RexSwitchMoveCurrentThreadToSystemCore(
          RexSwitchSystemCoreMixerPriority())) {
    RexSwitchLockCurrentGuestThreadLayout();
    REXLOG_INFO("[asura] XAudio2 mixer thread (sub_829EC6C0) moved to system "
                "core at priority 0x{:X}",
                RexSwitchSystemCoreMixerPriority());
  }
  __imp__sub_829EC6C0(ctx, base);
}

// PhysX NxThread::execute worker thread entry: place primary scene worker #0
// (XThread609375C0, ~34-40% CPU peak in combat) at priority 0x2F (above Main
// XThread 0x30 and UE3 RenderThread 0x31, below Deko3D 0x2C) so Main XThread
// never suffers priority inversion blocking in sub_82C58300 while PhysX waits
// behind UE3Render on Core 1/2. Secondary workers stay at 0x30 on mask 0x6.
REX_EXTERN(__imp__sub_82C57D28);
REX_HOOK_RAW(sub_82C57D28) {
  static std::atomic<uint32_t> s_physx_worker_idx{0};
  const uint32_t idx =
      s_physx_worker_idx.fetch_add(1, std::memory_order_relaxed);
  const int pref_core = (idx & 1u) ? 2 : 1;
  const int prio = ((idx & 1u) == 0) ? 0x2F : 0x30;
  if (RexSwitchSetCustomGuestThreadPlacement(pref_core, 0x6u, prio)) {
    REXLOG_INFO("[asura] PhysX worker #{} (sub_82C57D28) placed on core {} "
                "(mask 0x6) at prio 0x{:X}",
                idx, pref_core, prio);
  }
  __imp__sub_82C57D28(ctx, base);
}

REXCVAR_DEFINE_DOUBLE(
    asura_physx_max_dt, 0.034, "Asura/Performance",
    "Clamp PhysX fixed-timestep elapsedTime (seconds) in sub_82C256B0 to "
    "prevent substep spiral of death during frame spikes (0 = disabled).");

// PhysX NxScene::computeSubSteps(r5=maxIter, r6=method, f1=elapsedTime, f2=maxTimestep, ...)
// At 30 FPS (dt <= 33.3 ms), f1 <= 0.034s so this hook is a 100% no-op.
// During heavy combat spikes (dt = 50-97 ms), clamping f1 to 0.034s caps substeps
// to one 30-FPS frame budget while preserving exact fixed subStepSize (f2).
REX_EXTERN(__imp__sub_82C256B0);
REX_HOOK_RAW(sub_82C256B0) {
  if (ctx.r6.u32 == 0) {  // NX_TIMESTEP_FIXED
    const double max_dt = REXCVAR_GET(asura_physx_max_dt);
    if (max_dt > 0.0 && ctx.f2.f64 > 0.001 && ctx.f1.f64 > max_dt) {
      ctx.f1.f64 = max_dt;
    }
  }
  __imp__sub_82C256B0(ctx, base);
}

// sub_829E1DF0: XAudio2 Voice State-Variable Biquad Filter (types 0..5).
// Bit-identical to recompiled PPC: uses single-precision FMA with flush mode off.
static inline uint32_t AsuraLdBE32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

static inline float AsuraLdBEF32(const uint8_t *p) {
  return std::bit_cast<float>(AsuraLdBE32(p));
}

static inline void AsuraStBEF32(uint8_t *p, float v) {
  uint32_t u = __builtin_bswap32(std::bit_cast<uint32_t>(v));
  std::memcpy(p, &u, 4);
}

static inline bool RangesOverlap(uint32_t a, uint64_t alen, uint32_t b,
                                 uint64_t blen) {
  return uint64_t(a) < uint64_t(b) + blen &&
         uint64_t(b) < uint64_t(a) + alen;
}

REX_EXTERN(__imp__sub_829E1DF0);
REX_HOOK_RAW(sub_829E1DF0) {
  const uint32_t st_va = ctx.r3.u32;
  if (!base || st_va < 0x10000u || !ContiguousGuestRange(st_va, 36)) {
    __imp__sub_829E1DF0(ctx, base);
    return;
  }
  const uint8_t *st = GuestToHost(base, st_va);
  const uint32_t src_va = AsuraLdBE32(st + 0);
  const uint32_t dst_va = AsuraLdBE32(st + 4);
  const uint32_t lp_va = AsuraLdBE32(st + 8);
  const uint32_t bp_va = AsuraLdBE32(st + 12);
  const uint32_t count = AsuraLdBE32(st + 16);
  const uint32_t type = AsuraLdBE32(st + 24);
  const uint32_t f_bits = AsuraLdBE32(st + 28);
  const uint32_t q_bits = AsuraLdBE32(st + 32);

  if (lp_va < 0x10000u || bp_va < 0x10000u ||
      !ContiguousGuestRange(lp_va, 4) || !ContiguousGuestRange(bp_va, 4)) {
    __imp__sub_829E1DF0(ctx, base);
    return;
  }

  const bool run_loop = (count > 0 && type <= 5);
  if (run_loop) {
    if (count > 0x10000000u || src_va < 0x10000u || dst_va < 0x10000u ||
        !ContiguousGuestRange(src_va, uint64_t(count) * 4ull) ||
        !ContiguousGuestRange(dst_va, uint64_t(count) * 4ull) ||
        RangesOverlap(dst_va, uint64_t(count) * 4ull, st_va, 36) ||
        RangesOverlap(dst_va, uint64_t(count) * 4ull, lp_va, 4) ||
        RangesOverlap(dst_va, uint64_t(count) * 4ull, bp_va, 4) ||
        (src_va != dst_va &&
         RangesOverlap(dst_va, uint64_t(count) * 4ull, src_va,
                       uint64_t(count) * 4ull))) {
      __imp__sub_829E1DF0(ctx, base);
      return;
    }
  }

  ctx.fpscr.disableFlushMode();
  const float f = std::bit_cast<float>(f_bits);
  const float q = std::bit_cast<float>(q_bits);
  const float one_minus_f = 1.0f - f;
  uint8_t *const lp_ptr = GuestToHost(base, lp_va);
  uint8_t *const bp_ptr = GuestToHost(base, bp_va);
  float lp = AsuraLdBEF32(lp_ptr);
  float bp = AsuraLdBEF32(bp_ptr);

  if (run_loop) {
    const uint8_t *const src = GuestToHost(base, src_va);
    uint8_t *const dst = GuestToHost(base, dst_va);
    switch (type) {
      case 0:
        for (uint32_t i = 0; i < count; ++i) {
          const float x = AsuraLdBEF32(src + i * 4);
          const float t = x - lp;
          const float hp = std::fmaf(-q, bp, t);
          bp = std::fmaf(hp, f, bp);
          lp = std::fmaf(bp, f, lp);
          AsuraStBEF32(dst + i * 4, lp);
        }
        break;
      case 1:
        for (uint32_t i = 0; i < count; ++i) {
          const float x = AsuraLdBEF32(src + i * 4);
          const float t = x - lp;
          const float hp = std::fmaf(-q, bp, t);
          bp = std::fmaf(hp, f, bp);
          AsuraStBEF32(dst + i * 4, bp);
          lp = std::fmaf(bp, f, lp);
        }
        break;
      case 2:
        for (uint32_t i = 0; i < count; ++i) {
          const float x = AsuraLdBEF32(src + i * 4);
          const float t = x - lp;
          const float hp = std::fmaf(-q, bp, t);
          bp = std::fmaf(hp, f, bp);
          lp = std::fmaf(bp, f, lp);
          AsuraStBEF32(dst + i * 4, hp);
        }
        break;
      case 3:
        for (uint32_t i = 0; i < count; ++i) {
          const float x = AsuraLdBEF32(src + i * 4);
          const float t = x - lp;
          const float hp = std::fmaf(-q, bp, t);
          bp = std::fmaf(hp, f, bp);
          lp = std::fmaf(bp, f, lp);
          AsuraStBEF32(dst + i * 4, lp + hp);
        }
        break;
      case 4:
        for (uint32_t i = 0; i < count; ++i) {
          const float x = AsuraLdBEF32(src + i * 4);
          lp = std::fmaf(x, f, one_minus_f * lp);
          AsuraStBEF32(dst + i * 4, lp);
        }
        break;
      case 5:
        for (uint32_t i = 0; i < count; ++i) {
          const float x = AsuraLdBEF32(src + i * 4);
          lp = std::fmaf(x, f, one_minus_f * lp);
          AsuraStBEF32(dst + i * 4, x - lp);
        }
        break;
    }
    MarkStoreCardsRange(base, dst_va, count * 4u);
  }

  AsuraStBEF32(lp_ptr, lp);
  AsuraStBEF32(bp_ptr, bp);
  MarkStoreCardsRange(base, lp_va, 4);
  MarkStoreCardsRange(base, bp_va, 4);
}

// Xbox 360 C runtime memcpy -> host memcpy + Renut store cards.
REX_EXTERN(__imp__sub_82B58F80);
REX_HOOK_RAW(sub_82B58F80) {
  const uint32_t dst = ctx.r3.u32;
  const uint32_t src = ctx.r4.u32;
  const uint32_t n = ctx.r5.u32;
  if (n == 0)
    return;
  if (base && dst >= 0x10000u && src >= 0x10000u &&
      ContiguousGuestRange(dst, n) && ContiguousGuestRange(src, n) &&
      (uint64_t(dst) + n <= uint64_t(src) ||
       uint64_t(src) + n <= uint64_t(dst))) {
    std::memcpy(GuestToHost(base, dst), GuestToHost(base, src), n);
    MarkStoreCardsRange(base, dst, n);
    ctx.r3.u64 = dst;
    return;
  }
  __imp__sub_82B58F80(ctx, base);
}

// Xbox 360 C runtime memset -> host memset + Renut store cards.
REX_EXTERN(__imp__sub_82B75420);
REX_HOOK_RAW(sub_82B75420) {
  const uint32_t dst = ctx.r3.u32;
  const int val = static_cast<int>(ctx.r4.u8);
  const uint32_t n = ctx.r5.u32;
  if (n == 0)
    return;
  if (base && dst >= 0x10000u && ContiguousGuestRange(dst, n)) {
    std::memset(GuestToHost(base, dst), val, n);
    MarkStoreCardsRange(base, dst, n);
    ctx.r3.u64 = dst;
    return;
  }
  __imp__sub_82B75420(ctx, base);
}

static inline bool IsInvalidStringPtr(uint32_t addr) {
  return addr < 0x00010000u || addr == 0x0BADF00Du || addr == 0xDEADBEEFu ||
         addr == 0xCCCCCCCCu || addr == 0xCDCDCDCDu || addr == 0xDDDDDDDDu;
}

// sub_82B5A168 is _wcsicmp:
// Protects only against null/low addresses and poison sentinels;
// 0xE0000000..0xFFFFFFFF is valid physical memory.
REX_EXTERN(__imp__sub_82B5A168);
REX_HOOK_RAW(sub_82B5A168) {
  uint32_t s1_addr = ctx.r3.u32;
  uint32_t s2_addr = ctx.r4.u32;
  if (IsInvalidStringPtr(s1_addr) || IsInvalidStringPtr(s2_addr)) {
    if (s1_addr == s2_addr) {
      ctx.r3.u64 = 0;
    } else {
      ctx.r3.u64 = (s1_addr < s2_addr)
                       ? static_cast<uint64_t>(static_cast<int64_t>(-1))
                       : 1;
    }
    return;
  }
  __imp__sub_82B5A168(ctx, base);
}

// Log UE3 fatal error messages (FOutputDeviceXenonError::Serialize) before
// guest error handling.
REX_EXTERN(__imp__sub_8229BB28);
REX_HOOK_RAW(sub_8229BB28) {
  uint32_t wstr_addr = ctx.r4.u32;
  if (!IsInvalidStringPtr(wstr_addr)) {
    const uint8_t *p = GuestToHost(base, wstr_addr);
    if (p) {
      std::string msg;
      msg.reserve(256);
      for (size_t i = 0; i < 1024; ++i) {
        uint16_t ch = (uint16_t(p[i * 2]) << 8) | uint16_t(p[i * 2 + 1]);
        if (ch == 0)
          break;
        msg.push_back((ch >= 0x20 && ch < 0x7F)
                          ? static_cast<char>(ch)
                          : (ch == '\n' || ch == '\r' || ch == '\t'
                                 ? static_cast<char>(ch)
                                 : '?'));
      }
      REXLOG_ERROR("[UE3 FATAL] {}", msg);
    }
  }
  __imp__sub_8229BB28(ctx, base);
}

REXCVAR_DEFINE_BOOL(asura_char_shadows, true, "Asura/Graphics",
                    "Enable or disable dynamic character shadows in the "
                    "renderer. Applies live.");
REXCVAR_DEFINE_BOOL(asura_env_shadows, true, "Asura/Graphics",
                    "Enable or disable static environment shadows in the "
                    "renderer. Applies live.");
REXCVAR_DEFINE_BOOL(
    asura_sync_view_skip_overwrite, true, "Asura/Graphics",
    "Skip redundant SyncColorView before opaque fullscreen overwrite draws.");
