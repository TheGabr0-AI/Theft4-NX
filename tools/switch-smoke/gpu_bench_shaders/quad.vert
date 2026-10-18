#version 460
// gpu_bench (docs/switch-port/13-deko3d-spike.md): one small quad per draw,
// placed and tinted by the draw's 256-byte slice of a uniform buffer.
// The same source for NVK (glslang, VULKAN defined) and deko3d (uam).
#ifdef VULKAN
#define VERTEX_ID gl_VertexIndex
layout(set = 0, binding = 0, std140) uniform DrawBlock {
#else
#define VERTEX_ID gl_VertexID
layout(binding = 0, std140) uniform DrawBlock {
#endif
  vec4 offset_scale;
  vec4 color;
};
layout(location = 0) out vec2 uv;
layout(location = 1) out vec4 tint;
void main() {
  const vec2 corners[6] = vec2[6](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                  vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
  vec2 c = corners[VERTEX_ID % 6];
  uv = c;
  tint = color;
  gl_Position = vec4(offset_scale.xy + c * offset_scale.zw, 0.0, 1.0);
}
