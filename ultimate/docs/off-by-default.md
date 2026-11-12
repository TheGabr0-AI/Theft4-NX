# Off by default: what is there, and why it is off

The base ships switches that are off by default although they help somewhere: a fix that one game needs but that
breaks another game's picture, a faster form that is not exact, a speed-up the console measured as slower or as no
gain, or a trial the emulator stalls with. This page sorts them by why they are off. Every table has a **Speed**
column: what is known about the switch's speed, and **Slower** where a console run measured it slower.

**The rule (the user, 2026-11-11):** a key that breaks the graphics of a specific game is OFF in the base by default.
The game that needs it sets it in its own toml (or CMake / environment, as below), after checking its own picture.

How to turn one on:
- a key: `name = true` in the game's toml (the in-run A/B list switches many of them during a run);
- a CMake option: `-DNAME=ON` on a build folder (an existing folder keeps its cached value);
- an environment variable at post-codegen: `NAME=1 python tools/post_codegen.py`, then build.

## 1. Fixes for one game that break another game's picture

Correctness fixes, not speed-ups: off because they break a game the base also serves.

| Key | What it does | Why it is off | Speed | Who turns it on |
|---|---|---|---|---|
| `blur_renut_wipe_overwritten` (A/B 218, port 76) | vivapinata-nx's EDRAM wipe (a clear through another layout, applied at the wiped target's next resolve) by exact tile spans, and skipped when another view over the same tiles drew or cleared after it. Blur's frame came out black with the old rule. | vivapinata-nx (2026-11-11), against the SDK's emulated GPU: its fade frames at the attract mode's cuts came out 5-8 % white (fog) with it on, 0 % off and on the reference. The case it misses: a clear through another layout at the same EDRAM base that covers every tile of the mask's target. Off = vivapinata-nx's rule (port 77). | A fix, not a speed-up | blur-nx |
| `banjo_renut_replay_inherits` (halo3-nx) | A replayed command buffer draws with the running device plus only what its recording set, as on the console. Off, each recording's whole device is used, and a recording on a device of its own (Halo 3's no-hardware device) has no targets, so its draws are dropped. | vivapinata-nx (2026-11-11): a hard shadow under its title logo with it on (2,378 dark grass pixels at 21 s), 0 off and on the emulated GPU; its mask's normal frames 5-9 % white on, 2.4-5.9 % off (the reference 2.5-5 %). Port 67's set_words fix does not cure it. Off since port 77. | A fix, not a speed-up | halo3-nx, and any game that records on a device of its own |
| `blur_renut_edram_inherit` (A/B 220, port 76) | Before a draw that keeps part of its target (a partial write mask, or blending), the target takes what other views wrote over its EDRAM tiles since (a full-target copy draw; Blur's motion-blurred frames). | vivapinata-nx (2026-11-11), on the console: a band of garbage tiles over the top 960x48 pixels of its title screen (12x3 EDRAM tiles, dark scrambled scene content), at first entry and after every attract fly-through; off, none; the PC (D3D12) shows none either way, so it is the deko3d side (port 76 added the multisampled source's bind there). Off since port 86. | A fix, not a speed-up (it adds a copy draw) | blur-nx |
| `banjo_renut_blend_skip` (banjo_gpu_blend_skip) | Skips blending where it cannot change the pixel. | Off since banjo-nx package 30: it broke the depth of field (with banjo_gpu_bool_spec) and the deko3d renderer's river water in Banjo. | A speed-up (GPU), not measured since | none so far |

## 2. Faster forms that are not exact

Speed-ups that change results, so they are off unless a game has checked them.

| Switch | What it does | Why it is off | Speed |
|---|---|---|---|
| `BANJO_GEN_LR_MOVES_ELIDE`, `BANJO_GEN_NV_SAVES_ELIDE`, `BANJO_GEN_BACK_CHAIN_ELIDE` (CMake) | tools/frame_traffic.py's three stack-frame elisions for all game code: no guest link register moves, no saves of non-volatile registers that are locals, no back-chain stores. | They change the stale data a guest stack frame holds; sa2-nx found a game that reads a frame word it never wrote (characters tinted). | Faster (fewer stores per call) |
| `SASRT_HOT_LR_ELIDE` (CMake, port 74; off since port 84) | The link register's half of those elisions for the 12 bulk machine-copy shards only (sasrt-nx's idea 47). | vivapinata-nx's guard found 8 differences from it: the save elided in one of a copy's forms and not the other (forms in different translation units, only one with the define). Turn it on only after a guard run with it shows 0 differences. | Faster copies (sasrt-nx measured it with its round A batch, not alone) |
| `BANJO_GENERATED_NO_TRAPPING_MATH` (CMake) | -fno-trapping-math on the generated game code. | Not exact (see app/CMakeLists.txt). | Not measured |
| `BANJO_SWITCH_EARLY_RA` (CMake) | Leaves GCC's AArch64 early register allocation pass on. | It miscompiled the XDK's streaming copy in peggle2-nx (large textures came out as garbage on the console only). | GCC's default; no gain measured |
| `renut_format_7e3_rg11b10` (port 75) | Xenos 7e3 targets as 11:11:10 float, half FP16's bytes. | No alpha (the 7e3's two bits read as 1), one mantissa bit less on red and green, two on blue. | Less memory and bandwidth for such targets; sasrt-nx has none to measure |
| `renut_depth_float_no_stencil` (port 75) | Xenos float depth targets (D24FS8) as 32-bit float depth without stencil, half the bytes. | Stencil on those targets then does nothing (Katamari's scene depth is D24FS8 and uses stencil). | Less memory and bandwidth for float depth; not measured |
| `sasrt_texture_long_idle_frames` (port 58; an integer, 0 = off) | Releases host textures no draw bound for N frames. | Known to hang the GPU on the Switch (sasrt-nx, 1800). | A memory saver, not a speed-up |
| `vivapinata_depth_resolve_skip_unread` (port 115; A/B 156) | A multisampled depth resolve into a destination nothing has read yet is not drawn; the first read of it draws the one left out and every later one. | NOT exact in principle: when the target changed between the resolve left out and the first read, that one read sees the newer depth. Viva Pinata samples its copies, so it found nothing to skip there; for a game that really leaves depth resolves unread. (Viva's "[trace] sample resolved" line prints only for GPU-written textures, so it misses depth resolve destinations: not a test for "nobody reads this") | Not measured |
| `BLUR_FPCR_ONE_MODE` (CMake, port 123; blur-nx's CG-13) | All guest float code in the host's flush mode: no FPCR switch between VMX and scalar code, and the any-mode helpers as the plain operations. | NOT exact: a scalar operation that meets or makes a denormal gives zero, where the Xenon's FPU keeps it (its VMX flushes). Its census (`banjo_diag_runtime_census` on the PC, tools/census_sites.py --denormal) shows how often a game meets one; in Blur two thirds were in job-queue code whose results the VMX flushes anyway, with no visible or audible difference in four races. A compile-time switch: retrain the PGO profile with it. | blur-nx's console: +4.6 % fps (21.13 -> 22.10, ABBA), CPU 155 -> 147 ms a frame |

(Port 72's VMX estimates and fused multiply-add are not exact either, but are ON at the user's word: within 1-3 ulp of
the exact forms, no further from the 360 than before; sasrt-nx's race with them and a fresh profile was +2.4 % fps.
`SASRT_VMX_FAST_ESTIMATES=OFF` / `SASRT_VMX_FUSED_MADD=OFF` give the exact forms. sasrt-nx's idea 46, building
write-once globals into the machine copies, is not in the base at all: sasrt-nx found it unsound.)

## 3. Speed-ups the console measured

All exact. Off because they did not pay on the console, not because they break anything. A game whose frames are
different may measure them again.

### 3a. Slower on the console

| Key | What it does | Speed |
|---|---|---|
| `pacman_vblank_display_sync` (port 97) | The display's refresh starts the game's frames (pacman-nx's, from futari-nx): the game waits at its Swap for the refresh, so frames do not queue up behind each other. Needs `banjo_display_sync`. | **Slower in a game under the refresh rate:** blur-nx's 4-core console races (2026-11-12, 240 s each): 17.67 fps with it, 20.83 without (+18 %), input -> present 60.7 -> 60.4 ms (no latency cost); its Game Master waited in the pacing wait for 13-16 % of its samples (frames held to whole refreshes: 3 or 4, 20 or 15 fps), 1.4 % without. It fixed pacman-nx's input lag at its full frame rate: pacman-nx turns it on in its toml (2026-11-12, the user's call) |
| `banjo_renut_shadow_sort` | Depth-only draws of a recorded command buffer replayed sorted by shader. | **Slower:** render thread +1.1 ms, GPU +0.65 ms a frame (banjo-nx's console run of 2026-10-27) |
| `banjo_renut_replay_lookahead` | The next replayed op's lookups prefetched while a draw is drawn. | **Slower:** render thread +2.15 ms a frame (2026-10-27) |
| `banjo_renut_replay_stream_shaders` | The shader memo's sets prefetched two ops ahead instead of one. | **Slower:** +0.7 ms a frame, 4 of 5 periods worse (run c4, 2026-10-27) |
| `banjo_renut_texture_slot_skip` | A draw that changes some texture slots keeps the others without a full FillTextures. | **Slower:** render thread +0.64 ms a frame (night_final, 2026-10-28) |
| `sasrt_gpu_tiled_cache` (A/B 102) | Maxwell's tiled caching (binning), which NVN uses and deko3d leaves off. | **Slower:** +3.96 ms of GPU a frame in vivapinata-nx's garden (19.0 -> 17.7 fps, in-run A/B, run c7, 2026-11-12; its per-item GPU times read faster, but binning moves the work past their timestamps: the frame rate is the measure); sasrt-nx found it no better in its race. The picture was not judged |

### 3b. No gain on the console

| Key | What it does | Speed |
|---|---|---|
| `banjo_renut_replay_record_prefetch` | The replay prefetches the next op's shader and declaration records. | Neutral: -0.21 ms a frame, within the noise (night_gpu5) |
| `banjo_renut_host_packets` | A replayed draw's derived state kept by its device state and reused. | Neutral: -0.54 ms and +0.45 ms in two runs (night_gpu5) |
| `banjo_renut_merge_draws` | Consecutive draws that change nothing merged into one host draw. | No effect: Nuts & Bolts has no such draws (the census) |
| `banjo_renut_inline_line_compare` | An older compare of the pushed shared constants. | No effect since 2026-10-23 (the mask compare replaced it); before that it was **slower**: PushSharedConstants 2.7 -> 4.9 % of the render thread (36q) |
| `switch_thread_priorities`, `switch_thread_cores` (port 109; empty) | A host priority or core mask for a thread by its name's prefix, set when the thread names itself (`"Game Master=0x3A,JQ Worker 0=0x3A"`; masks `NAME=0xC`). A raised thread is not time-sliced with the game's other threads at 0x3B. | No gain in blur-nx: Game Master and JQ0-2 at 0x3A, 23.47 fps against 23.84 (its four cores full, 377-394 % of 400: the frame follows the CPU); a switch for other games' A/Bs (2026-11-12) |
| `sasrt_gpu_handover_with_smaa` (A/B 135) | Keeps the colour hand-over (a whole resolve left in the target's image, not copied) while SMAA is on: any texture still sharing SMAA's target is settled before SMAA writes it. The same pixels. | No change in Ridge Racer 6 (12.70 / 12.70 ms parked, 15.66-15.74 / 15.66-15.78 with the pack, 2026-11-09); sasrt-nx measured the hand-over's loss with SMAA on at +0.8 ms of resolves. With SMAA on by default (port 118), a game that draws many whole resolves may measure it again |
| `sasrt_gpu_lazy_depth_resolve` (A/B 104) | A written compressed depth target decompressed right before something reads it, not each time it leaves the bind point. | No difference: GPU 56.24-56.29 ms on against 56.23-56.29 off (vivapinata-nx's garden, run c7, 2026-11-12) |

## 4. Trials: off until tried on the console, or because the emulator stalls with them

| Key | What it does | Why it is off | Speed |
|---|---|---|---|
| `vivapinata_shader_noearly` (port 95; A/B 151) | Renut's early alpha test skipped (constant 976 = 0, GLSL pass noearly): a quad the early test would discard is discarded by the shader's own alpha test instead. The same pixels. | **Per game:** it pays where the shader after the test is short; with long bodies and many quads killed early it is slower. Each game measures A/B 151 on the console and lists its slower shaders in tools/noearly_keep.txt (2026-11-12). | vivapinata-nx's garden: -1.66 ms a frame with its keep list (39.45 -> 37.78, run c24, A/B 151, stock clocks; its first run, c18, -1.30 ms, had the A/B numbered 91, whose off half also switched sasrt_gpu_precise_depth_resolve off); two of its shaders +0.28 and +0.06 ms without the test (they keep it) |
| `audio_xma_kick_without_lock` (port 120) | An XMA kick (XMAEnableContext) sets the context's flag without its lock, so the game's audio thread does not wait for a decode in progress; the decoder takes the kick with one exchange, a kick racing a clear keeps its bit. The same frames in the same order. | Sent off by blur-nx (its user had it compared both ways); a console trial per game. | blur-nx's console: +5.9 % fps in its race (locked 18.52 / 18.15, lock-free 19.36 / 19.48, the same NRO in ABBA order; Renut's wall-clock 10 s lines), the decoder's mutex unlock 0.6 ms a frame before |
| `banjo_display_sync` | A thread follows the display's vsync; the frame limiter releases frames on it. | In Ryujinx every run with it stalled or hung (banjo-nx runs 65, 67, 68); for a console trial. | A latency change; not measured on the console |
| `banjo_renut_core_layout` | Pins the drawing thread to core 0 and the other game threads to cores 1-2 at the first Swap. | Hung the first Swap in Ryujinx in 4 of 12 runs (re-masking running threads). Port 103 (dishonored-nx's, sa2-nx's) applies the layout from each thread at its own next wait instead, the cause of those hangs: 3 of 3 Ryujinx runs passed the first Swap since; off until a console trial. | Faster in sa2-nx: render thread 12.5 -> 11.5 ms, game thread 17.6 -> 17.3 ms |
| `banjo_renut_deferred_chain` | Begin*Vertices draws join the snapshot delta chain instead of a whole device copy. | The chain is one for every device; a game drawing on two devices from two threads gets a delta on the wrong device (emulator hangs in 4 of 8 runs). | Less copying; not measured since |
| `banjo_deko3d_eager_kicks`, `banjo_deko3d_latency_trim` | Input-latency A/Bs: the frame sent to the GPU in more, earlier parts; the render thread waits for the GPU to trim its queue. | Latency trials, per game. | Lower input latency is the aim; frame time may rise |
| `banjo_deko3d_skin_once`, `banjo_deko3d_light_branch`, `banjo_deko3d_gpr32` | GPU A/Bs of Banjo's fur and lit shaders: skinning once a frame, per-light branches, a 32-register cap. | The same pixels; whether faster is the console's to say (their configs/*_on.toml). Light branch and gpr32 act only on the pixel shaders Banjo's lists name (tools/lightbranch_shaders.txt, tools/gpr32_shaders.txt): in another game they change no shader, so they are no A/B targets there (vivapinata-nx, 2026-11-12: its in-run A/B halves of them ran at 2 fps, every pipeline looked up again as the variant key changed). | Not measured on the console |
| `banjo_gpu_zcull_greater` | Asks the driver for ZCULL in the GREATER direction (reversed depth). | A console trial; the picture should be the same. It acts only on the Mesa/NVK path (setenv NVK_ZCULL for banjo-nx's patched NVK); on deko3d it does nothing, and nothing is needed there: dkCmdBufClearDepthStencil writes SET_ZCULL_DIR_FORMAT itself (method 0x36F), GREATER when the depth clear value is under 0.5 and LESS otherwise, so a reversed-depth game that clears depth to 0 already culls in its direction (vivapinata-nx, from libdeko3d.a's disassembly, 2026-11-12). | Not measured |

## Keeping this page

When a port reports that a key breaks its picture (with a comparison against the reference), its default goes to false
under the rule above, it gets a row in section 1 and in docs/ultimate-fixes.md, and the port that needs it is told to
set it in its own toml. A switch measured slower on the console goes in 3a with its numbers.
