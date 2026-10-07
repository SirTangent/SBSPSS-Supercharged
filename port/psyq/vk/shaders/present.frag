#version 450
// Samples emulated VRAM (R16_UINT, 1024x512) and unpacks the PS1 15-bit
// MBBBBBGGGGGRRRRR layout (R in the low bits).  disp = DISPENV rect in VRAM;
// mask = SetDispMask state (0 -> video blanked); rgb24 = DISPENV isrgb24
// (FMV): the scanned row is then a byte stream of R,G,B triplets packed in
// halfwords, and disp.z counts PIXELS (fmv.cpp pre-converts disp.w*2/3).
layout(binding = 0) uniform usampler2D vram;
layout(push_constant) uniform PC { ivec4 disp; int mask; int rgb24; } pc;

layout(location = 0) in  vec2 uv;
layout(location = 0) out vec4 col;

void main()
{
    if (pc.mask == 0)
    {
        col = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }
    ivec2 p = ivec2(uv * vec2(pc.disp.zw));
    p = clamp(p, ivec2(0), pc.disp.zw - ivec2(1));
    if (pc.rgb24 != 0)
    {
        int vy = (pc.disp.y + p.y) & 511;
        int base = p.x * 3;
        vec3 rgb;
        for (int i = 0; i < 3; i++)
        {
            int b = base + i;
            int hx = (pc.disp.x + (b >> 1)) & 1023;
            uint hw = texelFetch(vram, ivec2(hx, vy), 0).r;
            rgb[i] = float(((b & 1) == 0) ? (hw & 0xFFu) : (hw >> 8u)) / 255.0;
        }
        col = vec4(rgb, 1.0);
        return;
    }
    ivec2 v = (pc.disp.xy + p) & ivec2(1023, 511);
    uint px = texelFetch(vram, v, 0).r;
    // 5 -> 8 bits as (c << 3) | (c >> 2): 31 -> 255, and the same integer
    // GPU_ReadDisplayPixelRGB writes into --dump-frames BMPs, so a dump and
    // the window agree to the bit (it was c / 31 here and c << 3 there)
    uvec3 c = uvec3(px & 31u, (px >> 5u) & 31u, (px >> 10u) & 31u);
    col = vec4(vec3((c << 3u) | (c >> 2u)) / 255.0, 1.0);
}
