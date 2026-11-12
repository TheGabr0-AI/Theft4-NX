/* Theft4-NX (2026-11-12): FFmpeg's configuration dispatcher for the Ultimate Base SDK (ultimate/sdk/thirdparty/FFmpeg).
 *
 * The base's FFmpeg folder has no config.h, libavutil/avconfig.h or libavutil/ffversion.h: FFmpeg's own .gitignore
 * (config.h, *.version, ...) kept them out of the base's commit, so a fresh clone stops at the first libavutil file.
 * This is the Xenia FFmpeg fork's dispatcher (the SDK's FFmpeg submodule at 0604b46, "Add multi-platform config
 * dispatch") with the two additions the base's other files describe:
 *   - Horizon (devkitA64 defines __SWITCH__ and not __gnu_linux__): config_switch_aarch64.h, which says it is the
 *     Linux/aarch64 header with what Horizon lacks turned off;
 *   - config_masseffect_wmv3.h after each platform's header ("config.h includes it after each platform's header",
 *     its own comment; the base's thirdparty/CMakeLists.txt builds the WMV3 decoder's sources).
 * tools/ultimate/fetch_sdk_thirdparty.ps1 copies this file and the two libavutil headers into the SDK. */
#if defined(__SWITCH__)
#include "config_switch_aarch64.h"
#elif defined(WIN32) || defined(_WIN32)
#if defined(__aarch64__) || defined(_M_ARM64)
#include "config_windows_aarch64.h"
#else
#include "config_windows_x86_64.h"
#endif
#elif defined(__APPLE__) && defined(__MACH__)
#if defined(__aarch64__) || defined(__arm64__)
#include "config_macos_aarch64.h"
#else
#include "config_macos_x86_64.h"
#endif
#elif defined(__gnu_linux__)
#if defined(__aarch64__)
#include "config_linux_aarch64.h"
#else
#include "config_linux_x86_64.h"
#endif
#elif defined(__ANDROID__)
#if defined(__aarch64__)
#include "config_android_aarch64.h"
#elif defined(__amd64__) || defined(__x86_64__)
#include "config_android_x86_64.h"
#else
#error "android arch unsupported"
#endif
#else
#error "no config"
#endif
#include "config_masseffect_wmv3.h"
