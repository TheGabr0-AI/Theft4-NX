#version 460
// gpu_bench: the texture in grey, tinted (pipeline variant 1: another shader).
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D tex;
#else
layout(binding = 0) uniform sampler2D tex;
#endif
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 frag_color;
void main() {
  vec4 t = texture(tex, uv);
  float g = dot(t.rgb, vec3(0.299, 0.587, 0.114));
  frag_color = vec4(g, g, g, t.a) * tint;
}
