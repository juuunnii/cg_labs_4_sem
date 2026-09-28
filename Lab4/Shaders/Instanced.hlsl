//======================================================================================
// GEOMETRY PASS для множества объектов (инстансинг).
// Матрица и цвет каждого экземпляра берутся из StructuredBuffer по SV_InstanceID.
// В буфер попадают только объекты, прошедшие frustum culling на CPU.
//======================================================================================

struct InstanceData
{
    float4x4 World;
    float4   Color;
    float4   Surface;   // лаба 8: x = roughness, y = metallic
};

StructuredBuffer<InstanceData> gInstances : register(t0);

// Тот же буфер кадра, что и в GBuffer.hlsl — нужны только первые две матрицы
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;
}

// Смещение текущей группы в буфере экземпляров (root constant)
cbuffer cbInstanceOffset : register(b1)
{
    uint gInstanceOffset;
}

struct VertexIn
{
    float3 PosL    : POSITION;
    float3 NormalL : NORMAL;
};

struct VertexOut
{
    float4 PosH    : SV_POSITION;
    float3 PosW    : POSITION;
    float3 NormalW : NORMAL;
    nointerpolation float4 Color : COLOR;
    nointerpolation float2 Surface : SURFACE;
};

VertexOut VS(VertexIn vin, uint instanceID : SV_InstanceID)
{
    InstanceData inst = gInstances[gInstanceOffset + instanceID];

    VertexOut vout;
    float4 posW   = mul(float4(vin.PosL, 1.0f), inst.World);
    vout.PosW     = posW.xyz;
    vout.PosH     = mul(posW, gViewProj);
    vout.NormalW  = mul(vin.NormalL, (float3x3)inst.World);   // масштаб равномерный — так можно
    vout.Color    = inst.Color;
    vout.Surface  = inst.Surface.xy;
    return vout;
}

struct GBufferOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Position : SV_Target2;
};

GBufferOutput PS(VertexOut pin)
{
    GBufferOutput o;
    // PBR: roughness — в альфу альбедо, metallic — в w нормали
    o.Albedo   = float4(pin.Color.rgb, pin.Surface.x);
    o.Normal   = float4(normalize(pin.NormalW), pin.Surface.y);
    o.Position = float4(pin.PosW, 1.0f);
    return o;
}
