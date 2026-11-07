// Offline probe of run 54 (docs/switch-port/16-deko3d-devlog.md section AB), compiled by tests/deko3d/test_run54.py.
//
//   dk_run54_probe instrument <out dir> <list file>       InstrumentVertexGlsl (deko3d/dk_shader_probe.h) on each file,
//                                                        as the recorder does it on the console (VariantText's define
//                                                        after #version first); writes <out dir>/<name>.vert and a
//                                                        line "INSTR <file> <values> <ok>" per file.
//   dk_run54_probe lock                                  the synchronous texture lock (native_recorder.h's
//                                                        TextureLockWait): completed by another thread, and a wait
//                                                        that times out.

#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dk_shader_probe.h"
#include "native_recorder.h"

using namespace rex::graphics::gta4_native;

int Instrument(int argc, char** argv) {
  const std::string out_dir = argv[2];
  int ok = 0, refused = 0;
  // argv[3]: a text file with one GLSL path per line (636 paths do not fit a Windows command line).
  std::vector<std::string> files;
  {
    std::ifstream list(argv[3]);
    for (std::string line; std::getline(list, line);) {
      if (!line.empty()) files.push_back(line);
    }
  }
  for (const std::string& file : files) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    // VariantText: the define right after the #version line.
    const size_t nl = text.find('\n');
    if (text.rfind("#version", 0) == 0 && nl != std::string::npos) {
      text = text.substr(0, nl + 1) + "#define SPIRV_CROSS_CONSTANT_ID_0 0u\n" + text.substr(nl + 1);
    }
    std::vector<dk::ProbeVariable> vars;
    const std::string out = dk::InstrumentVertexGlsl(text, 64, vars);
    std::string name = file;
    name = name.substr(name.find_last_of("/\\") + 1);
    if (out.empty()) {
      ++refused;
      std::printf("INSTR %s 0 0\n", name.c_str());
      continue;
    }
    ++ok;
    std::ofstream(out_dir + "/" + name + ".vert", std::ios::binary) << out;
    std::printf("INSTR %s %zu 1\n", name.c_str(), vars.size());
  }
  std::printf("INSTRUMENTED %d REFUSED %d\n", ok, refused);
  return 0;
}

int Lock() {
  int errors = 0;
  {
    auto wait = std::make_shared<TextureLockWait>();
    std::thread recorder([wait] {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      TextureLockResult r{};
      r.copied_to_guest = 1;
      wait->Complete(true, r);
    });
    std::unique_lock<std::mutex> lock(wait->mutex);
    const bool done = wait->done_cv.wait_for(lock, std::chrono::seconds(5), [&] { return wait->done; });
    errors += !done || !wait->succeeded || wait->result.copied_to_guest != 1;
    lock.unlock();
    recorder.join();
  }
  {
    auto wait = std::make_shared<TextureLockWait>();
    std::unique_lock<std::mutex> lock(wait->mutex);
    const bool done = wait->done_cv.wait_for(lock, std::chrono::milliseconds(50), [&] { return wait->done; });
    errors += done;  // nobody completes it: the title must give up, not hang
  }
  std::printf("LOCK errors %d\n", errors);
  return errors;
}

int main(int argc, char** argv) {
  if (argc >= 3 && std::string(argv[1]) == "instrument") {
    return Instrument(argc, argv);
  }
  if (argc >= 2 && std::string(argv[1]) == "lock") {
    return Lock();
  }
  std::printf("usage: dk_run54_probe instrument <out> <files...> | lock\n");
  return 2;
}
