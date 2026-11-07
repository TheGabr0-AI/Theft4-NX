// Host-test stand-in for the SDK's <rex/logging.h>: just enough for dk_shader_package.cpp. Every message goes to
// stdout as "log <level>: <text>" so the test driver can see why a package was refused.
#pragma once

#include <cstdio>
#include <format>
#include <string>
#include <utility>

namespace stub_log {
template <typename... Args>
void Write(const char* level, std::format_string<Args...> fmt, Args&&... args) {
  const std::string text = std::format(fmt, std::forward<Args>(args)...);
  std::printf("log %s: %s\n", level, text.c_str());
}
}  // namespace stub_log

#define REXLOG_ERROR(...) ::stub_log::Write("error", __VA_ARGS__)
#define REXLOG_WARN(...) ::stub_log::Write("warn", __VA_ARGS__)
#define REXLOG_INFO(...) ::stub_log::Write("info", __VA_ARGS__)
