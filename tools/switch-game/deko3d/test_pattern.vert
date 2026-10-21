#version 460
// Theft4 deko3d test pattern (docs/switch-port/16-deko3d-devlog.md, M2): six quads from gl_VertexID,
// no vertex buffers. deko3d's clip space has y up (Direct3D's), so y = +1 is the top of the screen.
//   quads 0-3: the corners (top left red, top right green, bottom left blue, bottom right white);
//   quad 4: yellow, depth 0.25, drawn first; quad 5: magenta, depth 0.75, drawn after over part of it.
//   With a LESS depth test the overlap must stay yellow.
layout(location = 0) out vec4 v_color;

void main()
{
    const vec2 corner[6] = vec2[](vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(0.0, 1.0),
                                  vec2(0.0, 1.0), vec2(1.0, 0.0), vec2(1.0, 1.0));
    const vec4 rect[6] = vec4[](vec4(-1.0, 0.5, -0.5, 1.0), vec4(0.5, 0.5, 1.0, 1.0),
                                vec4(-1.0, -1.0, -0.5, -0.5), vec4(0.5, -1.0, 1.0, -0.5),
                                vec4(-0.4, -0.3, 0.2, 0.3), vec4(-0.2, -0.2, 0.4, 0.4));
    const vec4 color[6] = vec4[](vec4(1.0, 0.0, 0.0, 1.0), vec4(0.0, 1.0, 0.0, 1.0),
                                 vec4(0.0, 0.0, 1.0, 1.0), vec4(1.0, 1.0, 1.0, 1.0),
                                 vec4(1.0, 1.0, 0.0, 1.0), vec4(1.0, 0.0, 1.0, 1.0));
    const float depth[6] = float[](0.5, 0.5, 0.5, 0.5, 0.25, 0.75);
    int quad = gl_VertexID / 6;
    vec2 c = corner[gl_VertexID - quad * 6];
    vec4 r = rect[quad];
    gl_Position = vec4(mix(r.x, r.z, c.x), mix(r.y, r.w, c.y), depth[quad], 1.0);
    v_color = color[quad];
}
