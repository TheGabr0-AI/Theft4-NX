// theft4 (2026-11-11, run 68): a multisampled colour resolve drawn (draw.cpp, DrawResolveCopy with samples > 1): each
// destination pixel is the mean of the source pixel's samples, as D3D12's ResolveSubresourceRegion and Vulkan's
// vkCmdResolveImage give for unorm and float formats. On deko3d the region resolves went through the 2D engine's blit
// of the multisampled image (dkCmdBufBlitImage), which reads the image as a grid of samples (2x: twice as wide) and
// filters a point of it: not a resolve. GTA IV resolves its 2x G-buffer in three tiles, every tile a region.
//   g_Push.textureIndex : the multisampled surface's slot (its own format's view)
//   g_Push.translate    : added to the destination pixel to give the source pixel
//   g_Push.param0       : the sample count (2, 4 or 8)
// present_vs draws the fullscreen triangle; the scissor limits it to the resolved rect.
//
// theft4 (run 69): on deko3d a multisampled image is read through a "decayed" descriptor (plume_deko3d.cpp,
// dkImageDescriptorInitialize(..., decayMS = true)): a plain 2D texture of its sample grid, samplesX x samplesY texels a
// pixel (2x: 2 x 1, 4x: 2 x 2, 8x: 4 x 2), the layout dkCmdBufResolveImage reads. Run 69 showed every texel fetch of a
// multisampled image through the texture unit's multisample path returning 0 on the console (the G-buffer drawn, its
// resolves empty), while the 2D engine read the same images right.
#define UTILITY_OWN_TEXTURE_TABLE
#include "utility.hlsli"

#ifdef BANJO_DEKO3D
Texture2D<float4> g_Surfaces[] : register(t0, space0);
#else
Texture2DMS<float4> g_Surfaces[] : register(t0, space0);
#endif

float4 main(float4 position : SV_Position) : SV_Target0
{
    const int2 texel = int2(position.xy) + int2(g_Push.translate);
    const uint index = NonUniformResourceIndex(g_Push.textureIndex);
    const uint count = max(uint(g_Push.param0 + 0.5), 1u);
    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
#ifdef BANJO_DEKO3D
    const int2 grid = count >= 8u ? int2(4, 2) : count >= 4u ? int2(2, 2) : count >= 2u ? int2(2, 1) : int2(1, 1);
    for (int y = 0; y < grid.y; ++y)
        for (int x = 0; x < grid.x; ++x)
            sum += g_Surfaces[index].Load(int3(texel * grid + int2(x, y), 0));
    return sum / float(grid.x * grid.y);
#else
    for (uint i = 0; i < count; ++i)
        sum += g_Surfaces[index].Load(texel, int(i));
    return sum / float(count);
#endif
}
