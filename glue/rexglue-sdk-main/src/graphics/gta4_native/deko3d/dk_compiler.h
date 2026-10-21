// gta4_native deko3d back end: the uam compile worker (docs/switch-port/16-deko3d-devlog.md, M3 B3). Switch only.
//
// Ported from nfsmw-nx (StevensND and contributors, GPL-3.0), app/src/nfsmw_dk_compilador.{h,cpp} at 266f943.
// uam is not reentrant (Mesa's GLSL front end keeps one static context) and recurses deeply, so every compile in
// the process runs on one thread with an 8 MB stack at priority 0x3B (0x3C is refused for an application), on
// core 2 (run 35: the console's uam took 5-30 s per GTA IV variant there while the game ran, so the renderer
// loads the PC's DKSH and compiles only what the PC could not). The banjo-nx codegen flags are the PC tool's
// (1471), so a DKSH compiled here is the one the PC would make.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace rex::graphics::gta4_native::dk {

class Compiler {
 public:
  struct Result {
    bool ok = false;
    std::vector<uint8_t> dksh;
    std::string log;
    uint64_t ns = 0;
  };
  using Done = std::function<void(Result)>;

  Compiler();
  // Stops it after the current compile (queued work that has not started is dropped) and joins the thread.
  ~Compiler();
  Compiler(const Compiler&) = delete;
  Compiler& operator=(const Compiler&) = delete;

  bool started() const;
  // GLSL to DKSH, waiting for the result (the renderer's own shaders, at start).
  Result Compile(bool vertex, std::string glsl);
  // The same without waiting: `done` runs on the worker thread.
  void Queue(bool vertex, std::string glsl, Done done);
  uint32_t Pending() const;

 private:
  struct Data;
  std::unique_ptr<Data> data_;
};

}  // namespace rex::graphics::gta4_native::dk
