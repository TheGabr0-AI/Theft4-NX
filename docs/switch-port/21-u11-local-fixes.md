# 21. U11 local fixes (2026-11-15): shader converter, timing audit, packaging checks

Base: `sync-clean` `524c9a8` (U11). Source-level work only: this session had no devkitA64, no `ultimate/sdk/thirdparty`,
no generated code, no game files and no console logs, so **no NRO was built and nothing here was run on a console**.
The patch of the session before (`/workspace/work/theft4-nx-u11-local.patch`) was not found and is not part of this.

## 1. What changed

| Commit | File | Change |
|---|---|---|
| `bea2a0e` | `ultimate/tools/deko3d_glsl.py` | **Defect fixed.** `convert()` resolved a descriptor index through SPIRV-Cross's `uint _N = expr;` temporaries by their declaration alone; a temporary written again was taken for the SharedConstants word it was declared with and its fetch bound that word's texture (`dk_tex_bK`, `texb`). `immutable_uint_temps()` leaves out temporaries written after their declaration (assignment, compound assignment, `++`/`--`), passed to a function or built-in with an `out` parameter, or declared twice: those fetches stay bindless (`dkh`) |
| `c77f67b` | `tools/ultimate/u8_run_report.py` | **Defects fixed.** Lengths and positions from the log's ms time stamps (printed 0.1 s lengths as the fallback), fps from exact frame counts; intervals selected by their start (`--from-s 60` took in 50-60 s); stalled frames inside a report; **a stall at the end of the log** (time after the last report >= 10 s + `--stall-s`), also from the profile's trailing 0.0 fps windows; coverage of the time asked for |
| `3ae5583` | `tools/ultimate/deko3d_temp_audit.py` | New: lists the game shaders whose bound textures the converter fix changes |
| `1fa18e6` | `tools/ultimate/package_switch.py` | LTO / PGO checks (`option_problems`): no instrumented build without `--allow-pgo-generate`, `BANJO_PGO=use` needs LTO and not the guard forms, `--lto on\|off`; `lto` and `pgo` in `package.json`. No check removed |

The older tree's copy `tools/deko3d/deko3d_glsl.py` has the same defect and is unchanged (not U11's build path;
`patches/README.md` records it as taken without changes).

## 2. Tests

Run here (Linux, Python 3.13, clang 18):

| Test | Result |
|---|---|
| `tests/ultimate/test_deko3d_glsl_temps.py` | 17 passed (16 fail on the converter before the fix) |
| `tests/ultimate/test_u8_run_report.py` | 14 passed |
| `tests/ultimate/test_deko3d_temp_audit.py` | 3 passed |
| `tests/ultimate/test_package_switch_options.py` | 5 passed |
| `ultimate/tools/test_gpu_region.cpp`, `test_gpu_region_bestfit.cpp` (host build) | passed |
| `tests/deko3d/test_hlsl_structure.py`, `test_run58-63.py`, `tests/renut/test_gta4_d3d.py` | passed (58-63 with skips) |
| `tests/deko3d/*` needing Visual Studio's clang (`THEFT4_VS_ROOT`), `test_run51/54-57/64/65` (they chain to those) | **not run / failed for the environment**: no Visual Studio, no generated code, no run logs |
| `tests/renut/test_ultimate_gta4_layout.py`, `tools/ultimate/hook_census.py --strict` | **failed for the environment**: `ultimate/app/generated` is not in the repository |
| Windows-only host tests (`test_address_event.cpp`, `test_address_semaphore.cpp`), simde / AArch64 SDK tests | not run |

## 3. The dark street: status

**Not fixed, not validated.** Facts already measured (section 20): the console's HDR buffer is sunlit, the 1x1
luminance the game reads back equals the PC's, the final picture is darker by about a factor 2 in linear intensity;
every run-time key tried (L0-L5, G1, C2) leaves it as it is.

Confirmed defect that fits those exclusions, **not confirmed as the cause**: the converter defect of section 1. It is
deko3d-only, upstream of every GLSL pass, and no run-time key reaches it. Whether any GTA IV shader has the pattern is
unknown without the private shader texts. To decide it (PC, no console):

    python tools/ultimate/deko3d_temp_audit.py --show

- exit 0: no shader changes, the defect is not the cause, and the existing package and DKSH cache stay valid;
- exit 1: the listed shaders sampled a wrong texture on some path. Make the package and the cache again
  (`ultimate/tools/switch_shaders.ps1`, `tools/ultimate/gta4_dksh_cache.py`), build, and compare the street on the
  console before calling anything fixed.

Hypotheses (not confirmed), in the order worth testing:

1. The exponent chain. The renderer does not apply a target's colour exponent bias on write (GTA IV's HDR targets:
   bias -1, constant 925 = 63), applies a resolve's exponent bias (`blur_renut_resolve_exp_bias`) and the fetch's
   exponent adjust (`sasrt_renut_texture_exp_adjust`); the older tree, with the right light on the console (run 72),
   applied none of the three. A/B from the same save, no build: both keys `false`; then each alone. In the log, look
   for `[renut] resolve of WxH with exponent bias N` (and whether it ends in `not drawable here, copied without it`)
   and the `[renut] exponent adjust` line. Note: that resolve line is written only for the first four biased
   resolves, and says nothing when the biased draw itself fails after its source was found (`DrawCopyFromSlotLocked`
   returning false falls through to an unscaled copy).
2. The GLSL passes that have no constant: the package without passes (`gta4_dksh_cache.py --glsl --package`, prepared
   in U10, not run yet).
3. The post-processing chain stage by stage with `banjo_renut_diag_show_texture` (640x360 `DA1A63E0`, the bloom
   targets, the composite's inputs), PC against console from the same save and the same `input_script`.

## 4. Performance: evidence for 30 fps

**None.** The last console measurement (U10-A / U11 X0, Erista, standard clocks) is ~10 fps on the street with ~5,700
draws a frame, a ~95 ms frame, the three game cores full (336 % of 400) and the audio at 66-86 % of real time. Nothing
in this session changes run-time code, so those numbers stand. The uniform-buffer bind cache already exists in the
base (`plume_deko3d_commands.cpp` `bindGraphicsBuffers`: `graphicsBuffersDirty`, `uboBoundStages`); it was not
written again. No renderer change was made without a compiler to build it and a console to measure it.

## 5. What a complete NRO needs (not available here)

- devkitPro (devkitA64, libnx, deko3d) and the Windows build chain of `tools/ultimate/env.bat`;
- `ultimate/sdk/thirdparty` (`tools/ultimate/fetch_sdk_thirdparty.ps1`);
- the game's `default.xex` (SHA-256 `74d480d2...`) and the generated code (`tools/ultimate/regenerate.ps1` ->
  `ultimate/app/generated`);
- the shader package and DKSH cache (`ultimate/tools/switch_shaders.ps1`, `tools/ultimate/gta4_dksh_cache.py`),
  and `pipeline_list.bin` from the card.

## 6. Logs to bring back from the console

Everything in `sdmc:/switch/theft4-ultimate/` except the NRO: `logs/` (`renut_*.log`, `logs/rex/rex_profile.log`),
`pipeline_list.bin`, `deko3d_shaders.bin`, `capturas/`. Same save, same route (`input_script`), standard clocks, one
key changed a run. Then:

    python tools/ultimate/u8_run_report.py <run folder> --baseline <baseline folder> --series

Read the coverage and the two STALL lines before any fps figure: a run whose log ends in a stall, or whose coverage
is low, is not a measurement of the frame rate.
