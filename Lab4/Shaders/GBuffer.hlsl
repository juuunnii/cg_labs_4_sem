//======================================================================================
// GEOMETRY PASS — записывает свойства поверхности в G-буфер
//   SV_Target0  Albedo    (цвет текстуры, sRGB)
//   SV_Target1  Normal    (мировая нормаль)
//   SV_Target2  Position  (мировая позиция, w = 1)
//======================================================================================

Texture2D    gDiffuseMap   : register(t0);
Texture2D    gAlphaMap     : register(t1);
SamplerState gsamAnisoWrap : register(s0);

cbuffer cbPerObject : register(b0)
{
    float4x4 gWorldViewProj;
    float4x4 gWorld;
}

// Совпадает с MaterialConstants из d3dUtil.h
cbuffer cbMaterial : register(b1)
{
    float4   gDiffuseAlbedo;
    float3   gFresnelR0;
    float    gRoughness;
    float4x4 gMatTransform;   // тайлинг + анимация UV
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

struct GBufferOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Position : SV_Target2;
};

VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosW    = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.PosH    = mul(float4(vin.PosL, 1.0f), gWorldViewProj);
    vout.NormalW = mul(vin.NormalL, (float3x3)gWorld);
    vout.TexC    = mul(float4(vin.TexC, 0.0f, 1.0f), gMatTransform).xy;
    return vout;
}

GBufferOutput PS(VertexOut pin, bool isFrontFace : SV_IsFrontFace)
{
    // Маска прозрачности (листья, цепи)
    float4 maskTex = gAlphaMap.Sample(gsamAnisoWrap, pin.TexC);
    clip(maskTex.r * maskTex.a - 0.5f);

    float4 texColor = gDiffuseMap.Sample(gsamAnisoWrap, pin.TexC);

    // Для обратной стороны одинарных плоскостей (ткани, листья) разворачиваем нормаль
    float3 N = normalize(pin.NormalW);
    if (!isFrontFace)
        N = -N;

    GBufferOutput o;
    o.Albedo   = float4(texColor.rgb * gDiffuseAlbedo.rgb, 1.0f);
    o.Normal   = float4(N, 0.0f);
    o.Position = float4(pin.PosW, 1.0f);   // w = 1 — «здесь есть геометрия»
    return o;
}
