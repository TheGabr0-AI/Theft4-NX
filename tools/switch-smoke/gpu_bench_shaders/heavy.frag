#version 460
// gpu_bench: a deliberately heavy pixel shader (32 rounds of sin/cos/fma per
// pixel) for the 720p fill-rate pass; nothing is read from memory.
layout(location = 0) out vec4 frag_color;
void main() {
  vec4 a = vec4(gl_FragCoord.xy * 0.001, 0.3, 0.7);
  for (int i = 0; i < 32; ++i) {
    a = sin(a * 1.37 + a.yzwx) * 0.9 + cos(a.wxyz * 0.71);
  }
  frag_color = a;
}
