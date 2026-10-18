#version 460
// gpu_bench: a triangle covering the whole target (fill-rate pass).
#ifdef VULKAN
#define VERTEX_ID gl_VertexIndex
#else
#define VERTEX_ID gl_VertexID
#endif
void main() {
  vec2 p = vec2(float((VERTEX_ID << 1) & 2), float(VERTEX_ID & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
