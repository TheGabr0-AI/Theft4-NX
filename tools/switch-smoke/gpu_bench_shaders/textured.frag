#version 460
// gpu_bench: the texture times the draw's tint (pipeline variant 0).
#ifdef VULKAN
layout(set = 0, binding = 1) uniform sampler2D tex;
#else
layout(binding = 0) uniform sampler2D tex;
#endif
layout(location = 0) in vec2 uv;
layout(location = 1) in vec4 tint;
layout(location = 0) out vec4 frag_color;
void main() {
  frag_color = texture(tex, uv) * tint;
}
