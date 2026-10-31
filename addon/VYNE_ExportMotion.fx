// VYNE Export Motion v0.1
// Original shader: estimates directional motion between consecutive native
// Rockstar Editor output frames, then smears only CURRENT-frame samples along
// the estimated velocity. It never blends an old frame into the video.
// One world render per output frame. No temporal supersampling or replay seeks.
//
// Designed to be enabled temporarily by VYNE_BakeFX.addon64 exactly once
// per native Media Foundation video sample, not on the ordinary Present loop.
#include "ReShade.fxh"

uniform float VYNE_Shutter <
    ui_type = "slider";
    ui_label = "VYNE Export | Shutter";
    ui_min = 0.0;
    ui_max = 1.5;
    ui_step = 0.05;
> = 0.80;

uniform float VYNE_SearchRadius <
    ui_type = "slider";
    ui_label = "VYNE Export | Max motion search (pixels)";
    ui_min = 1.0;
    ui_max = 24.0;
    ui_step = 1.0;
> = 10.0;

uniform float VYNE_MatchThreshold <
    ui_type = "slider";
    ui_label = "VYNE Export | Motion confidence threshold";
    ui_min = 0.02;
    ui_max = 0.30;
    ui_step = 0.01;
> = 0.15;

texture2D VYNE_CurrentTexture {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
};
texture2D VYNE_PreviousTexture {
    Width = BUFFER_WIDTH;
    Height = BUFFER_HEIGHT;
    Format = RGBA8;
};

sampler2D VYNE_CurrentSampler {
    Texture = VYNE_CurrentTexture;
    AddressU = CLAMP;
    AddressV = CLAMP;
    MagFilter = LINEAR;
    MinFilter = LINEAR;
};

sampler2D VYNE_PreviousSampler {
    Texture = VYNE_PreviousTexture;
    AddressU = CLAMP;
    AddressV = CLAMP;
    MagFilter = LINEAR;
    MinFilter = LINEAR;
};

float VYNE_Luma(float3 rgb)
{
    return dot(rgb, float3(0.2126, 0.7152, 0.0722));
}

float4 VYNE_CopyScene(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    return tex2D(ReShade::BackBuffer, uv);
}

float4 VYNE_DirectionalBlur(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    float4 scene = tex2D(VYNE_CurrentSampler, uv);
    float2 pixel = ReShade::PixelSize;
    const float3 lumaOff = float3(0.2126, 0.7152, 0.0722);
    float center = dot(scene.rgb, lumaOff);
    float nx = dot(tex2D(VYNE_CurrentSampler, uv + float2(pixel.x * 2.0, 0.0)).rgb, lumaOff);
    float ny = dot(tex2D(VYNE_CurrentSampler, uv + float2(0.0, pixel.y * 2.0)).rgb, lumaOff);

    // Local 3x3 optical-flow search: match CURRENT features in PREVIOUS frame.
    // Keep a small center preference to avoid hallucinating velocity in flat
    // areas. This intentionally yields no motion in featureless regions.
    float lowest = 10.0;
    float2 offsetBest = float2(0.0, 0.0);
    [unroll] for (int y = -1; y <= 1; ++y)
    {
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float2 off = float2((float)x, (float)y) * VYNE_SearchRadius * pixel;
            float previous0 = VYNE_Luma(tex2D(VYNE_PreviousSampler, uv + off).rgb);
            float previousX = VYNE_Luma(tex2D(VYNE_PreviousSampler,
                uv + off + float2(pixel.x * 2.0, 0.0)).rgb);
            float previousY = VYNE_Luma(tex2D(VYNE_PreviousSampler,
                uv + off + float2(0.0, pixel.y * 2.0)).rgb);
            float score = abs(previous0 - center)
                        + abs(previousX - nx) * 0.55
                        + abs(previousY - ny) * 0.55
                        + dot(off / pixel, off / pixel) * 0.00002;
            if (score < lowest)
            {
                lowest = score;
                offsetBest = off;
            }
        }
    }

    // Reject unreliable motion and first-frame uninitialized history.
    if (lowest > VYNE_MatchThreshold || dot(offsetBest, offsetBest) <
        pixel.x * pixel.x * 0.5 || VYNE_Shutter <= 0.001)
        return scene;

    // Keep the main image from CURRENT ONLY. Prior frame is solely a motion
    // estimation input; it is NEVER blended into the export.
    float3 sum = scene.rgb;
    float sumWeight = 1.0;
    [unroll] for (int i = -4; i <= 4; ++i)
    {
        if (i == 0) continue;
        float t = ((float)i / 8.0) * VYNE_Shutter;
        float2 sampleUV = saturate(uv + offsetBest * t);
        float3 color = tex2D(VYNE_CurrentSampler, sampleUV).rgb;
        // Preserve borders between unrelated objects, reduce foreground halos.
        float coherence = saturate(1.0 - length(color - scene.rgb) * 1.8);
        float weight = coherence * 0.75;
        sum += color * weight;
        sumWeight += weight;
    }
    return float4(sum / max(sumWeight, 0.0001), scene.a);
}

float4 VYNE_SaveHistory(float4 position : SV_Position, float2 uv : TEXCOORD) : SV_Target
{
    // Save the unblurred current scene only after motion was estimated.
    return tex2D(VYNE_CurrentSampler, uv);
}

technique VYNE_ExportMotion
<
    ui_label = "VYNE | Export Optical-Flow Motion Blur";
    ui_tooltip = "One scene frame, motion estimated from preceding native frame. No temporal accumulation.";
>
{
    pass CopyNativeCurrent
    {
        VertexShader = PostProcessVS;
        PixelShader = VYNE_CopyScene;
        RenderTarget = VYNE_CurrentTexture;
    }
    pass EstimateAndBlur
    {
        VertexShader = PostProcessVS;
        PixelShader = VYNE_DirectionalBlur;
    }
    pass SaveUnblurredHistory
    {
        VertexShader = PostProcessVS;
        PixelShader = VYNE_SaveHistory;
        RenderTarget = VYNE_PreviousTexture;
    }
}
