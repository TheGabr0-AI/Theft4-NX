// deko3d recorder: a vertex shader instrumented to report its own values (run 54, 16-deko3d-devlog.md section AB).
//
// Why: run 53's CPU skin check found the skinned draws' data sane (708 bones, all rigid; vertices near their bones) while
// the characters stay in pieces on screen. What is left is the shader's execution on the GPU. deko3d 0.5.0 has no
// transform feedback, but a vertex shader may write a storage buffer: the draw's own GLSL (the package's variant
// text, translated by XenosRecomp and SPIRV-Cross) is given a storage buffer and, at the end of main, writes the final
// value of every variable declared at the top level of main, then gl_Position, for the first `max_vertices` vertices
// (gl_VertexID, which is index - index_min with the recorder's vertex offset). The recorder draws the same draw once
// more with this shader and the rasterizer off (nothing reaches the screen) and saves the values with the exact
// inputs it gave the GPU; tools/deko3d/probe_compare.py runs the same GLSL on the PC on those inputs and names the
// first variable whose value differs.
//
// Each value is stored as one uvec4 of raw bits (floats by floatBitsToUint, ints and bools as uints, missing
// components 0). Variables declared inside a branch are out of scope at the end of main and are not reported.
// No deko3d or SDK headers: tests/deko3d/test_run54.py runs it on the package's GLSL and compiles the result with uam.

#pragma once

#include <cstdint>
#include <regex>
#include <string>
#include <vector>

namespace rex::graphics::gta4_native::dk {

struct ProbeVariable {
  std::string name;
  std::string type;  // float int uint bool vecN ivecN uvecN bvecN
};

constexpr uint32_t kProbeBinding = 0;  // the vertex stage's storage buffer 0
// Run 55: a probed draw has at least kProbeMinVertices vertices (a character's mesh, not a quad), and gl_Position of up
// to kProbePositions of them is kept.
constexpr uint32_t kProbeMinVertices = 200;
constexpr uint32_t kProbePositions = 8192;

// The uvec4 expression that holds the raw bits of `name` of GLSL type `type`.
inline std::string ProbeStore(const std::string& type, const std::string& name) {
  const char last = type.back();
  const int n = (last >= '2' && last <= '4') ? last - '0' : 1;
  std::string bits;
  if (type == "float" || type.rfind("vec", 0) == 0) {
    bits = "floatBitsToUint(" + name + ")";
  } else if (type == "int" || type.rfind("ivec", 0) == 0) {
    bits = (n == 1 ? std::string("uint(") : "uvec" + std::to_string(n) + "(") + name + ")";
  } else if (type == "uint" || type.rfind("uvec", 0) == 0) {
    bits = name;
  } else {  // bool, bvecN
    bits = (n == 1 ? std::string("uint(") : "uvec" + std::to_string(n) + "(") + name + ")";
  }
  switch (n) {
    case 1: return "uvec4(" + bits + ", 0u, 0u, 0u)";
    case 2: return "uvec4(" + bits + ", 0u, 0u)";
    case 3: return "uvec4(" + bits + ", 0u)";
    default: return bits;
  }
}

// Returns the instrumented text, or "" when the shader cannot be instrumented (no main, a return inside main, no
// variable). `vars` gets the variables in the order of their slots; gl_Position is the last one.
// Run 55: with max_positions, gl_Position of the first max_positions vertices also goes after the variables'
// block (element max_vertices * slots + gl_VertexID): the whole mesh as the GPU placed it, for the PC's pictures.
inline std::string InstrumentVertexGlsl(const std::string& glsl, uint32_t max_vertices,
                                        std::vector<ProbeVariable>& vars, uint32_t max_positions = 0) {
  vars.clear();
  const size_t main_at = glsl.find("void main()");
  if (main_at == std::string::npos) {
    return {};
  }
  const size_t open = glsl.find('{', main_at);
  if (open == std::string::npos) {
    return {};
  }
  // The brace that closes main.
  int depth = 0;
  size_t close = std::string::npos;
  for (size_t i = open; i < glsl.size(); ++i) {
    if (glsl[i] == '{') {
      ++depth;
    } else if (glsl[i] == '}' && --depth == 0) {
      close = i;
      break;
    }
  }
  if (close == std::string::npos) {
    return {};
  }
  const std::string body = glsl.substr(open + 1, close - open - 1);
  if (body.find("return;") != std::string::npos || body.find("return ") != std::string::npos) {
    return {};
  }
  // Top-level declarations of main: exactly four spaces of indentation (SPIRV-Cross's layout).
  static const std::regex kDecl(
      R"(^    (?:precise )?(float|int|uint|bool|vec[234]|ivec[234]|uvec[234]|bvec[234]) (_[0-9A-Za-z_]+)(?: = |;))");
  size_t line_start = 0;
  while (line_start < body.size()) {
    size_t line_end = body.find('\n', line_start);
    if (line_end == std::string::npos) {
      line_end = body.size();
    }
    const std::string line = body.substr(line_start, line_end - line_start);
    std::smatch m;
    if (std::regex_search(line, m, kDecl)) {
      vars.push_back({m[2].str(), m[1].str()});
    }
    line_start = line_end + 1;
  }
  if (vars.empty()) {
    return {};
  }
  vars.push_back({"gl_Position", "vec4"});
  const uint32_t slots = uint32_t(vars.size());
  std::string probe = "    if (gl_VertexID >= 0 && gl_VertexID < " + std::to_string(max_vertices) + ")\n    {\n";
  probe += "        uint theft4_probe_base = uint(gl_VertexID) * " + std::to_string(slots) + "u;\n";
  for (uint32_t k = 0; k < slots; ++k) {
    probe += "        theft4_probe.v[theft4_probe_base + " + std::to_string(k) + "u] = " +
             ProbeStore(vars[k].type, vars[k].name) + ";\n";
  }
  probe += "    }\n";
  if (max_positions) {
    probe += "    if (gl_VertexID >= 0 && gl_VertexID < " + std::to_string(max_positions) + ")\n    {\n";
    probe += "        theft4_probe.v[" + std::to_string(max_vertices * slots) +
             "u + uint(gl_VertexID)] = floatBitsToUint(gl_Position);\n    }\n";
  }
  // The buffer's declaration before main.
  const std::string decl = "layout(binding = " + std::to_string(kProbeBinding) +
                           ", std430) buffer Theft4Probe\n{\n    uvec4 v[];\n} theft4_probe;\n\n";
  std::string out = glsl.substr(0, main_at) + decl + glsl.substr(main_at, close - main_at) + probe + glsl.substr(close);
  return out;
}

}  // namespace rex::graphics::gta4_native::dk
