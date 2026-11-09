// switch-game: the guest mixer's routing function, sub_8216D7E0 (EU base), as native code (run 64, devlog section AL).
//
// Why this one: run 63's profile of the mixer thread (id 16, 53,832 samples) charged to guest functions by each
// sample's outermost frame puts 22.5 % of the thread inside sub_8216D7E0 itself (run 51's NEON voice loop, its callee
// sub_8216DBB0, 8.4 %; sub_82185E38 8.0 %; sub_828C6740, the callee behind sub_821737C0's indirect call, 5.9 %;
// sub_82188C28, the callee behind sub_82185E38's, 2.9 %). The mixer produces 54-68 % of real time in play.
//
// What the function does (gta4_recomp.2.cpp, 0x8216D7E0-0x8216DBAC): r3 a mix object, r4 and r5 the source and the
// destination buffer descriptors. Under the audio spin lock at 0x8314749C (taken unless this thread holds it), for
// each route of the object's table (12 bytes: source channel, destination channel, current gain, target gain) it
// builds a gain ramp of 256 samples from the current gain to the target and either adds the source channel times the
// ramp into the destination channel (sub_8216DBB0) or, when the object's byte 26 is set, writes the product (a loop of
// 64 vectors in the function itself); the route's current gain becomes the target. Without a route table it adds or
// copies channel by channel. Every vector of the generated body goes through the stored PPC vector registers and the
// guest stack (the ramp is stored there for sub_8216DBB0 to load back).
//
// This is that body statement for statement, with the vectors in host registers: the same guest loads and stores in
// the same order (a source that overlaps its destination behaves the same), the same scalar arithmetic (float results
// of double operations), the same flush-mode switches around the vector arithmetic (denormals), the same calls of the
// guest's other functions with the same arguments and return addresses. Not written: the ramp vectors and scalars the
// generated body leaves on its own stack frame, which is dead once it returns.
// tests/deko3d/test_run64.py runs it against the generated function itself, extracted from the local gta4_recomp.2.cpp.

#pragma once

#include <bit>
#include <cstdint>
#include <cstring>

#include <rex/ppc/context.h>

#include "nx_audio_mixer.h"

namespace theft4::nx_audio_mixer {

// The guest functions the routing function calls (each takes the context as the generated call does).
struct RouteGuest {
  void (*match_format)(PPCContext&);     // sub_821686F8
  void (*fill)(PPCContext&);             // sub_829A1260 (r3 destination, r4 value, r5 bytes)
  void (*copy)(PPCContext&);             // sub_829A0E58 (r3 destination, r4 source, r5 bytes)
  void (*raise_irql)(PPCContext&);       // KeRaiseIrqlToDpcLevel
  void (*acquire)(PPCContext&);          // KeAcquireSpinLockAtRaisedIrql
  void (*release)(PPCContext&);          // KeReleaseSpinLockFromRaisedIrql
  void (*lower_irql)(PPCContext&);       // KfLowerIrql
};

constexpr uint32_t kRouteLock = 0x8314749Cu;       // lis r11,-31980; addi r23,r11,29852
constexpr uint32_t kRouteConstants = 0x820B0000u;  // lis rN,-32245

template <class Host>
inline void MixRoutes(PPCContext& ctx, Host&& host, const RouteGuest& guest) {
  const auto l8 = [&](uint32_t ea) -> uint32_t { return *host(ea); };
  const auto l32 = [&](uint32_t ea) -> uint32_t {
    uint32_t v;
    std::memcpy(&v, host(ea), 4);
    return __builtin_bswap32(v);
  };
  const auto l64 = [&](uint32_t ea) -> uint64_t {
    uint64_t v;
    std::memcpy(&v, host(ea), 8);
    return __builtin_bswap64(v);
  };
  const auto s8 = [&](uint32_t ea, uint32_t v) { *host(ea) = uint8_t(v); };
  const auto s32 = [&](uint32_t ea, uint32_t v) {
    v = __builtin_bswap32(v);
    std::memcpy(host(ea), &v, 4);
  };
  const auto s64 = [&](uint32_t ea, uint64_t v) {
    v = __builtin_bswap64(v);
    std::memcpy(host(ea), &v, 8);
  };
  // lfs / stfs: a float through a double, as the generated code keeps it.
  const auto lf = [&](uint32_t ea) -> double { return double(std::bit_cast<float>(l32(ea))); };
  const auto sf = [&](uint32_t ea, double v) { s32(ea, std::bit_cast<uint32_t>(float(v))); };
  const auto splat = [](double v) { return simde_vdupq_n_f32(float(v)); };

  const uint32_t self = ctx.r3.u32;      // r24
  const uint32_t source = ctx.r4.u32;    // r27
  const uint32_t target = ctx.r5.u32;    // r29
  const uint64_t target_r3 = target ? uint64_t(ctx.r5.s64 + -8) : 0;  // addi r3,r29,-8 / li r3,0
  // stwu r1,-272(r1)
  const uint32_t frame = ctx.r1.u32 - 272;
  s32(frame, ctx.r1.u32);
  ctx.r1.u32 = frame;
  const uint32_t source_header = source ? source - 8 : 0;
  s64(frame + 112, l64(source_header + 20));
  const uint32_t source_base = l32(source_header + 28);  // r25
  const uint32_t target_header = uint32_t(target_r3);
  const uint64_t target_format = l64(target_header + 20);
  const uint32_t target_base = l32(target_header + 28);  // r26
  const uint32_t convert = l8(self + 26);
  s64(frame + 96, target_format);
  s32(frame + 104, target_base);
  uint32_t target_channels;  // r28
  if (convert == 1) {
    target_channels = l8(self + 25);
    ctx.r3.u64 = target_r3;
    ctx.r5.s64 = 1;
    ctx.r4.s64 = ctx.r1.s64 + 96;
    s8(frame + 97, target_channels);
    ctx.lr = 0x8216D85C;
    guest.match_format(ctx);
    if (ctx.r3.s32 < 0) {
      ctx.r1.s64 = ctx.r1.s64 + 272;
      return;
    }
  } else {
    target_channels = l8(frame + 97);
  }
  ctx.lr = 0x8216D87C;
  guest.raise_irql(ctx);
  const uint32_t irql = ctx.r3.u32;      // r30
  const uint32_t thread = ctx.r13.u32;   // r31
  const uint64_t lock_r3 = uint64_t(int64_t(int32_t(kRouteLock)));  // r23 as the generated code holds it
  uint32_t held = l32(kRouteLock + 4);
  if (held == 0 || l32(kRouteLock + 8) != thread) {
    ctx.r3.u64 = lock_r3;
    ctx.lr = 0x8216D8AC;
    guest.acquire(ctx);
    s32(kRouteLock + 8, thread);
    s8(kRouteLock + 12, irql);
    held = l32(kRouteLock + 4);
  }
  uint32_t count = held + 1;  // r10
  s32(kRouteLock + 4, count);

  if (l8(self + 16) != 0) {
    ctx.fpscr.disableFlushMode();
    const double f28 = lf(kRouteConstants - 31616);
    const double f26 = lf(kRouteConstants - 32404);
    const uint32_t target_count = target_channels & 0xFF;  // r29
    const uint32_t source_count = l8(frame + 113);          // r28
    const double f27 = lf(kRouteConstants - 6592);
    const double f29 = lf(kRouteConstants - 10000);
    const double f30 = lf(kRouteConstants - 7984);
    const double f31 = lf(kRouteConstants - 7996);
    const float lane_values[4] = {float(f31), float(f30), float(f29), float(f28)};  // stfs to 112..124(r1); lvx128 v11
    const simde_float32x4_t lanes = simde_vld1q_f32(lane_values);
    uint32_t index = 0, offset = 0;  // r27, r30
    do {
      const uint32_t route = offset + l32(self + 20);  // r31
      const uint32_t target_channel = l8(route + 1);
      ctx.fpscr.disableFlushMode();
      const double goal = lf(route + 8);  // f13
      const uint32_t source_channel = l8(route + 0);
      const uint32_t dst = (target_channel << 10) + target_base;  // r3
      const uint32_t src = (source_channel << 10) + source_base;  // r10
      if (target_channel < target_count) {
        if (source_channel < source_count) {
          const double current = lf(route + 4);                       // lfs f0,4(r31)
          double f0 = double(float(goal - current));                  // fsubs f0,f13,f0
          const uint32_t overwrite = l8(self + 26);                   // lbz r7,26(r24)
          const simde_float32x4_t gain = splat(current);              // lvlx v0 at 80(r1); vspltw
          f0 = double(float(f0 * f27));                               // fmuls f0,f0,f27
          const simde_float32x4_t step = splat(f0);                   // v13
          f0 = double(float(f0 * f26));                               // fmuls f0,f0,f26
          const simde_float32x4_t step4 = splat(f0);                  // v12
          ctx.fpscr.enableFlushModeUnconditional();
          simde_float32x4_t ramp = simde_vaddq_f32(simde_vmulq_f32(step, lanes), gain);  // vmaddfp v0,v13,v11,v0
          if (overwrite == 0) {
            ctx.fpscr.enableFlushMode();  // sub_8216DBB0's first vector add
            MixVoiceVectors(host, dst, src, ramp, step4);
            ctx.fpscr.disableFlushMode();
            sf(route + 4, goal);
          } else {
            uint32_t from = src, to = dst;  // r10, r9
            for (uint32_t n = 64; n != 0; --n) {
              const simde_float32x4_t now = ramp;                     // vor v13,v0,v0
              ctx.fpscr.enableFlushMode();
              ramp = simde_vaddq_f32(ramp, step4);                    // vaddfp v0,v0,v12
              const simde_float32x4_t in = LoadGuest(host, from);     // lvx128 v11,r0,r8
              from += 16;
              StoreGuest(host, to, simde_vmulq_f32(in, now));         // vmulfp128 v13,v11,v13; stvx128 v13,r0,r9
              to += 16;
            }
            ctx.fpscr.disableFlushModeUnconditional();
            sf(route + 4, goal);
          }
        } else if (l8(self + 26) != 0) {
          ctx.r3.u64 = dst;
          ctx.r5.s64 = 1024;
          ctx.r4.s64 = 0;
          ctx.lr = 0x8216DA2C;
          guest.fill(ctx);
        }
      }
      ++index;
      offset += 12;
    } while (index < l8(self + 16));
    count = l32(kRouteLock + 4);  // loc_8216DB54
  } else if (source != target && (target_channels & 0xFF) != 0) {
    const uint32_t source_count = l8(frame + 113);         // r29
    const uint32_t target_count = target_channels & 0xFF;  // r28
    ctx.fpscr.disableFlushMode();
    const double f28 = lf(kRouteConstants - 31616);
    const double f29 = lf(kRouteConstants - 10000);
    const double f30 = lf(kRouteConstants - 7984);
    const double f31 = lf(kRouteConstants - 7996);
    const float lane_values[4] = {float(f31), float(f30), float(f29), float(f28)};
    const simde_float32x4_t lanes = simde_vld1q_f32(lane_values);
    uint32_t to = target_base, from = source_base;  // r3 (r31), r4
    const uint32_t delta = source_base - target_base;  // r27
    uint32_t index = 0;  // r30
    do {
      const uint32_t overwrite = l8(self + 26);
      if (index < source_count) {
        if (overwrite == 0) {
          ctx.fpscr.disableFlushMode();
          const simde_float32x4_t gain = splat(f30);   // lvlx v0 at 84(r1) holding f30
          const simde_float32x4_t step = splat(f31);   // v12, stored at 96(r1)
          const simde_float32x4_t scale = splat(f31);  // v13 from 80(r1)
          ctx.fpscr.enableFlushModeUnconditional();
          const simde_float32x4_t ramp = simde_vaddq_f32(simde_vmulq_f32(scale, lanes), gain);  // vmaddfp v0,v13,v12,v0
          ctx.fpscr.enableFlushMode();
          MixVoiceVectors(host, to, from, ramp, step);
        } else {
          ctx.r3.u64 = to;
          ctx.r4.u64 = from;
          ctx.r5.s64 = 1024;
          ctx.lr = 0x8216DB20;
          guest.copy(ctx);
        }
      } else {
        if (overwrite == 0) {
          break;  // beq cr6,0x8216db54
        }
        ctx.r3.u64 = to;
        ctx.r5.s64 = 1024;
        ctx.r4.s64 = 0;
        ctx.lr = 0x8216DB38;
        guest.fill(ctx);
      }
      to += 1024;
      ++index;
      from = delta + to;
    } while (index < target_count);
    count = l32(kRouteLock + 4);  // loc_8216DB54 (reached from the loop's end and from its early exit)
  }
  // loc_8216DB58
  if (int32_t(count) != 0 && l32(kRouteLock + 8) == ctx.r13.u32) {
    const uint32_t left = count - 1;
    s32(kRouteLock + 4, left);
    if (left == 0) {
      const uint32_t old_irql = l8(kRouteLock + 12);
      ctx.r3.u64 = lock_r3;
      s8(kRouteLock + 12, 0);
      s32(kRouteLock + 8, 0);
      ctx.lr = 0x8216DB94;
      guest.release(ctx);
      ctx.r3.u64 = old_irql;
      ctx.lr = 0x8216DB9C;
      guest.lower_irql(ctx);
    }
  }
  ctx.r3.s64 = 0;
  ctx.r1.s64 = ctx.r1.s64 + 272;
}

}  // namespace theft4::nx_audio_mixer
