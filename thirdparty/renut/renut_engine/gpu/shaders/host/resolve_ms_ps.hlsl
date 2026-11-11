// theft4 (2026-11-11, run 68): a multisampled colour resolve drawn (draw.cpp, DrawResolveCopy with samples > 1): each
// destination pixel is the mean of the source pixel's samples, as D3D12's ResolveSubresourceRegion and Vulkan's
// vkCmdResolveImage give for unorm and float formats. On deko3d the region resolves went through the 2D engine's blit
// of the multisampled image (dkCmdBufBlitImage), which reads the image as a grid of samples (2x: twice as wide) and
// filters a point of it: not a resolve. GTA IV resolves its 2x G-buffer in three tiles, every tile a region.
//   g_Push.textureIndex : the multisampled surface's slot (its own format's view)
//   g_Push.translate    : added to the destination pixel to give the source pixel
//   g_Push.param0       : the sample count (2, 4 or 8)
// present_vs draws the fullscreen triangle; the scissor limits it to the resolved rect.
#define UTILITY_OWN_TEXTURE_TABLE
#include "utility.hlsli"

Texture2DMS<float4> g_Surfaces[] : register(t0, space0);

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const uint index = NonUniformResourceIndex(g_Push.textureIndex);
    const uint count = max(uint(g_Push.param0 + 0.5), 1u);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    for (uint i = 0; i < count; ++i)
        sum += g_Surfaces[index].Load(texel, int(i));
    return sum / float(count);
}
