//======================================================================================
// LIGHTING PASS (лаба 8) — полноэкранный треугольник, физически корректное освещение.
//
//  Прямой свет:  Cook-Torrance BRDF
//                  f = kD * albedo / PI  +  D * G * F / (4 (N·V)(N·L))
//                  D — GGX (Trowbridge-Reitz), G — Smith + Schlick-GGX, F — Schlick
//  Фоновый свет: IBL (image based lighting), split-sum аппроксимация Карриса:
//                  диффузный  = irradiance(N) * albedo * kD
//                  зеркальный = prefiltered(R, roughness) * (F0 * A + B),  (A, B) = BRDF_LUT(N·V, roughness)
//  Все три карты рассчитаны заранее (gen_ibl.py) и загружаются из .dds.
//
//  G-буфер: Albedo.rgb = базовый цвет, Albedo.a = roughness,
//           Normal.xyz = нормаль,      Normal.w = metallic,
//           Position.xyz = мировая позиция, Position.w = 1 (есть геометрия)
//======================================================================================

#define MAX_LIGHTS    32
#define CASCADE_COUNT 4

#define LIGHT_DIRECTIONAL 0
#define LIGHT_POINT       1
#define LIGHT_SPOT        2

static const float PI = 3.14159265f;

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

Texture2D      gAlbedoMap      : register(t0);
Texture2D      gNormalMap      : register(t1);
Texture2D      gPositionMap    : register(t2);
Texture2DArray gShadowMap      : register(t3);   // слой = каскад

// ---- IBL (лаба 8) ----
TextureCube    gIrradianceMap  : register(t4);   // свёртка окружения с косинусом — диффузный свет
TextureCube    gPrefilteredMap : register(t5);   // окружение, размытое по GGX; mip = roughness * maxLod
Texture2D      gBrdfLut        : register(t6);   // BRDF integration map: (N·V, roughness) -> (A, B)

SamplerComparisonState gsamShadow      : register(s0);
SamplerState           gsamLinearWrap  : register(s1);
SamplerState           gsamLinearClamp : register(s2);

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

    // ---- PBR + IBL ----
    float4x4  gInvViewProj;
    float     gInvScreenWidth;
    float     gInvScreenHeight;
    float     gPrefilteredMaxLod;
    float     gIBLIntensity;
    uint      gIBLEnabled;
    uint      gMaterialOverride;    // 0 — из G-буфера, 1 — гладкий металл, 2 — матовый диэлектрик
    float     gLightScale;
    float     gExposure;

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
// Каскадные тени (без изменений с лабы 5)
//--------------------------------------------------------------------------------------
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

    float3 Poffset = P + N * CascadeTexel(cascade) * 1.5f;   // normal offset против shadow acne

    float4 sp = mul(float4(Poffset, 1.0f), gShadowViewProj[cascade]);
    sp.xyz /= sp.w;

    float2 uv = float2(sp.x * 0.5f + 0.5f, -sp.y * 0.5f + 0.5f);
    float depth = sp.z;

    if (any(uv < 0.0f) || any(uv > 1.0f) || depth > 1.0f)
        return 1.0f;

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
// Cook-Torrance: составные части
//--------------------------------------------------------------------------------------

// D — распределение микрограней GGX / Trowbridge-Reitz
float DistributionGGX(float NdotH, float roughness)
{
    float a  = roughness * roughness;   // «перцептивная» шероховатость -> alpha
    float a2 = a * a;
    float d  = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d);
}

// G1 — Schlick-GGX; для прямого света k = (r + 1)^2 / 8
float GeometrySchlickGGX(float NdotX, float k)
{
    return NdotX / (NdotX * (1.0f - k) + k);
}

// G — Smith: затенение (для L) и маскирование (для V) микрогранями
float GeometrySmith(float NdotV, float NdotL, float roughness)
{
    float r = roughness + 1.0f;
    float k = (r * r) / 8.0f;
    return GeometrySchlickGGX(NdotV, k) * GeometrySchlickGGX(NdotL, k);
}

// F — Френель, аппроксимация Шлика
float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0f - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

// Френель для IBL: у шероховатых поверхностей «блик по краям» слабее
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness)
{
    return F0 + (max((float3)(1.0f - roughness), F0) - F0) * pow(saturate(1.0f - cosTheta), 5.0f);
}

//--------------------------------------------------------------------------------------
// Прямой свет от одного источника (radiance * BRDF * cos)
//--------------------------------------------------------------------------------------
float DistanceAttenuation(float dist, float range)
{
    float x = saturate(1.0f - dist / range);
    return x * x;
}

float3 ComputeLightPBR(LightData light, float3 P, float3 N, float3 V,
                       float3 albedo, float roughness, float metallic, float3 F0)
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

    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 1e-4f);
    float NdotH = saturate(dot(N, H));
    float HdotV = saturate(dot(H, V));

    // Для точечных источников совсем нулевая шероховатость даёт бесконечно узкий блик
    float r = max(roughness, 0.04f);

    float  D = DistributionGGX(NdotH, r);
    float  G = GeometrySmith(NdotV, NdotL, r);
    float3 F = FresnelSchlick(HdotV, F0);

    float3 specular = (D * G * F) / (4.0f * NdotV * NdotL + 1e-4f);

    // Энергия, не отражённая зеркально, уходит в диффузию; у металлов диффузии нет
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 diffuse = kD * albedo / PI;

    // gLightScale = PI: «Intensity» источника в сцене задавалась так, что белая поверхность
    // при NdotL = 1 имела яркость Intensity (как в Ламберте прошлых лаб) — сохраняем тот же вид
    float3 radiance = light.Color * light.Intensity * gLightScale * attenuation;

    return (diffuse + specular) * radiance * NdotL;
}

//--------------------------------------------------------------------------------------
// IBL: фоновое освещение от окружения
//--------------------------------------------------------------------------------------
float3 ComputeIBL(float3 N, float3 V, float3 albedo, float roughness, float metallic, float3 F0)
{
    float NdotV = max(dot(N, V), 1e-4f);
    float3 R = reflect(-V, N);

    float3 F  = FresnelSchlickRoughness(NdotV, F0, roughness);
    float3 kD = (1.0f - F) * (1.0f - metallic);

    // 1) Диффузная часть: irradiance map уже содержит интеграл по полусфере вокруг N
    float3 irradiance = gIrradianceMap.SampleLevel(gsamLinearWrap, N, 0.0f).rgb;
    float3 diffuse = irradiance * albedo;

    // 2) Зеркальная часть (split-sum):
    //    первая сумма — pre-filtered environment map (mip выбирается по шероховатости),
    //    вторая — BRDF integration map: масштаб A и смещение B для F0
    float3 prefiltered = gPrefilteredMap.SampleLevel(gsamLinearWrap, R, roughness * gPrefilteredMaxLod).rgb;
    float2 envBrdf = gBrdfLut.SampleLevel(gsamLinearClamp, float2(NdotV, roughness), 0.0f).rg;
    float3 specular = prefiltered * (F * envBrdf.x + envBrdf.y);

    return (kD * diffuse + specular) * gIBLIntensity;
}

//--------------------------------------------------------------------------------------
// Тонмаппинг: HDR -> [0,1] (ACES, аппроксимация Narkowicz)
//--------------------------------------------------------------------------------------
float3 ToneMapACES(float3 x)
{
    const float a = 2.51f, b = 0.03f, c = 2.43f, d = 0.59f, e = 0.14f;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
}

float4 FinalColor(float3 hdr)
{
    float3 ldr = ToneMapACES(hdr * gExposure);
    return float4(pow(ldr, 1.0f / 2.2f), 1.0f);
}

// Направление взгляда через пиксель: точка на дальней плоскости -> мир
float3 ViewRay(float2 pixel)
{
    float2 uv  = (pixel + 0.5f) * float2(gInvScreenWidth, gInvScreenHeight);
    float2 ndc = float2(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f);
    float4 p = mul(float4(ndc, 1.0f, 1.0f), gInvViewProj);
    return normalize(p.xyz / p.w - gEyePosW);
}

//--------------------------------------------------------------------------------------
// PIXEL SHADER
//--------------------------------------------------------------------------------------
float4 PS(VertexOut pin) : SV_Target
{
    int3 coord = int3(pin.PosH.xy, 0);

    float4 albedoSample   = gAlbedoMap.Load(coord);
    float4 normalSample   = gNormalMap.Load(coord);
    float4 positionSample = gPositionMap.Load(coord);

    if (gDebugView == 1) return float4(albedoSample.rgb, 1.0f);
    if (gDebugView == 2) return float4(normalSample.xyz * 0.5f + 0.5f, 1.0f);
    if (gDebugView == 3) return float4(frac(positionSample.xyz * gPositionScale), 1.0f);
    if (gDebugView == 4) return float4(albedoSample.a, normalSample.w, 0.0f, 1.0f);   // R = roughness, G = metallic

    // ---- Небо: то же окружение, из которого рассчитан IBL ----
    if (positionSample.w == 0.0f)
    {
        if (gIBLEnabled != 0)
            return FinalColor(gPrefilteredMap.SampleLevel(gsamLinearWrap, ViewRay(pin.PosH.xy), 0.0f).rgb);
        return gSkyColor;
    }

    float3 albedo    = pow(abs(albedoSample.rgb), 2.2f);   // sRGB -> линейное пространство
    float  roughness = saturate(albedoSample.a);
    float  metallic  = saturate(normalSample.w);

    // Демонстрация: все поверхности — один материал
    if (gMaterialOverride == 1)      { roughness = 0.15f; metallic = 1.0f; }
    else if (gMaterialOverride == 2) { roughness = 1.0f;  metallic = 0.0f; }

    float3 P = positionSample.xyz;
    float3 N = normalize(normalSample.xyz);
    float3 V = normalize(gEyePosW - P);

    // Отражательная способность при нормальном падении: 4% у диэлектриков, цвет — у металлов
    float3 F0 = lerp((float3)0.04f, albedo, metallic);

    float viewDepth = dot(P - gEyePosW, gCameraForward);
    uint cascade = SelectCascade(viewDepth);

    // ---- Прямой свет ----
    float3 color = 0.0f;
    [loop]
    for (uint i = 0; i < gNumLights; ++i)
    {
        float3 contribution = ComputeLightPBR(gLights[i], P, N, V, albedo, roughness, metallic, F0);
        if ((int)i == gShadowLightIndex)
            contribution *= ShadowFactor(P, N, cascade);
        color += contribution;
    }

    // ---- Фоновый свет: IBL или (если выключен) постоянный ambient ----
    if (gIBLEnabled != 0)
        color += ComputeIBL(N, V, albedo, roughness, metallic, F0);
    else
        color += gAmbient.rgb * albedo;

    if (gShowCascades != 0 && cascade < CASCADE_COUNT)
    {
        static const float3 tints[CASCADE_COUNT] =
        {
            float3(1.0f, 0.4f, 0.4f), float3(0.4f, 1.0f, 0.4f),
            float3(0.4f, 0.4f, 1.0f), float3(1.0f, 1.0f, 0.4f)
        };
        color *= tints[cascade];
    }

    return FinalColor(color);
}
