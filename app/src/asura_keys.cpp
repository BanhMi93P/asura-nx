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
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

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
// (e.g. XThread61C52BE0, ~31-42% CPU peak in heavy combat) on Core 2 (mask 0x6)
// at priority 0x2E (strictly above UE3 RenderThread 0x2F and Main XThread 0x30,
// below Deko3D 0x2C and Audio 0x2D) so Main XThread never stalls waiting in
// Event::Wait while PhysX gets preempted behind Deko3D on Core 1 or slices with
// UE3 RenderThread on Core 2. Secondary workers run on Core 1 (mask 0x6) at
// 0x30.
REX_EXTERN(__imp__sub_82C57D28);
REX_HOOK_RAW(sub_82C57D28) {
  static std::atomic<uint32_t> s_physx_worker_idx{0};
  const uint32_t idx =
      s_physx_worker_idx.fetch_add(1, std::memory_order_relaxed);
  const bool primary = ((idx & 1u) == 0);
  const int pref_core = primary ? 2 : 1;
  const int prio = primary ? 0x2E : 0x30;
  if (RexSwitchSetCustomGuestThreadPlacement(pref_core, 0x6u, prio)) {
    REXLOG_INFO("[asura] PhysX worker #{} (sub_82C57D28) placed on core {} "
                "(mask 0x6) at prio 0x{:X}",
                idx, pref_core, prio);
  }
  __imp__sub_82C57D28(ctx, base);
}

REXCVAR_DECLARE(double, renut_gpu_profile);

REXCVAR_DEFINE_DOUBLE(
    asura_physx_max_dt, 0.034, "Asura/Performance",
    "Clamp PhysX fixed-timestep elapsedTime (seconds) in sub_82C256B0 to "
    "prevent substep spiral of death during frame spikes (0 = disabled).");

REXCVAR_DEFINE_INT32(
    asura_physx_max_substeps, 1, "Asura/Performance",
    "Cap PhysX fixed-timestep maxIter in sub_82C256B0 (0 = disabled).")
    .range(0, 16);

REXCVAR_DEFINE_DOUBLE(
    asura_physx_min_timestep, 0.0334, "Asura/Performance",
    "Minimum PhysX fixed-timestep maxTimestep (seconds) in sub_82C256B0 so "
    "1 substep per 30 FPS frame runs at 1.0x realtime speed (0 = disabled).");

// PhysX NxScene::computeSubSteps(r5=maxIter, r6=method, f1=elapsedTime,
// f2=maxTimestep, r7=outSubStepSize*, r8=outNumSubSteps*, r9=accumulator*).
// Clamps elapsedTime and maxIter and raises maxTimestep floor to 30 Hz so
// PhysX runs at most 1 substep per frame at 1.0x speed during combat spikes.
REX_EXTERN(__imp__sub_82C256B0);
REX_HOOK_RAW(sub_82C256B0) {
  const double orig_dt = ctx.f1.f64;
  const double orig_ts = ctx.f2.f64;
  const uint32_t orig_max_iter = ctx.r5.u32;
  const uint32_t method = ctx.r6.u32;
  const uint32_t out_steps_va = ctx.r8.u32;
  bool clamped = false;

  if (method == 0) { // NX_TIMESTEP_FIXED
    const double min_ts = REXCVAR_GET(asura_physx_min_timestep);
    if (min_ts > 0.0 && ctx.f2.f64 > 0.001 && ctx.f2.f64 < min_ts) {
      ctx.f2.f64 = min_ts;
      clamped = true;
    }
    const double max_dt = REXCVAR_GET(asura_physx_max_dt);
    if (max_dt > 0.0 && ctx.f2.f64 > 0.001 && ctx.f1.f64 > max_dt) {
      ctx.f1.f64 = max_dt;
      clamped = true;
    }
    const int32_t max_sub = REXCVAR_GET(asura_physx_max_substeps);
    if (max_sub > 0 && ctx.r5.u32 > static_cast<uint32_t>(max_sub)) {
      ctx.r5.u32 = static_cast<uint32_t>(max_sub);
      clamped = true;
    }
  }
  const double eff_ts = ctx.f2.f64;

  __imp__sub_82C256B0(ctx, base);

  if (REXCVAR_GET(renut_gpu_profile) > 0.0) [[unlikely]] {
    uint32_t out_steps = 0;
    if (base && out_steps_va >= 0x10000u &&
        ContiguousGuestRange(out_steps_va, 4u)) {
      uint32_t raw = 0;
      std::memcpy(&raw, GuestToHost(base, out_steps_va), 4);
      out_steps = __builtin_bswap32(raw);
    }
    static std::atomic<uint64_t> s_calls{0};
    static std::atomic<uint64_t> s_clamped{0};
    static std::atomic<uint64_t> s_steps_sum{0};
    static std::atomic<uint64_t> s_dt_us_sum{0};
    static std::atomic<uint32_t> s_max_steps{0};
    static std::atomic<uint32_t> s_max_dt_us{0};
    static std::atomic<uint64_t> s_last_ns{0};

    s_calls.fetch_add(1, std::memory_order_relaxed);
    if (clamped)
      s_clamped.fetch_add(1, std::memory_order_relaxed);
    s_steps_sum.fetch_add(out_steps, std::memory_order_relaxed);
    const uint32_t dt_us =
        (orig_dt > 0.0 && orig_dt < 10.0)
            ? static_cast<uint32_t>(orig_dt * 1000000.0 + 0.5)
            : 0u;
    s_dt_us_sum.fetch_add(dt_us, std::memory_order_relaxed);
    uint32_t prev = s_max_steps.load(std::memory_order_relaxed);
    while (out_steps > prev &&
           !s_max_steps.compare_exchange_weak(prev, out_steps,
                                              std::memory_order_relaxed)) {
    }
    uint32_t prev_dt = s_max_dt_us.load(std::memory_order_relaxed);
    while (dt_us > prev_dt && !s_max_dt_us.compare_exchange_weak(
                                  prev_dt, dt_us, std::memory_order_relaxed)) {
    }

    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint64_t last = s_last_ns.load(std::memory_order_relaxed);
    if (last == 0) {
      s_last_ns.compare_exchange_strong(last, now_ns,
                                        std::memory_order_relaxed);
    } else if (now_ns - last >= 10000000000ull &&
               s_last_ns.compare_exchange_strong(last, now_ns,
                                                 std::memory_order_relaxed)) {
      const uint64_t calls = s_calls.exchange(0, std::memory_order_relaxed);
      const uint64_t clamp_n = s_clamped.exchange(0, std::memory_order_relaxed);
      const uint64_t steps_sum =
          s_steps_sum.exchange(0, std::memory_order_relaxed);
      const uint64_t dt_sum =
          s_dt_us_sum.exchange(0, std::memory_order_relaxed);
      const uint32_t max_steps =
          s_max_steps.exchange(0, std::memory_order_relaxed);
      const uint32_t max_dt_us =
          s_max_dt_us.exchange(0, std::memory_order_relaxed);
      const double avg_dt_ms =
          calls ? (double(dt_sum) / double(calls)) / 1000.0 : 0.0;
      const double avg_steps = calls ? double(steps_sum) / double(calls) : 0.0;
      REXLOG_INFO("[asura] PhysX computeSubSteps 10s: calls={} clamped={} "
                  "avg_dt={:.2f}ms "
                  "max_dt={:.2f}ms orig_ts={:.4f}s eff_ts={:.4f}s "
                  "orig_maxIter={} method={} avg_steps={:.2f} max_steps={}",
                  calls, clamp_n, avg_dt_ms, double(max_dt_us) / 1000.0,
                  orig_ts, eff_ts, orig_max_iter, method, avg_steps,
                  max_steps);
    }
  }
}

// sub_829E1DF0: XAudio2 Voice State-Variable Biquad Filter (types 0..5).
// Bit-identical to recompiled PPC: uses single-precision FMA with flush mode
// off.
static inline uint16_t AsuraLdBE16(const uint8_t *p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}

static inline void AsuraStBE16(uint8_t *p, uint16_t v) {
  uint16_t u = __builtin_bswap16(v);
  std::memcpy(p, &u, 2);
}

static inline uint32_t AsuraLdBE32(const uint8_t *p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}

static inline void AsuraStBE32(uint8_t *p, uint32_t v) {
  uint32_t u = __builtin_bswap32(v);
  std::memcpy(p, &u, 4);
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
  return uint64_t(a) < uint64_t(b) + blen && uint64_t(b) < uint64_t(a) + alen;
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

  if (lp_va < 0x10000u || bp_va < 0x10000u || !ContiguousGuestRange(lp_va, 4) ||
      !ContiguousGuestRange(bp_va, 4)) {
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
        (src_va != dst_va && RangesOverlap(dst_va, uint64_t(count) * 4ull,
                                           src_va, uint64_t(count) * 4ull))) {
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
REXCVAR_DEFINE_BOOL(asura_shadow_dirty_rect, true, "Asura/Graphics",
                    "Scissor shadow depth clear and float-only resolve to the "
                    "dirty drawn region when cleared to 1.0.");

extern "C" {
extern thread_local bool g_asura_asyncio_spin_pending;
extern thread_local uint32_t g_asura_asyncio_spin_count;
}

// FAsyncIOSystemBase::GetNextRequestIndex: detect busy-spin when
// OutstandingRequests is non-empty but no request can start yet (returns -1),
// then back off in RtlLeaveCriticalSection_raw outside the lock.
REX_EXTERN(__imp__sub_828CB638);
REX_HOOK_RAW(sub_828CB638) {
  const uint32_t self_va = ctx.r3.u32;
  __imp__sub_828CB638(ctx, base);
  if (ctx.r3.s32 == -1 && base && self_va >= 0x10000u &&
      ContiguousGuestRange(self_va + 76u, 4u) &&
      int32_t(AsuraLdBE32(GuestToHost(base, self_va + 76u))) > 0) {
    g_asura_asyncio_spin_pending = true;
    return;
  }
  g_asura_asyncio_spin_pending = false;
  g_asura_asyncio_spin_count = 0;
}

REXCVAR_DEFINE_BOOL(
    asura_native_physx_solver, true, "Asura/Performance",
    "Use native ARM64 NEON implementations of PhysX PGS contact/friction "
    "solver kernels (sub_82D99518, sub_82D9A3F8, sub_82D9AFE8).");

REXCVAR_DEFINE_BOOL(
    asura_native_anim_decode, true, "Asura/Performance",
    "Use native ARM64 NEON implementation of UE3 rotation track "
    "decompression (sub_822AF870).");

#if defined(__aarch64__)
[[gnu::always_inline]] static inline float32x4_t
asura_load_vec4_aligned(const uint8_t *base, uint32_t ea) noexcept {
  return vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(base + (ea & ~0xFu))));
}

[[gnu::always_inline]] static inline void
asura_store_vec4_aligned(uint8_t *base, uint32_t ea, float32x4_t v) noexcept {
  vst1q_u8(base + (ea & ~0xFu), vrev32q_u8(vreinterpretq_u8_f32(v)));
}

[[gnu::always_inline]] static inline float32x4_t
asura_load_vec3_unaligned(const uint8_t *base, uint32_t ea) noexcept {
  const float tmp[4] = {
      AsuraLdBEF32(base + ea),
      AsuraLdBEF32(base + (ea + 4u)),
      AsuraLdBEF32(base + (ea + 8u)),
      0.0f,
  };
  return vld1q_f32(tmp);
}

[[gnu::always_inline]] static inline float asura_vnmsub_s(float a, float b,
                                                          float c) noexcept {
  return -std::fmaf(a, b, -c);
}

[[gnu::always_inline]] static inline float32x4_t
asura_vnmsub_v(float32x4_t a, float32x4_t b, float32x4_t c) noexcept {
  return vnegq_f32(vfmaq_f32(vnegq_f32(c), a, b));
}

[[gnu::always_inline]] static inline float
asura_clamp_fmaxmin(float min_v, float max_v, float x) noexcept {
  const float32x2_t mx = vmax_f32(vdup_n_f32(min_v), vdup_n_f32(x));
  return vget_lane_f32(vmin_f32(vdup_n_f32(max_v), mx), 0);
}
#endif // defined(__aarch64__)

// PhysX PGS linear+angular contact solver (32-bit BE vectors, 128-byte rows).
// Bit-exact native ARM64 NEON replacement for VMX128-via-SIMDe recompiled code.
REX_EXTERN(__imp__sub_82D99518);
REX_HOOK_RAW(sub_82D99518) {
#if defined(__aarch64__)
  if (REXCVAR_GET(asura_native_physx_solver) && base) {
    const uint32_t hdr = ctx.r3.u32;
    const uint32_t count = uint32_t(base[hdr + 1u]) + 1u;
    if (hdr >= 0x10000u && (hdr & 0xFu) == 0u &&
        uint64_t(hdr) + 32ull + uint64_t(count) * 128ull < 0x7EFF0000ull) {
      const uint32_t body0_va = AsuraLdBE32(base + (hdr + 4u));
      const uint32_t body1_va = AsuraLdBE32(base + (hdr + 8u));
      if (body0_va >= 0x10000u && body0_va < 0x7EFF0000u &&
          (body0_va & 0xFu) == 0u && body1_va >= 0x10000u &&
          body1_va < 0x7EFF0000u && (body1_va & 0xFu) == 0u) {
        const uint32_t flags = uint32_t(base[hdr + 2u]);
        const bool has_body0 = (flags & 0x10u) != 0u;
        const bool has_body1 = (flags & 0x20u) != 0u;

        ctx.fpscr.enableFlushMode();

        const float32x4_t v_invM0 =
            vdupq_n_f32(AsuraLdBEF32(base + (body0_va + 44u)));
        const float32x4_t v_invM1 =
            vdupq_n_f32(AsuraLdBEF32(base + (body1_va + 44u)));

        float32x4_t linVel0 = asura_load_vec4_aligned(base, body0_va);
        float32x4_t angVel0 = asura_load_vec4_aligned(base, body0_va + 16u);
        float32x4_t linVel1 = asura_load_vec4_aligned(base, body1_va);
        float32x4_t angVel1 = asura_load_vec4_aligned(base, body1_va + 16u);

        uint32_t row = hdr + 32u;
        for (uint32_t i = 0; i < count; ++i, row += 128u) {
          const float32x4_t v61 = asura_load_vec4_aligned(base, row - 16u);
          const float32x4_t v60 = asura_load_vec4_aligned(base, row);
          const float32x4_t v11 = asura_load_vec4_aligned(base, row + 16u);
          const float32x4_t v59 = asura_load_vec4_aligned(base, row + 32u);
          const float32x4_t v26 = asura_load_vec4_aligned(base, row + 48u);
          const float32x4_t v56 = asura_load_vec4_aligned(base, row + 64u);
          const float32x4_t v0 = asura_load_vec4_aligned(base, row + 80u);

          float32x4_t t = vmulq_f32(v61, linVel0);
          t = vfmaq_f32(t, v11, angVel0);
          t = asura_vnmsub_v(v60, linVel1, t);
          t = asura_vnmsub_v(v59, angVel1, t);

          const float normalVel = (0.0f + vgetq_lane_f32(t, 2)) +
                                  (vgetq_lane_f32(t, 1) + vgetq_lane_f32(t, 0));

          const float applied = vgetq_lane_f32(v0, 0);
          const float maxImp = vgetq_lane_f32(v0, 1);
          const float minImp = vgetq_lane_f32(v0, 2);
          const float targetV = vgetq_lane_f32(v0, 3);

          const float k1 = vgetq_lane_f32(v61, 3);
          const float k57 = vgetq_lane_f32(v60, 3);
          const float k31 = vgetq_lane_f32(v11, 3);
          const float k55 = vgetq_lane_f32(v59, 3);
          const float k25 = vgetq_lane_f32(v26, 3);
          const float w56 = vgetq_lane_f32(v56, 3);

          const float s50 = targetV - normalVel;
          const float s5 = k57 * s50;
          const float s4 = asura_vnmsub_s(k1, k31, s5);
          const float s5b = asura_vnmsub_s(w56, k55, s5);
          const float s7 = asura_vnmsub_s(applied, k55, s4);

          const float new_w56 = std::fmaf(s5b, k25, w56);
          AsuraStBEF32(base + ((row + 76u) & ~3u), new_w56);

          const float cand = std::fmaf(s7, k25, applied);
          const float clamped = asura_clamp_fmaxmin(minImp, maxImp, cand);
          AsuraStBEF32(base + ((row + 80u) & ~3u), clamped);

          const float delta = clamped - applied;
          if (has_body0) {
            const float32x4_t vd = vdupq_n_f32(delta);
            linVel0 = vfmaq_f32(linVel0, v_invM0, vmulq_f32(vd, v61));
            angVel0 = vfmaq_f32(angVel0, v26, vd);
          }
          if (has_body1) {
            const float32x4_t vd = vdupq_n_f32(delta);
            linVel1 = asura_vnmsub_v(vmulq_f32(vd, v60), v_invM1, linVel1);
            angVel1 = asura_vnmsub_v(v56, vd, angVel1);
          }
        }

        const uint32_t b0 = AsuraLdBE32(base + (hdr + 4u));
        const uint32_t b1 = AsuraLdBE32(base + (hdr + 8u));
        asura_store_vec4_aligned(base, b0, linVel0);
        asura_store_vec4_aligned(base, b0 + 16u, angVel0);
        asura_store_vec4_aligned(base, b1, linVel1);
        asura_store_vec4_aligned(base, b1 + 16u, angVel1);
        ctx.r3.u64 = uint32_t(count << 7);
        return;
      }
    }
  }
#endif
  __imp__sub_82D99518(ctx, base);
}

// PhysX PGS angular-only contact/joint solver (128-byte rows, 3x3 inertia).
// Bit-exact native ARM64 NEON replacement for VMX128-via-SIMDe recompiled code.
REX_EXTERN(__imp__sub_82D9A3F8);
REX_HOOK_RAW(sub_82D9A3F8) {
#if defined(__aarch64__)
  if (REXCVAR_GET(asura_native_physx_solver) && base) {
    const uint32_t hdr = ctx.r3.u32;
    const uint32_t count = uint32_t(base[hdr + 1u]) + 1u;
    if (hdr >= 0x10000u && (hdr & 0xFu) == 0u &&
        uint64_t(hdr) + 32ull + uint64_t(count) * 128ull < 0x7EFF0000ull) {
      const uint32_t body0_va = AsuraLdBE32(base + (hdr + 4u));
      const uint32_t body1_va = AsuraLdBE32(base + (hdr + 8u));
      if (body0_va >= 0x10000u && body0_va < 0x7EFF0000u &&
          (body0_va & 3u) == 0u && body1_va >= 0x10000u &&
          body1_va < 0x7EFF0000u && (body1_va & 3u) == 0u) {
        const uint32_t flags = uint32_t(base[hdr + 2u]);
        const bool has_body0 = (flags & 0x10u) != 0u;
        const bool has_body1 = (flags & 0x20u) != 0u;

        ctx.fpscr.enableFlushMode();

        float32x4_t angVel0 = asura_load_vec3_unaligned(base, body0_va + 16u);
        float32x4_t angVel1 = asura_load_vec3_unaligned(base, body1_va + 16u);

        float32x4_t I0_c0 = vdupq_n_f32(0.0f);
        float32x4_t I0_c1 = vdupq_n_f32(0.0f);
        float32x4_t I0_c2 = vdupq_n_f32(0.0f);
        if (has_body0) {
          I0_c0 = asura_load_vec3_unaligned(base, body0_va + 60u);
          I0_c1 = asura_load_vec3_unaligned(base, body0_va + 72u);
          I0_c2 = asura_load_vec3_unaligned(base, body0_va + 84u);
        }

        float32x4_t I1_c0 = vdupq_n_f32(0.0f);
        float32x4_t I1_c1 = vdupq_n_f32(0.0f);
        float32x4_t I1_c2 = vdupq_n_f32(0.0f);
        if (has_body1) {
          I1_c0 = asura_load_vec3_unaligned(base, body1_va + 60u);
          I1_c1 = asura_load_vec3_unaligned(base, body1_va + 72u);
          I1_c2 = asura_load_vec3_unaligned(base, body1_va + 84u);
        }

        uint32_t row = hdr + 32u;
        for (uint32_t i = 0; i < count; ++i, row += 128u) {
          const float32x4_t v47 = asura_load_vec4_aligned(base, row - 16u);
          const float32x4_t v46 = asura_load_vec4_aligned(base, row);

          const float32x4_t p0 = vmulq_f32(v47, angVel0);
          const float32x4_t p1 = vmulq_f32(v46, angVel1);
          const float dot0 = (0.0f + vgetq_lane_f32(p0, 2)) +
                             (vgetq_lane_f32(p0, 1) + vgetq_lane_f32(p0, 0));
          const float dot1 = (0.0f + vgetq_lane_f32(p1, 2)) +
                             (vgetq_lane_f32(p1, 1) + vgetq_lane_f32(p1, 0));

          const float k7 = vgetq_lane_f32(v47, 3);
          const float k43 = vgetq_lane_f32(v46, 3);

          const float s12 = AsuraLdBEF32(base + (row + 44u));
          const float s5 = AsuraLdBEF32(base + (row + 60u));
          const float s6 = AsuraLdBEF32(base + (row + 76u));
          const float32x4_t v0 = asura_load_vec4_aligned(base, row + 80u);

          const float applied = vgetq_lane_f32(v0, 0);
          const float maxImp = vgetq_lane_f32(v0, 1);
          const float minImp = vgetq_lane_f32(v0, 2);
          const float targetV = vgetq_lane_f32(v0, 3);

          const float s39 = dot0 - dot1;
          const float s38 = targetV - s39;
          const float s4 = k43 * s38;
          const float s3 = asura_vnmsub_s(s6, s12, s4);
          const float new_s6 = std::fmaf(s3, s5, s6);
          AsuraStBEF32(base + ((row + 76u) & ~3u), new_s6);

          const float s9 = AsuraLdBEF32(base + (row + 28u));
          const float s8 = asura_vnmsub_s(k7, s9, s4);
          const float s9b = asura_vnmsub_s(applied, s12, s8);
          const float cand = std::fmaf(s9b, s5, applied);
          const float clamped = asura_clamp_fmaxmin(minImp, maxImp, cand);
          AsuraStBEF32(base + ((row + 80u) & ~3u), clamped);

          const float delta = clamped - applied;
          if (has_body0) {
            const float32x4_t d0 = vmulq_f32(v47, vdupq_n_f32(delta));
            float32x4_t acc0 = vmulq_laneq_f32(I0_c0, d0, 0);
            acc0 = vfmaq_laneq_f32(acc0, I0_c1, d0, 1);
            acc0 = vfmaq_laneq_f32(acc0, I0_c2, d0, 2);
            angVel0 = vaddq_f32(angVel0, acc0);
          }
          if (has_body1) {
            const float32x4_t d1 = vmulq_f32(v46, vdupq_n_f32(delta));
            float32x4_t acc1 = vmulq_laneq_f32(I1_c0, d1, 0);
            acc1 = vfmaq_laneq_f32(acc1, I1_c1, d1, 1);
            acc1 = vfmaq_laneq_f32(acc1, I1_c2, d1, 2);
            angVel1 = vsubq_f32(angVel1, acc1);
          }
        }

        const uint32_t b0 = AsuraLdBE32(base + (hdr + 4u)) + 16u;
        const uint32_t b1 = AsuraLdBE32(base + (hdr + 8u)) + 16u;
        AsuraStBEF32(base + ((b0 + 0u) & ~3u), vgetq_lane_f32(angVel0, 0));
        AsuraStBEF32(base + ((b0 + 4u) & ~3u), vgetq_lane_f32(angVel0, 1));
        AsuraStBEF32(base + ((b0 + 8u) & ~3u), vgetq_lane_f32(angVel0, 2));
        AsuraStBEF32(base + ((b1 + 0u) & ~3u), vgetq_lane_f32(angVel1, 0));
        AsuraStBEF32(base + ((b1 + 4u) & ~3u), vgetq_lane_f32(angVel1, 1));
        AsuraStBEF32(base + ((b1 + 8u) & ~3u), vgetq_lane_f32(angVel1, 2));
        ctx.r3.u64 = uint32_t(count << 7);
        return;
      }
    }
  }
#endif
  __imp__sub_82D9A3F8(ctx, base);
}

#if defined(__aarch64__)
[[gnu::always_inline]] static inline float
asura_dot3_f32(float32x4_t a, float32x4_t b) noexcept {
  const float32x4_t prod = vmulq_f32(a, b);
  return (0.0f + vgetq_lane_f32(prod, 2)) +
         (vgetq_lane_f32(prod, 1) + vgetq_lane_f32(prod, 0));
}
#endif

static inline void AsuraRecordHook10s(std::atomic<uint64_t> &fast_cnt,
                                      std::atomic<uint64_t> &fb_cnt,
                                      std::atomic<uint64_t> &last_ns,
                                      bool is_fast, const char *tag) {
  if (is_fast)
    fast_cnt.fetch_add(1, std::memory_order_relaxed);
  else
    fb_cnt.fetch_add(1, std::memory_order_relaxed);
  if (REXCVAR_GET(renut_gpu_profile) > 0.0) [[unlikely]] {
    const uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint64_t last = last_ns.load(std::memory_order_relaxed);
    if (last == 0) {
      last_ns.compare_exchange_strong(last, now_ns, std::memory_order_relaxed);
    } else if (now_ns - last >= 10000000000ull &&
               last_ns.compare_exchange_strong(last, now_ns,
                                               std::memory_order_relaxed)) {
      const uint64_t f = fast_cnt.exchange(0, std::memory_order_relaxed);
      const uint64_t b = fb_cnt.exchange(0, std::memory_order_relaxed);
      REXLOG_INFO("[asura] {} 10s: fast={} fallback={}", tag, f, b);
    }
  }
}

// PhysX PGS contact normal + Coulomb friction solver (96B contact rows, 80B
// friction rows). Native ARM64 NEON replacement for VMX128-via-SIMDe code.
REX_EXTERN(__imp__sub_82D9AFE8);
REX_HOOK_RAW(sub_82D9AFE8) {
  static std::atomic<uint64_t> s_fast{0};
  static std::atomic<uint64_t> s_fb{0};
  static std::atomic<uint64_t> s_last_ns{0};
#if defined(__aarch64__)
  if (REXCVAR_GET(asura_native_physx_solver) && base) {
    const uint32_t hdr = ctx.r3.u32;
    const uint32_t fric_va = ctx.r4.u32;
    if (hdr >= 0x10000u && (hdr & 0xFu) == 0u && fric_va >= 0x10000u &&
        ContiguousGuestRange(hdr, 32u) && ContiguousGuestRange(fric_va, 24u)) {
      const uint8_t *hdr_p = GuestToHost(base, hdr);
      const uint32_t numContacts = uint32_t(hdr_p[2]);
      const uint32_t numFriction = uint32_t(hdr_p[3]);
      const uint64_t totalBytes =
          32ull + uint64_t(numContacts) * 96ull + uint64_t(numFriction) * 80ull;
      if (totalBytes <= 0x1000000ull &&
          ContiguousGuestRange(hdr, static_cast<uint32_t>(totalBytes))) {
        const uint32_t contactEnd = hdr + 32u + numContacts * 96u;
        const uint32_t fricEnd = contactEnd + numFriction * 80u;
        const uint32_t body0_va = AsuraLdBE32(hdr_p + 4u);
        const uint32_t body1_va = AsuraLdBE32(hdr_p + 8u);
        if (body0_va >= 0x10000u && (body0_va & 0xFu) == 0u &&
            body1_va >= 0x10000u && (body1_va & 0xFu) == 0u &&
            ContiguousGuestRange(body0_va, 48u) &&
            ContiguousGuestRange(body1_va, 48u)) {
          uint8_t *fric_p = GuestToHost(base, fric_va);
          const bool hasFricFlag = (fric_p[20] != 0u);
          // Pre-scan friction rows for uninitialized tangent frames before
          // mutating any guest memory.
          bool needs_lazy_init = false;
          if (hasFricFlag && numFriction > 0u) {
            for (uint32_t i = 0; i < numFriction; ++i) {
              const uint32_t q = contactEnd + i * 80u;
              if (AsuraLdBE16(GuestToHost(base, q + 60u)) == 0u) {
                needs_lazy_init = true;
                break;
              }
            }
          }

          if (!needs_lazy_init) {
            const uint8_t flags = hdr_p[1];
            const bool has_body0 = (flags & 0x10u) != 0u;
            const bool has_body1 = (flags & 0x20u) != 0u;

            ctx.fpscr.enableFlushMode();

            uint8_t *body0_p = GuestToHost(base, body0_va);
            uint8_t *body1_p = GuestToHost(base, body1_va);
            const float32x4_t v_invM0 =
                vdupq_n_f32(AsuraLdBEF32(body0_p + 44u));
            const float32x4_t v_invM1 =
                vdupq_n_f32(AsuraLdBEF32(body1_p + 44u));

            float32x4_t linVel0 = asura_load_vec4_aligned(base, body0_va);
            float32x4_t angVel0 =
                asura_load_vec4_aligned(base, body0_va + 16u);
            float32x4_t linVel1 = asura_load_vec4_aligned(base, body1_va);
            float32x4_t angVel1 =
                asura_load_vec4_aligned(base, body1_va + 16u);

            const uint32_t hdr_tag = AsuraLdBE32(hdr_p + 12u);
            float normalSum = 0.0f;
            if (hdr_tag != AsuraLdBE32(fric_p)) {
              AsuraStBE32(fric_p, hdr_tag);
              normalSum = 0.0f;
            } else {
              normalSum = AsuraLdBEF32(fric_p + 4u);
            }

            uint32_t p = hdr + 32u;
            for (uint32_t i = 0; i < numContacts; ++i, p += 96u) {
              uint8_t *row_p = GuestToHost(base, p);
              const float32x4_t n = asura_load_vec4_aligned(base, p);
              const float32x4_t raXn = asura_load_vec4_aligned(base, p + 16u);
              const float32x4_t rbXn = asura_load_vec4_aligned(base, p + 32u);
              const float32x4_t delAng0 =
                  asura_load_vec4_aligned(base, p + 48u);
              const float32x4_t delAng1 =
                  asura_load_vec4_aligned(base, p + 64u);

              const float vn0 =
                  asura_dot3_f32(n, linVel0) + asura_dot3_f32(raXn, angVel0);
              const float vn1 =
                  asura_dot3_f32(n, linVel1) + asura_dot3_f32(rbXn, angVel1);
              const float vn = vn0 - vn1;

              const float targetVel = vgetq_lane_f32(delAng0, 3);
              const float velMul = vgetq_lane_f32(delAng1, 3);
              const float appliedF2 = vgetq_lane_f32(rbXn, 3);
              const float deltaF = (targetVel - vn) * velMul;
              AsuraStBEF32(row_p + 44u, appliedF2 + deltaF);

              const float bias = vgetq_lane_f32(n, 3);
              const float appliedF = vgetq_lane_f32(raXn, 3);
              const float baseDelta = deltaF - bias;
              const float cand = appliedF + baseDelta;
              const bool neg = (0.0f > cand);
              const float clamped = neg ? 0.0f : cand;
              const float actualDelta = neg ? -appliedF : baseDelta;
              normalSum += clamped;
              AsuraStBEF32(row_p + 28u, clamped);

              if (has_body0) {
                const float32x4_t vd = vdupq_n_f32(actualDelta);
                angVel0 = vfmaq_f32(angVel0, delAng0, vd);
                linVel0 = vfmaq_f32(linVel0, n, vmulq_f32(vd, v_invM0));
              }
              if (has_body1) {
                const float32x4_t vd = vdupq_n_f32(actualDelta);
                angVel1 = asura_vnmsub_v(delAng1, vd, angVel1);
                linVel1 = asura_vnmsub_v(n, vmulq_f32(vd, v_invM1), linVel1);
              }
            }

            AsuraStBEF32(fric_p + 4u, normalSum);

            if (hasFricFlag && normalSum != 0.0f) {
              AsuraStBE32(fric_p, 0u);
              const float maxStatic = normalSum * AsuraLdBEF32(hdr_p + 16u);
              const float maxDynamic = normalSum * AsuraLdBEF32(hdr_p + 20u);
              uint32_t q = contactEnd;
              for (uint32_t i = 0; i < numFriction; ++i, q += 80u) {
                uint8_t *frow_p = GuestToHost(base, q);
                const float32x4_t t = asura_load_vec4_aligned(base, q);
                const float32x4_t raXt =
                    asura_load_vec4_aligned(base, q + 16u);
                const float32x4_t rbXt =
                    asura_load_vec4_aligned(base, q + 32u);
                const float32x4_t fDelAng0 =
                    asura_load_vec4_aligned(base, q + 48u);
                const float32x4_t fDelAng1 =
                    asura_load_vec4_aligned(base, q + 64u);

                const float vf = (asura_dot3_f32(t, linVel0) +
                                  asura_dot3_f32(raXt, angVel0)) -
                                 (asura_dot3_f32(t, linVel1) +
                                  asura_dot3_f32(rbXt, angVel1));

                const float targetF = vgetq_lane_f32(t, 3);
                const float appliedF = vgetq_lane_f32(raXt, 3);
                const float velMulF = vgetq_lane_f32(fDelAng1, 3);
                float deltaF = -((targetF + vf) * velMulF);
                float newF = deltaF + appliedF;
                const float absNewF = std::fabs(newF);
                if (absNewF > maxStatic) {
                  AsuraStBE16(frow_p + 62u, 1u);
                  const float rcp = 1.0f / absNewF;
                  newF = newF * (maxDynamic * rcp);
                  deltaF = newF - appliedF;
                }
                AsuraStBEF32(frow_p + 28u, newF);

                if (has_body0) {
                  const float32x4_t vd = vdupq_n_f32(deltaF);
                  angVel0 = vfmaq_f32(angVel0, fDelAng0, vd);
                  linVel0 = vfmaq_f32(linVel0, t, vmulq_f32(vd, v_invM0));
                }
                if (has_body1) {
                  const float32x4_t vd = vdupq_n_f32(deltaF);
                  angVel1 = asura_vnmsub_v(fDelAng1, vd, angVel1);
                  linVel1 = asura_vnmsub_v(t, vmulq_f32(vd, v_invM1), linVel1);
                }
              }
            }

            asura_store_vec4_aligned(base, body0_va, linVel0);
            asura_store_vec4_aligned(base, body0_va + 16u, angVel0);
            asura_store_vec4_aligned(base, body1_va, linVel1);
            asura_store_vec4_aligned(base, body1_va + 16u, angVel1);
            ctx.r3.u64 = fricEnd - hdr;
            AsuraRecordHook10s(s_fast, s_fb, s_last_ns, true,
                               "PhysX sub_82D9AFE8");
            return;
          }
        }
      }
    }
  }
#endif
  AsuraRecordHook10s(s_fast, s_fb, s_last_ns, false, "PhysX sub_82D9AFE8");
  __imp__sub_82D9AFE8(ctx, base);
}

// UE3 Compressed Rotation Track Decompression + NLERP + Normalize.
// Native ARM64 NEON replacement for VMX128-via-SIMDe ACF_Float96NoW (fmt 1)
// and ACF_Fixed48NoW (fmt 2) rotation decoding in sub_822AF870.
REX_EXTERN(__imp__sub_822AF870);
REX_HOOK_RAW(sub_822AF870) {
  static std::atomic<uint64_t> s_fast{0};
  static std::atomic<uint64_t> s_fb{0};
  static std::atomic<uint64_t> s_last_ns{0};
#if defined(__aarch64__)
  if (REXCVAR_GET(asura_native_anim_decode) && base) {
    const uint32_t out_va = ctx.r3.u32;
    const uint32_t seq_va = ctx.r4.u32;
    const int32_t track_off = ctx.r5.s32;
    const float rel_pos = float(ctx.f1.f64);
    const uint32_t bLooping = ctx.r7.u32;

    if (out_va >= 0x10000u && (out_va & 0xFu) == 0u &&
        ContiguousGuestRange(out_va, 16u)) {
      if (track_off == -1) {
        std::memcpy(GuestToHost(base, out_va), GuestToHost(base, 0x83208C20u),
                    16);
        MarkStoreCardsRange(base, out_va, 16u);
        AsuraRecordHook10s(s_fast, s_fb, s_last_ns, true, "Anim sub_822AF870");
        return;
      }

      if (seq_va >= 0x10000u && ContiguousGuestRange(seq_va, 204u)) {
        const uint8_t *seq_p = GuestToHost(base, seq_va);
        const uint32_t stream_va = AsuraLdBE32(seq_p + 196u);
        const uint32_t hdr_va = stream_va + uint32_t(track_off);
        if (hdr_va >= 0x10000u && ContiguousGuestRange(hdr_va, 4u)) {
          const uint32_t hdr = AsuraLdBE32(GuestToHost(base, hdr_va));
          const uint32_t fmt = (hdr >> 28) & 0xFu;
          const uint32_t flags = (hdr >> 24) & 0xFu;
          const uint32_t axis_mask = flags & 0x7u;
          const int32_t num_keys = int32_t(hdr & 0xFFFFFFu);

          if ((fmt == 1u || fmt == 2u) && num_keys >= 1) {
            static constexpr uint8_t kCompCount[8] = {3, 1, 1, 2, 1, 2, 2, 3};
            const uint32_t elem_size = (fmt == 2u) ? 2u : 4u;
            const uint32_t key_stride =
                uint32_t(kCompCount[axis_mask]) * elem_size;
            const uint32_t data_va = hdr_va + 4u;
            const uint64_t data_bytes = uint64_t(num_keys) * key_stride;
            if (data_bytes <= 0x1000000ull &&
                ContiguousGuestRange(
                    data_va, static_cast<uint32_t>(data_bytes) + 8u)) {
              const uint32_t tt_va =
                  (data_va + static_cast<uint32_t>(data_bytes) + 3u) & ~3u;
              const int32_t num_frames = int32_t(AsuraLdBE32(seq_p + 108u));
              const bool explicit_times = (flags & 8u) != 0u;
              bool time_table_ok = true;
              if (num_keys > 1 && explicit_times) {
                if (num_frames < 1 ||
                    !ContiguousGuestRange(
                        tt_va,
                        uint32_t(num_keys) * (num_frames > 255 ? 2u : 1u))) {
                  time_table_ok = false;
                }
              }

              if (time_table_ok) {
                int32_t idx0 = 0;
                int32_t idx1 = 0;
                float alpha = 0.0f;

                if (num_keys > 1) {
                  if (!explicit_times) {
                    const float seq_len = AsuraLdBEF32(seq_p + 104u);
                    uint8_t *cache_p = GuestToHost(base, 0x832BC858u);
                    if (int32_t(AsuraLdBE32(cache_p + 24u)) == num_keys &&
                        AsuraLdBE32(cache_p + 0u) == bLooping &&
                        AsuraLdBEF32(cache_p + 4u) == seq_len &&
                        AsuraLdBEF32(cache_p + 8u) == rel_pos) {
                      idx0 = int32_t(AsuraLdBE32(cache_p + 12u));
                      idx1 = int32_t(AsuraLdBE32(cache_p + 16u));
                      alpha = AsuraLdBEF32(cache_p + 20u);
                    } else {
                      AsuraStBEF32(cache_p + 4u, seq_len);
                      AsuraStBE32(cache_p + 24u, uint32_t(num_keys));
                      AsuraStBEF32(cache_p + 8u, rel_pos);
                      AsuraStBE32(cache_p + 0u, bLooping);

                      if (rel_pos <= 0.0f) {
                        idx0 = 0;
                        idx1 = 0;
                        alpha = 0.0f;
                      } else if (bLooping == 0u) {
                        const int32_t last = num_keys - 1;
                        if (rel_pos >= 1.0f) {
                          idx0 = last;
                          idx1 = last;
                          alpha = 0.0f;
                        } else {
                          const float f = float(last) * rel_pos;
                          const float fl = std::floorf(f);
                          idx0 = std::min(int32_t(fl), last);
                          alpha = f - fl;
                          idx1 = std::min(idx0 + 1, last);
                        }
                      } else {
                        if (rel_pos >= 1.0f) {
                          idx0 = 0;
                          idx1 = 0;
                          alpha = 0.0f;
                        } else {
                          const float f = float(num_frames) * rel_pos;
                          const float fl = std::floorf(f);
                          idx0 = std::min(int32_t(fl), num_frames - 1);
                          alpha = f - fl;
                          idx1 = idx0 + 1;
                          if (idx1 == num_frames) {
                            idx0 = num_keys - 1;
                            idx1 = 0;
                          } else if (num_frames != num_keys) {
                            const int32_t last = num_keys - 1;
                            const float f2 =
                                (f / float(num_frames - 1)) * float(last);
                            const float fl2 = std::floorf(f2);
                            idx0 = std::min(int32_t(fl2), last);
                            alpha = f2 - fl2;
                            idx1 = std::min(idx0 + 1, last);
                          }
                        }
                      }
                      AsuraStBE32(cache_p + 12u, uint32_t(idx0));
                      AsuraStBE32(cache_p + 16u, uint32_t(idx1));
                      AsuraStBEF32(cache_p + 20u, alpha);
                    }
                  } else {
                    const int32_t last = num_keys - 1;
                    const int32_t wrap_idx = bLooping ? 0 : last;
                    const int32_t frame_range =
                        bLooping ? num_frames : (num_frames - 1);
                    if (rel_pos <= 0.0f) {
                      idx0 = 0;
                      idx1 = 0;
                      alpha = 0.0f;
                    } else if (rel_pos >= 1.0f) {
                      idx0 = wrap_idx;
                      idx1 = wrap_idx;
                      alpha = 0.0f;
                    } else {
                      const float frame_f = float(frame_range) * rel_pos;
                      int32_t target = int32_t(frame_f);
                      if (target < 0)
                        target = 0;
                      else if (target > frame_range - 1)
                        target = frame_range - 1;
                      int32_t g = int32_t(float(last) * rel_pos);
                      if (g < 0)
                        g = 0;
                      else if (g > last)
                        g = last;
                      const uint8_t *tt_p = GuestToHost(base, tt_va);
                      auto read_t = [tt_p, num_frames](int32_t k) noexcept {
                        return (num_frames > 255)
                                   ? int32_t(AsuraLdBE16(tt_p + k * 2))
                                   : int32_t(tt_p[k]);
                      };
                      if (read_t(g) <= target) {
                        ++g;
                        while (g <= last && read_t(g) <= target)
                          ++g;
                        idx0 = (g <= last) ? (g - 1) : last;
                      } else {
                        --g;
                        while (g > 0 && read_t(g) > target)
                          --g;
                        idx0 = (g > 0) ? g : 0;
                      }
                      idx1 = (idx0 + 1 <= last) ? (idx0 + 1) : wrap_idx;
                      const int32_t t0 = read_t(idx0);
                      const int32_t t1 = read_t(idx1);
                      const int32_t denom = (t1 - t0 >= 1) ? (t1 - t0) : 1;
                      alpha = (frame_f - float(t0)) / float(denom);
                    }
                  }
                }

                ctx.fpscr.enableFlushMode();

                const uint8_t *v124_p = GuestToHost(base, 0x832A9FA0u);
                const float32x4_t v123 =
                    asura_load_vec4_aligned(base, 0x832A9FB0u);
                const float32x4_t v122 =
                    asura_load_vec4_aligned(base, 0x832A9FC0u);
                const uint32x4_t w_mask = vreinterpretq_u32_f32(
                    asura_load_vec4_aligned(base, 0x832A9F20u));
                const uint8x16_t perm = vandq_u8(
                    vld1q_u8(GuestToHost(base, 0x832AA090u + (axis_mask << 4))),
                    vdupq_n_u8(0x1Fu));
                const uint8x16_t v124_v = vld1q_u8(v124_p);

                auto decode_quat = [&](uint32_t key_va) noexcept -> float32x4_t {
                  float32x4_t xyz;
                  if (fmt == 2u) {
                    uint8_t src_buf[16] = {};
                    std::memcpy(src_buf, GuestToHost(base, key_va), 8);
                    const uint8x16x2_t tbl = {{vld1q_u8(src_buf), v124_v}};
                    const uint32x4_t uvals = vreinterpretq_u32_u8(
                        vrev32q_u8(vqtbl2q_u8(tbl, perm)));
                    const float32x4_t fvals = vcvtq_f32_u32(uvals);
                    xyz = vmulq_f32(vaddq_f32(fvals, v123), v122);
                  } else {
                    xyz = asura_load_vec3_unaligned(base, key_va);
                  }
                  const float32x4_t sq = vmulq_f32(xyz, xyz);
                  const float dot3 =
                      (0.0f + vgetq_lane_f32(sq, 2)) +
                      (vgetq_lane_f32(sq, 1) + vgetq_lane_f32(sq, 0));
                  const float w_sq = 1.0f - dot3;
                  const float w = (w_sq > 0.0f) ? std::sqrt(w_sq) : 0.0f;
                  return vbslq_f32(w_mask, vdupq_n_f32(w), xyz);
                };

                float32x4_t q =
                    decode_quat(data_va + uint32_t(idx0) * key_stride);
                if (idx0 != idx1) {
                  const float32x4_t q1 =
                      decode_quat(data_va + uint32_t(idx1) * key_stride);
                  const float32x4_t prod = vmulq_f32(q, q1);
                  const float d4 =
                      (vgetq_lane_f32(prod, 3) + vgetq_lane_f32(prod, 2)) +
                      (vgetq_lane_f32(prod, 1) + vgetq_lane_f32(prod, 0));
                  const float32x4_t q0_hemi = (d4 >= 0.0f) ? q : vnegq_f32(q);
                  q = vfmaq_f32(vmulq_f32(q1, vdupq_n_f32(alpha)), q0_hemi,
                                vdupq_n_f32(1.0f - alpha));
                }

                const float32x4_t sq4 = vmulq_f32(q, q);
                const float lenSq =
                    (vgetq_lane_f32(sq4, 3) + vgetq_lane_f32(sq4, 2)) +
                    (vgetq_lane_f32(sq4, 1) + vgetq_lane_f32(sq4, 0));
                const float32x4_t eps_v =
                    asura_load_vec4_aligned(base, 0x832A9F40u);
                const uint32x4_t ge_eps = vcgeq_f32(vdupq_n_f32(lenSq), eps_v);
                const float invLen =
                    (lenSq > 0.0f) ? (1.0f / std::sqrt(lenSq)) : 0.0f;
                const float32x4_t norm_q = vmulq_f32(q, vdupq_n_f32(invLen));
                const float32x4_t id_q =
                    asura_load_vec4_aligned(base, 0x832A9F30u);
                const float32x4_t out_q = vbslq_f32(ge_eps, norm_q, id_q);

                asura_store_vec4_aligned(base, out_va, out_q);
                MarkStoreCardsRange(base, out_va, 16u);
                AsuraRecordHook10s(s_fast, s_fb, s_last_ns, true,
                                   "Anim sub_822AF870");
                return;
              }
            }
          }
        }
      }
    }
  }
#endif
  AsuraRecordHook10s(s_fast, s_fb, s_last_ns, false, "Anim sub_822AF870");
  __imp__sub_822AF870(ctx, base);
}
