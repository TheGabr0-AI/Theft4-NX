// GTA IV's Direct3D (Xbox 360 XDK, D3D9 2.0.6274.0: the static library list of the European base's
// default.xex) as ReNut's renderer names it. docs/switch-port/18-renut-migration.md.
//
// ReNut's renderer was written for Banjo-Kazooie: Nuts & Bolts (D3D 2.0.7645, renut-nx app/src/renut_engine/gpu/d3d/
// d3d.h) and retargeted by sa2-nx to D3D 2.0.21076. The library is the same code in every title; the private part of
// the device moves between XDK builds. Every value here was read from the European base's own code (the function it
// was read from is named; tests/renut/test_gta4_d3d.py reads each one again from the generated listing). N&B's value
// is in brackets where ReNut has one. Nothing here is copied game code: offsets and addresses only.
//
// What the comparison says: the public device (dirty masks, ring pointers, call tables, constants, register packets)
// and the private words up to 0x2ABF are N&B's byte for byte; from the vertex declaration to the tiling surfaces the
// fields are N&B's - 8; from the "KickOff in progress" word to the side buffer N&B's - 16; the device's tail
// N&B's - 20. The device is 22,400 bytes (N&B 0x5E00 = 24,064).
#pragma once

#include <cstdint>

namespace theft4::renut::d3d {

// ---------------------------------------------------------------------------------------------------------------
// The device
// ---------------------------------------------------------------------------------------------------------------

// Direct3D_CreateDevice (sub_829D0218): li r3,22400; li r4,128; bl sub_829CFE30 (D3D_MemAllocAligned).
constexpr uint32_t kDeviceAllocSize = 22400;  // [0x5E00]
constexpr uint32_t kDeviceAlignment = 128;

// Public part, N&B's (KickOff sub_829D8518 returns +48; BeginRingBig sub_829D8730 compares +48 with +52; the wait for
// the GPU sub_829D8798 compares +48 with +56; sub_829DF380 stores 1 at +60).
constexpr uint32_t kRing = 0x30;            // m_pRing
constexpr uint32_t kRingLimit = 0x34;       // m_pRingLimit
constexpr uint32_t kRingGuarantee = 0x38;   // m_pRingGuarantee
constexpr uint32_t kReferenceCount = 0x3C;  // m_ReferenceCount
// The register image (the native renderer's hooks replay these blocks: gta4_native_hooks.cpp kReplay*).
constexpr uint32_t kTextureFetch = 0x480;    // m_Constants.TextureFetch[26], 24 bytes each
constexpr uint32_t kVertexFetch = 0x6F0;     // m_Constants.VertexFetch[18], 8 bytes each
constexpr uint32_t kVertexShaderF = 0x780;   // [256][4]
constexpr uint32_t kPixelShaderF = 0x1780;   // [256][4]
constexpr uint32_t kVertexShaderB = 0x2780;  // [4]
constexpr uint32_t kPixelShaderB = 0x2790;   // [4]

// Private words that are where N&B's are.
constexpr uint32_t kGlobalQword = 10880;       // sub_829DF380: ld from 0x82A9E568, std 10880 [10880]
constexpr uint32_t kCreationThreadId = 10888;  // sub_829DF380 [10888]
constexpr uint32_t kIsTitleProcess = 10892;    // sub_829DF380 [10892]
constexpr uint32_t kFenceCompletion = 10896;   // -> the completed-fence word (KickOff, BlockOnFence) [0x2A90]
constexpr uint32_t kFenceScratch = 10900;      // the word after it [10900]; placed as N&B's, not read from the code
constexpr uint32_t kFenceTag = 10908;          // the current fence (BlockOnFence sub_829D7D40) [0x2A9C]
constexpr uint32_t kInsertedFence = 10928;     // D3DDevice_InsertFence sub_829D8618 stores the fence it returns
constexpr uint32_t kFlags = 10940;             // bit 0x80: no GPU hardware (sub_829D0218's type-2 path) [0x2ABC]
constexpr uint32_t kFlags1 = 10941;            // 0x20 command memory ran out (sub_829D74C0), 0x10 swap pending [0x2ABD]
constexpr uint32_t kFlags2 = 10942;            // 0x04 set by the hardware path sub_829DF3F0 [0x2ABE]
constexpr uint8_t kFlagNoHardware = 0x80;
constexpr uint8_t kFlag1OutOfCommandMemory = 0x20;
constexpr uint8_t kFlag1SwapPending = 0x10;    // sub_829D5338 clears it (andi 239)

// Bound objects: N&B's - 8.
constexpr uint32_t kVertexDeclaration = 11812;  // SetVertexDeclaration sub_829D1490: stw r4,11812(r3) [0x2E2C]
constexpr uint32_t kIndexBuffer = 12428;        // SetIndices sub_829CA310 [0x3094]
constexpr uint32_t kRenderTargets = 12432;      // [4], SetRenderTarget sub_829CAAB0 [0x3098]
constexpr uint32_t kDepthStencil = 12448;       // SetDepthStencilSurface sub_829CAE18 [0x30A8]
constexpr uint32_t kStreamSources = 12452;      // [16], SetStreamSource sub_829CA1F0: (stream + 3113) * 4 [0x30AC]
constexpr uint32_t kStreamStrides = 12520;      // u8[16], SetStreamSource sub_829CA1F0: stb r11,12520(r10) [0x30F0]
constexpr uint32_t kTextures = 12536;           // [26], the texture setters: (slot + 3134) * 4 [0x3100]
constexpr uint32_t kPixelShader = 12684;        // SetPixelShader sub_829D1008 [0x3194]
constexpr uint32_t kVertexShader = 12688;       // SetVertexShader sub_829D12C0 [0x3198]
constexpr uint32_t kTilingSurfaces = 12720;     // SetRenderTarget reads 12720..12736 [0x31B8]

// The ring and the command memory.
constexpr uint32_t kInKickOff = 13216;        // KickOff and BlockOnFence return early while it is set [13232]
// Predicated tiling (BeginTiling sub_829CD470, EndTiling sub_829CD908).
constexpr uint32_t kPendingBlocks = 11000;    // command blocks handed to the GPU and not done: sub_829D7C68 adds,
                                              // sub_829D8798 spins until it is 0
constexpr uint32_t kTileCount = 12740;        // BeginTiling stores its Count
constexpr uint32_t kTileRects = 12744;        // [count] D3DRECT
constexpr uint32_t kTileSegmentWords = 13220; // cleared with kInKickOff by EndTiling's tail sub_829DDF28
constexpr uint32_t kSegmentLists = 13232;     // 7 lists of 20 bytes (13232..13352); sub_829D73D0 ends one
constexpr uint32_t kSegmentListCount = 7;
constexpr uint32_t kSegmentListSize = 20;
constexpr uint32_t kCommandBuffer = 13500;    // AllocateInlineData sub_829D76F0, the segment allocator sub_829D8138 [13516]
constexpr uint32_t kSegmentBytes = 14896;     // Swap: the next segment's size (sub_829D8138's argument, / 4)
constexpr uint32_t kMinusOneA = 14920;        // -1 in the type-2 path sub_829DF600 [14936]
constexpr uint32_t kPresentFenceTag = 14924;  // Swap stores the frame's fence; sub_829D8138 waits for it [0x3A5C]
constexpr uint32_t kCriticalSection0 = 14928;  // sub_829DF3F0 [14944]
constexpr uint32_t kCriticalSection1 = 14956;  // sub_829DF3F0 [14972]
constexpr uint32_t kSwapCount = 16544;        // Swap sub_829D5338 counts here [0x40B0]
constexpr uint32_t kFlipCount = 16552;        // Swap waits while kSwapCount - kFlipCount >= 15 (behaviour flag 4)
constexpr uint32_t kSideBuffer = 16712;       // sub_829DF380: 4,800 bytes [16728]
constexpr uint32_t kSideBufferSize = 4800;

// The tail: N&B's - 20.
constexpr uint32_t kMinusOneB = 21540;       // -1 in both creation paths [21560]
constexpr uint32_t kMinusOneC = 21544;       // [21564]
constexpr uint32_t kPerfFrequency = 21568;   // float frequency and its reciprocal, sub_829DF3F0 [21588]
constexpr uint32_t kTitleMinusOne = 21680;   // 41 words of -1, sub_829E1BB8
constexpr uint32_t kTitleMinusOneWords = 41;
constexpr uint32_t kBehaviorFlags = 22280;   // Direct3D_CreateDevice's argument 4 [23980]

constexpr uint32_t kDeviceTypeNoHardware = 2;
// The import slot holding the address of the kernel's VdGlobalDevice (sub_829DF3F0: lis 0x8200, lwz 1896).
constexpr uint32_t kVdGlobalDeviceImportSlot = 0x82000768;  // [0x820007FC]

// A surface after its 24-byte resource header (as N&B's: the native renderer's hooks read +0x24).
constexpr uint32_t kSurfaceSize = 0x24;  // (height - 1) << 3 | (width - 1) << 18

// ---------------------------------------------------------------------------------------------------------------
// The functions ReNut's renderer hooks (European base; sa2-nx's and N&B's are in docs/switch-port/18)
// ---------------------------------------------------------------------------------------------------------------
// X(name, address, what the hook does in step R1)
#define THEFT4_RENUT_D3D_FUNCTIONS(X)                                                                        \
  X(Direct3D_CreateDevice, 829D0218, "a device without GPU hardware, then the title device's set-up")        \
  X(CDevice_KickOff, 829D8518, "the ring sink rewinds; the batch's fence completes")                         \
  X(CDevice_BeginRingBig, 829D8730, "room in the sink")                                                      \
  X(CDevice_NewSegment, 829D8138, "the sink again")                                                          \
  X(CDevice_AfterKickOff, 829D8390, "the sink again (KickOff's tail, and sub_829D67E0's)")                   \
  X(RingBufferDeviceAllocate, 829D74E8, "scratch handed out in a circle")                                    \
  X(CDevice_AllocateInlineData, 829D76F0, "scratch handed out in a circle")                                  \
  X(CDevice_BlockOnFence, 829D7D40, "kick if the fence is the current one; it is complete after that")       \
  X(CDevice_SubmitIndirect, 829D77D8, "nothing: the hardware submission of a command block (ring registers)")   \
  X(CBlocker_Check, 829CFE80, "0: nothing on the host advances a wait for the GPU")                          \
  X(D3DDevice_Swap, 829D5338, "the frame's bookkeeping, as the original's tail")                             \
  X(D3DDevice_DrawVertices, 829CCAC0, "counted, then the original (its packets go into the sink)")           \
  X(D3DDevice_DrawIndexedVertices, 829CCEA8, "counted, then the original")                                   \
  X(D3DDevice_BeginVertices, 829CC610, "counted, then the original")                                         \
  X(D3DDevice_BeginTiling, 829CD470, "counted, then the original (it is BeginTiling, not Clear: 18 s.6)")    \
  X(EndTiling_SubmitTiles, 829DDF28, "EndTiling's tail without the GPU: no tile block, no worker callback")  \
  X(D3DDevice_Resolve, 829CB7C8, "counted, then the original")

// Device work the creation hook calls (never hooked).
constexpr uint32_t kFnPhysicalAlloc = 0x8218BF20;            // (bytes, flags): D3D's allocations use 0xB5800000
constexpr uint32_t kFnInitInternalCommands = 0x829D0B80;     // (device): 8 KB of prebuilt commands, hardware path
constexpr uint32_t kFnInitPresentation = 0x829DEF28;         // (device, D3DPRESENT_PARAMETERS*)
constexpr uint32_t kFnInitializeApiState = 0x829DECC0;       // (device)
constexpr uint32_t kFnResetAllState = 0x829CA608;            // (device, 0): Swap's last call
constexpr uint32_t kPhysicalAllocFlags = 0xB5800000;

}  // namespace theft4::renut::d3d
