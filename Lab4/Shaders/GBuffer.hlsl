//======================================================================================
// GEOMETRY PASS с тесселяцией
//   VS  — переводит вершины в мировые координаты
//   HS  — выбирает уровень тесселяции каждого ребра по расстоянию до камеры
//   DS  — вершины после тесселятора сдвигаются по нормали на значение из карты высот
//   PS  — normal mapping + запись в G-буфер
//
//   SV_Target0  Albedo    (цвет текстуры, sRGB)
//   SV_Target1  Normal    (мировая нормаль с учётом карты нормалей)
//   SV_Target2  Position  (мировая позиция, w = 1)
//======================================================================================

Texture2D    gDiffuseMap    : register(t0);
Texture2D    gAlphaMap      : register(t1);
Texture2D    gNormalMap     : register(t2);   // карта нормалей в касательном пространстве
Texture2D    gHeightMap     : register(t3);   // карта высот (0.5 = без сдвига)

SamplerState gsamAnisoWrap  : register(s0);
SamplerState gsamLinearWrap : register(s1);

// Совпадает с ObjectConstants в main.cpp
cbuffer cbPerObject : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProj;

    float3   gEyePosW;
    float    gTessMinDist;        // ближе — максимальная тесселяция

    float    gTessMaxDist;        // дальше — тесселяции нет (фактор = gTessMinFactor)
    float    gTessMinFactor;
    float    gTessMaxFactor;
    float    gTessMinEdgeLength;  // не дробить ребро на куски короче этой длины

    float    gDisplacementScale;
    uint     gUseNormalMap;
    uint     gUseDisplacement;
    uint     gUseTessellation;
}

// Совпадает с SurfaceConstants в main.cpp
cbuffer cbMaterial : register(b1)
{
    float4   gDiffuseAlbedo;
    float4x4 gMatTransform;       // тайлинг + анимация UV
    float    gMaterialDisplacement;   // 0 — материал не выдавливается (листья, ткани без карты)
    float3   gMatPad;
};

struct VertexIn
{
    float3 PosL     : POSITION;
    float3 NormalL  : NORMAL;
    float2 TexC     : TEXCOORD;
    float4 TangentL : TANGENT;
};

// Контрольная точка патча (выход VS и HS)
struct VertexOut
{
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float4 TangentW : TANGENT;
    float2 TexC     : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// VERTEX SHADER
//--------------------------------------------------------------------------------------
VertexOut VS(VertexIn vin)
{
    VertexOut vout;
    vout.PosW     = mul(float4(vin.PosL, 1.0f), gWorld).xyz;
    vout.NormalW  = mul(vin.NormalL, (float3x3)gWorld);
    vout.TangentW = float4(mul(vin.TangentL.xyz, (float3x3)gWorld), vin.TangentL.w);
    vout.TexC     = mul(float4(vin.TexC, 0.0f, 1.0f), gMatTransform).xy;
    return vout;
}

//--------------------------------------------------------------------------------------
// HULL SHADER
//--------------------------------------------------------------------------------------
struct PatchTess
{
    float EdgeTess[3] : SV_TessFactor;
    float InsideTess  : SV_InsideTessFactor;
};

// Уровень тесселяции ребра зависит от расстояния от его середины до камеры.
// Считается только по двум концам ребра, поэтому у соседних треугольников
// общее ребро получает одинаковый фактор — без трещин.
float EdgeTessFactor(float3 p0, float3 p1)
{
    if (gUseTessellation == 0)
        return 1.0f;

    float3 mid = 0.5f * (p0 + p1);
    float  d   = distance(mid, gEyePosW);

    // 0 — рядом с камерой, 1 — далеко
    float t = saturate((d - gTessMinDist) / (gTessMaxDist - gTessMinDist));
    float factor = lerp(gTessMaxFactor, gTessMinFactor, t);

    // Мелкие рёбра (львы, растения) не нужно дробить так же сильно, как огромный пол
    float lengthLimit = distance(p0, p1) / gTessMinEdgeLength;

    return clamp(min(factor, lengthLimit), 1.0f, 64.0f);
}

PatchTess ConstantHS(InputPatch<VertexOut, 3> patch, uint patchID : SV_PrimitiveID)
{
    PatchTess pt;

    // Ребро i лежит напротив вершины i
    pt.EdgeTess[0] = EdgeTessFactor(patch[1].PosW, patch[2].PosW);
    pt.EdgeTess[1] = EdgeTessFactor(patch[2].PosW, patch[0].PosW);
    pt.EdgeTess[2] = EdgeTessFactor(patch[0].PosW, patch[1].PosW);
    pt.InsideTess  = (pt.EdgeTess[0] + pt.EdgeTess[1] + pt.EdgeTess[2]) / 3.0f;

    return pt;
}

[domain("tri")]
[partitioning("fractional_odd")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("ConstantHS")]
[maxtessfactor(64.0f)]
VertexOut HS(InputPatch<VertexOut, 3> patch, uint i : SV_OutputControlPointID)
{
    return patch[i];   // контрольные точки передаём без изменений
}

//--------------------------------------------------------------------------------------
// DOMAIN SHADER — вызывается для каждой вершины, созданной тесселятором
//--------------------------------------------------------------------------------------
struct DomainOut
{
    float4 PosH     : SV_POSITION;
    float3 PosW     : POSITION;
    float3 NormalW  : NORMAL;
    float4 TangentW : TANGENT;
    float2 TexC     : TEXCOORD;
};

[domain("tri")]
DomainOut DS(PatchTess pt, float3 bary : SV_DomainLocation, const OutputPatch<VertexOut, 3> tri)
{
    // Интерполяция атрибутов по барицентрическим координатам
    float3 posW    = bary.x * tri[0].PosW     + bary.y * tri[1].PosW     + bary.z * tri[2].PosW;
    float3 normalW = bary.x * tri[0].NormalW  + bary.y * tri[1].NormalW  + bary.z * tri[2].NormalW;
    float4 tangW   = bary.x * tri[0].TangentW + bary.y * tri[1].TangentW + bary.z * tri[2].TangentW;
    float2 texC    = bary.x * tri[0].TexC     + bary.y * tri[1].TexC     + bary.z * tri[2].TexC;

    float3 N = normalize(normalW);

    // Displacement: сдвиг вдоль нормали на высоту из текстуры (0.5 — нулевой уровень)
    if (gUseDisplacement != 0 && gUseTessellation != 0)
    {
        // Вдали берём более мелкий mip — меньше мерцания
        float d = distance(posW, gEyePosW);
        float mip = saturate((d - gTessMinDist) / (gTessMaxDist - gTessMinDist)) * 6.0f;

        float h = gHeightMap.SampleLevel(gsamLinearWrap, texC, mip).r;
        posW += N * (h - 0.5f) * gDisplacementScale * gMaterialDisplacement;
    }

    DomainOut dout;
    dout.PosW     = posW;
    dout.PosH     = mul(float4(posW, 1.0f), gViewProj);
    dout.NormalW  = N;
    dout.TangentW = tangW;
    dout.TexC     = texC;
    return dout;
}

//--------------------------------------------------------------------------------------
// PIXEL SHADER — normal mapping и запись в G-буфер
//--------------------------------------------------------------------------------------
struct GBufferOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Position : SV_Target2;
};

GBufferOutput PS(DomainOut pin, bool isFrontFace : SV_IsFrontFace)
{
    float4 maskTex = gAlphaMap.Sample(gsamAnisoWrap, pin.TexC);
    clip(maskTex.r * maskTex.a - 0.5f);

    float4 texColor = gDiffuseMap.Sample(gsamAnisoWrap, pin.TexC);

    float3 N = normalize(pin.NormalW);

    if (gUseNormalMap != 0)
    {
        // Базис TBN: T ортогонализуем к N, B восстанавливаем через знак w
        float3 T = pin.TangentW.xyz - dot(pin.TangentW.xyz, N) * N;
        if (dot(T, T) > 1e-8f)
        {
            T = normalize(T);
            float3 B = (pin.TangentW.w < 0.0f ? -1.0f : 1.0f) * cross(N, T);

            // [0,1] -> [-1,1], из касательного пространства — в мировое
            float3 nT = gNormalMap.Sample(gsamAnisoWrap, pin.TexC).xyz * 2.0f - 1.0f;
            N = normalize(nT.x * T + nT.y * B + nT.z * N);
        }
    }

    // Обратная сторона одинарных плоскостей
    if (!isFrontFace)
        N = -N;

    GBufferOutput o;
    o.Albedo   = float4(texColor.rgb * gDiffuseAlbedo.rgb, 1.0f);
    o.Normal   = float4(N, 0.0f);
    o.Position = float4(pin.PosW, 1.0f);
    return o;
}
