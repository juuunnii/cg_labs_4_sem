//======================================================================================
// LIGHTING PASS — полноэкранный треугольник.
// Для каждого пикселя читаем G-буфер и суммируем вклад всех источников света.
//======================================================================================

#define MAX_LIGHTS 32

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

// Совпадает с LightData в RenderingSystem.h (64 байта)
struct LightData
{
    float3 Position;
    float  Range;
    float3 Direction;
    float  SpotInnerCos;
    float3 Color;
    float  Intensity;
    int    Type;
    float  SpotOuterCos;
    float2 Pad;
};

Texture2D gAlbedoMap   : register(t0);
Texture2D gNormalMap   : register(t1);
Texture2D gPositionMap : register(t2);

// Совпадает с LightingPassConstants в RenderingSystem.h
cbuffer cbLighting : register(b0)
{
    float3    gEyePosW;
    uint      gNumLights;
    float4    gAmbient;
    float4    gSkyColor;
    uint      gDebugView;
    float     gPositionScale;
    float2    gPad;
    LightData gLights[MAX_LIGHTS];
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

// Полноэкранный треугольник без вершинного буфера:
// id 0 -> (-1, 1), id 1 -> (3, 1), id 2 -> (-1, -3)
VertexOut VS(uint id : SV_VertexID)
{
    VertexOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

// Плавное затухание до нуля на расстоянии Range
float DistanceAttenuation(float dist, float range)
{
    float x = saturate(1.0f - dist / range);
    return x * x;
}

float3 ComputeLight(LightData light, float3 P, float3 N, float3 V, float3 albedo)
{
    float3 L;
    float attenuation = 1.0f;

    if (light.Type == LIGHT_DIRECTIONAL)
    {
        L = -light.Direction;
    }
    else
    {
        float3 toLight = light.Position - P;
        float dist = length(toLight);
        L = toLight / max(dist, 1e-4f);
        attenuation = DistanceAttenuation(dist, light.Range);

        if (light.Type == LIGHT_SPOT)
        {
            // Угол между лучом прожектора и направлением на точку
            float cosAngle = dot(-L, light.Direction);
            attenuation *= smoothstep(light.SpotOuterCos, light.SpotInnerCos, cosAngle);
        }
    }

    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f || attenuation <= 0.0f)
        return 0;

    // Blinn-Phong
    float3 H = normalize(L + V);
    float specular = pow(saturate(dot(N, H)), 32.0f) * 0.25f;

    return (albedo + specular) * NdotL * light.Color * light.Intensity * attenuation;
}

float4 PS(VertexOut pin) : SV_Target
{
    int3 coord = int3(pin.PosH.xy, 0);

    float4 albedoSample   = gAlbedoMap.Load(coord);
    float4 normalSample   = gNormalMap.Load(coord);
    float4 positionSample = gPositionMap.Load(coord);

    // ---- Отладочные режимы (клавиша G): показать содержимое G-буфера ----
    if (gDebugView == 1) return float4(albedoSample.rgb, 1.0f);
    if (gDebugView == 2) return float4(normalSample.xyz * 0.5f + 0.5f, 1.0f);
    if (gDebugView == 3) return float4(frac(positionSample.xyz * gPositionScale), 1.0f);

    // Нет геометрии — небо
    if (positionSample.w == 0.0f)
        return gSkyColor;

    float3 albedo = pow(abs(albedoSample.rgb), 2.2f);   // sRGB -> линейное
    float3 P = positionSample.xyz;
    float3 N = normalize(normalSample.xyz);
    float3 V = normalize(gEyePosW - P);

    float3 color = gAmbient.rgb * albedo;

    [loop]
    for (uint i = 0; i < gNumLights; ++i)
        color += ComputeLight(gLights[i], P, N, V, albedo);

    color = saturate(color);
    return float4(pow(color, 1.0f / 2.2f), 1.0f);        // линейное -> sRGB
}
