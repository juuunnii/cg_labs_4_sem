//======================================================================================
// SPONZA SHADER — текстуры, материалы, маски прозрачности, текстурная анимация и тайлинг
//======================================================================================

Texture2D    gDiffuseMap   : register(t0);  // map_Kd  (белая, если у материала нет текстуры)
Texture2D    gAlphaMap     : register(t1);  // map_d   (белая, если маски нет)
SamplerState gsamAnisoWrap : register(s0);  // режим WRAP — нужен для тайлинга

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
}

// Совпадает со структурой MaterialConstants из d3dUtil.h
cbuffer cbMaterial : register(b1)
{
    float4   gDiffuseAlbedo;
    float3   gFresnelR0;
    float    gRoughness;
    float4x4 gMatTransform;   // преобразование UV: тайлинг + анимация
};

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
    float2 TexC    : TEXCOORD;
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    float2 TexC    : TEXCOORD;
};

static const float3 gLightDir[2] = {
    normalize(float3(1.0f, 2.0f, 1.0f)),   // основной
    normalize(float3(-1.0f, 1.0f, -1.0f))  // заполняющий
};

static const float3 gLightColor[2] = {
    float3(1.0f, 0.95f, 0.9f),
    float3(0.8f, 0.9f, 1.0f)
};

static const float gLightIntensity[2] = { 0.9f, 0.5f };

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosW    = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.PosH    = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);

    // Тайлинг (масштаб) и анимация (сдвиг) текстурных координат
    vout.TexC = mul(float4(vin.TexC, 0.0f, 1.0f), gMatTransform).xy;
    return vout;
}

float4 PS(VertexOut pin) : SV_Target
{
    // Маска прозрачности: у одних файлов форма в цвете, у других — в альфа-канале
    float4 maskTex = gAlphaMap.Sample(gsamAnisoWrap, pin.TexC);
    clip(maskTex.r * maskTex.a - 0.5f);

    float4 texColor = gDiffuseMap.Sample(gsamAnisoWrap, pin.TexC);

    // Текстуры в sRGB -> в линейное пространство для освещения
    float3 baseColor = pow(abs(texColor.rgb), 2.2f) * gDiffuseAlbedo.rgb;

    float3 N = normalize(pin.NormalW);

    float3 Lo = 0;
    [unroll]
    for (int i = 0; i < 2; i++)
    {
        float NdotL = max(0.2f, dot(N, gLightDir[i]));
        Lo += baseColor * NdotL * gLightColor[i] * gLightIntensity[i];
    }

    float3 ambient = baseColor * 0.15f;
    float3 color = ambient + Lo;

    // Обратно в sRGB для вывода на экран
    color = pow(abs(color), 1.0f / 2.2f);

    return float4(color, 1.0f);
}
