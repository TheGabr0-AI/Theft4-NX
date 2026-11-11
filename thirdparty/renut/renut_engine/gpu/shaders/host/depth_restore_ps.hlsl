// theft4 (2026-11-11): a depth-stencil target restored from a 32-bit colour target over the same EDRAM (draw.cpp,
// RestoreDepthFromColorLocked). On the Xenos the two are one memory: GTA IV writes its G-buffer's depth words back as
// colour (an 8_8_8_8 target at the depth buffer's base) and then binds the depth buffer for its lighting, whose stencil
// and depth tests read those words. The host keeps colour and depth in separate targets, so this pass moves them.
//
// The colour texel is the EDRAM word R | G << 8 | B << 16 | A << 24 (the inverse of depth_resolve.hlsli's words);
// as depth-stencil the stencil is its low byte and the depth its upper 24 bits.
//   g_Push.textureIndex : the colour target's slot
//   g_Push.translate    : added to the pixel position to give the colour texel
//   g_Push.param0       : 1 for D24FS8 (20e4 float depth), 0 for D24S8 (24-bit unorm)
//   g_Push.param1       : < 0: the depth pass (writes the depth, stencil cleared by the pipeline); 0-7: the stencil
//                         bit pass for that bit (discards where the bit is clear; the pipeline sets the bit)
#include "utility.hlsli"

struct PSInput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

float DecodeFloat20e4(uint v)
{
    const uint exponent = v >> 20;
    const uint mantissa = v & 0xFFFFF;
    if (exponent == 0)
        return float(mantissa) / 17179869184.0; // 2^34, as depth_resolve.hlsli's encoder
    return asfloat(((exponent + 112) << 23) | (mantissa << 3));
}

float main(PSInput input) : SV_Depth
{
    const int2 texel = int2(input.position.xy) + int2(g_Push.translate);
    const float4 c = g_Textures[NonUniformResourceIndex(g_Push.textureIndex)].Load(int3(texel, 0));
    const uint4 b = uint4(saturate(c) * 255.0 + 0.5);
    const uint word = b.r | (b.g << 8) | (b.b << 16) | (b.a << 24);
    if (g_Push.param1 >= 0.0 && ((word >> uint(g_Push.param1)) & 1) == 0)
        discard;
    const uint depth = word >> 8;
    return g_Push.param0 != 0.0 ? DecodeFloat20e4(depth) : float(depth) / 16777215.0;
}
