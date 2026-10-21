#version 460
// Theft4 deko3d test pattern: the quad's colour (test_pattern.vert).
layout(location = 0) in vec4 v_color;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = v_color;
}
