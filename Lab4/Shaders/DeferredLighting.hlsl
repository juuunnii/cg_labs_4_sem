//======================================================================================
// LIGHTING PASS — полноэкранный треугольник.
// Для каждого пикселя читаем G-буфер, суммируем вклад всех источников,
// для солнца учитываем каскадные тени с PCF.
//======================================================================================

#define MAX_LIGHTS    32
#define CASCADE_COUNT 4

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

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

Texture2D      gAlbedoMap   : register(t0);
Texture2D      gNormalMap   : register(t1);
Texture2D      gPositionMap : register(t2);
Texture2DArray gShadowMap   : register(t3);   // слой = каскад

SamplerComparisonState gsamShadow : register(s0);

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

    float4x4  gShadowViewProj[CASCADE_COUNT];
    float4    gCascadeSplits;       // дальняя граница каждого каскада (глубина в камере)
    float4    gCascadeTexelWorld;   // размер тексела карты теней в мире
    float3    gCameraForward;
    uint      gShadowsEnabled;
    uint      gPcfKernel;           // 1, 3, 5, 7
    uint      gShowCascades;
    int       gShadowLightIndex;
    float     gShadowMapSize;

    LightData gLights[MAX_LIGHTS];
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
};

VertexOut VS(uint id : SV_VertexID)
{
    VertexOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.PosH = float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return o;
}

//--------------------------------------------------------------------------------------
// Каскадные тени
//--------------------------------------------------------------------------------------

// Номер каскада по глубине точки в пространстве камеры; CASCADE_COUNT — за пределами теней
uint SelectCascade(float viewDepth)
{
    if (viewDepth <= gCascadeSplits.x) return 0;
    if (viewDepth <= gCascadeSplits.y) return 1;
    if (viewDepth <= gCascadeSplits.z) return 2;
    if (viewDepth <= gCascadeSplits.w) return 3;
    return CASCADE_COUNT;
}

float CascadeTexel(uint c)
{
    return c == 0 ? gCascadeTexelWorld.x : c == 1 ? gCascadeTexelWorld.y
         : c == 2 ? gCascadeTexelWorld.z : gCascadeTexelWorld.w;
}

// 1 — освещено, 0 — в тени
float ShadowFactor(float3 P, float3 N, uint cascade)
{
    if (gShadowsEnabled == 0 || cascade >= CASCADE_COUNT)
        return 1.0f;

    // Normal offset: чуть сдвигаем точку вдоль нормали — против «теневых прыщей»
    float3 Poffset = P + N * CascadeTexel(cascade) * 1.5f;

    float4 sp = mul(float4(Poffset, 1.0f), gShadowViewProj[cascade]);
    sp.xyz /= sp.w;

    // NDC -> UV текстуры (ось Y в текстуре направлена вниз)
    float2 uv = float2(sp.x * 0.5f + 0.5f, -sp.y * 0.5f + 0.5f);
    float depth = sp.z;

    if (any(uv < 0.0f) || any(uv > 1.0f) || depth > 1.0f)
        return 1.0f;

    // PCF: усредняем сравнения в окне k x k текселей.
    // Каждое SampleCmp ещё и билинейно смешивает 4 соседа — края получаются мягкими.
    const float texel = 1.0f / gShadowMapSize;
    const int r = (int)gPcfKernel / 2;

    float lit = 0.0f;
    float count = 0.0f;
    [loop]
    for (int y = -r; y <= r; ++y)
    {
        [loop]
        for (int x = -r; x <= r; ++x)
        {
            float2 offset = float2(x, y) * texel;
            lit += gShadowMap.SampleCmpLevelZero(gsamShadow, float3(uv + offset, cascade), depth);
            count += 1.0f;
        }
    }
    return lit / count;
}

//--------------------------------------------------------------------------------------
// Освещение
//--------------------------------------------------------------------------------------
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
            float cosAngle = dot(-L, light.Direction);
            attenuation *= smoothstep(light.SpotOuterCos, light.SpotInnerCos, cosAngle);
        }
    }

    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0f || attenuation <= 0.0f)
        return 0;

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

    if (gDebugView == 1) return float4(albedoSample.rgb, 1.0f);
    if (gDebugView == 2) return float4(normalSample.xyz * 0.5f + 0.5f, 1.0f);
    if (gDebugView == 3) return float4(frac(positionSample.xyz * gPositionScale), 1.0f);

    if (positionSample.w == 0.0f)
        return gSkyColor;

    float3 albedo = pow(abs(albedoSample.rgb), 2.2f);
    float3 P = positionSample.xyz;
    float3 N = normalize(normalSample.xyz);
    float3 V = normalize(gEyePosW - P);

    // Глубина точки в пространстве камеры — по ней выбирается каскад
    float viewDepth = dot(P - gEyePosW, gCameraForward);
    uint cascade = SelectCascade(viewDepth);

    float3 color = gAmbient.rgb * albedo;

    [loop]
    for (uint i = 0; i < gNumLights; ++i)
    {
        float3 contribution = ComputeLight(gLights[i], P, N, V, albedo);
        if ((int)i == gShadowLightIndex)
            contribution *= ShadowFactor(P, N, cascade);
        color += contribution;
    }

    // Отладка: подкрасить каскады (красный, зелёный, синий, жёлтый)
    if (gShowCascades != 0 && cascade < CASCADE_COUNT)
    {
        static const float3 tints[CASCADE_COUNT] =
        {
            float3(1.0f, 0.4f, 0.4f), float3(0.4f, 1.0f, 0.4f),
            float3(0.4f, 0.4f, 1.0f), float3(1.0f, 1.0f, 0.4f)
        };
        color *= tints[cascade];
    }

    color = saturate(color);
    return float4(pow(color, 1.0f / 2.2f), 1.0f);
}
