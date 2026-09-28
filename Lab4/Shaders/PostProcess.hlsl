//======================================================================================
// POST-PROCESS
//   VS — полноэкранный квад без вершинного буфера (4 вершины, triangle strip,
//        координаты вычисляются из SV_VertexID)
//   PS — заготовка, принимающая текстуры G-буфера + цвет сцены, и три эффекта:
//        1) контуры (edge detection по нормалям и глубине из G-буфера)
//        2) туман по расстоянию и высоте (позиции из G-буфера)
//        3) виньетка + хроматическая аберрация
//======================================================================================

// ---- Входы: G-буфер + результат lighting pass ----
Texture2D gAlbedoMap   : register(t0);   // цвет поверхности
Texture2D gNormalMap   : register(t1);   // мировая нормаль
Texture2D gPositionMap : register(t2);   // мировая позиция, w = 1 если есть геометрия
// t3 — карта теней (здесь не используется)
Texture2D gSceneColor  : register(t4);   // освещённая картинка

SamplerState gsamLinearClamp : register(s0);

// Совпадает с PostConstants в RenderingSystem.h
cbuffer cbPost : register(b0)
{
    float3 gEyePosW;
    float  gTime;

    float4 gFogColor;

    float  gFogStart;
    float  gFogEnd;
    float  gFogBaseY;
    float  gFogHeightFalloff;

    float  gFogDensity;
    float  gOutlineThickness;
    float  gOutlineDepthScale;
    float  gOutlineNormalScale;

    float4 gOutlineColor;

    float  gVignetteStrength;
    float  gChromaticAmount;
    float  gInvWidth;
    float  gInvHeight;

    uint   gFogEnabled;
    uint   gOutlineEnabled;
    uint   gVignetteEnabled;
    uint   gChromaticEnabled;
};

struct VertexOut
{
    float4 PosH : SV_POSITION;
    float2 TexC : TEXCOORD;
};

//--------------------------------------------------------------------------------------
// Полноэкранный квад без вершинного буфера.
//   id: 0 -> (0,0)   1 -> (1,0)   2 -> (0,1)   3 -> (1,1)   (UV)
//   NDC: x = u*2-1, y = 1-v*2  ->  левый верх, правый верх, левый низ, правый низ
// Draw(4) с топологией TRIANGLESTRIP даёт два треугольника на весь экран.
//--------------------------------------------------------------------------------------
VertexOut VS(uint id : SV_VertexID)
{
    VertexOut o;
    o.TexC = float2(id & 1, id >> 1);
    o.PosH = float4(o.TexC.x * 2.0f - 1.0f, 1.0f - o.TexC.y * 2.0f, 0.0f, 1.0f);
    return o;
}

//--------------------------------------------------------------------------------------
// Вспомогательные чтения G-буфера
//--------------------------------------------------------------------------------------
float LinearDepthAt(int2 px)
{
    float4 p = gPositionMap.Load(int3(px, 0));
    return p.w > 0.0f ? distance(p.xyz, gEyePosW) : 1e6f;   // небо — «очень далеко»
}

float3 NormalAt(int2 px)
{
    return gNormalMap.Load(int3(px, 0)).xyz;
}

//--------------------------------------------------------------------------------------
// Эффект 1: контуры. Сравниваем пиксель с 4 соседями:
//   - резкий перепад глубины  -> край объекта (силуэт)
//   - резкий поворот нормали  -> ребро (угол стены, край колонны)
//--------------------------------------------------------------------------------------
float EdgeFactor(int2 px)
{
    int o = max(1, (int)gOutlineThickness);
    int2 offsets[4] = { int2(o, 0), int2(-o, 0), int2(0, o), int2(0, -o) };

    float  dc = LinearDepthAt(px);
    float3 nc = NormalAt(px);

    float depthEdge = 0.0f;
    float normalEdge = 0.0f;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        // не выходим за край экрана
        int2 maxPx = int2(1.0f / gInvWidth, 1.0f / gInvHeight) - 1;
        int2 q = clamp(px + offsets[i], int2(0, 0), maxPx);
        float  d = LinearDepthAt(q);
        float3 n = NormalAt(q);

        depthEdge  += abs(d - dc) / max(min(d, dc), 1e-3f);   // относительный перепад глубины
        normalEdge += 1.0f - saturate(dot(n, nc));
    }

    float e = depthEdge * gOutlineDepthScale * 0.25f + normalEdge * gOutlineNormalScale * 0.25f;
    return smoothstep(0.3f, 0.8f, e);
}

//--------------------------------------------------------------------------------------
// Эффект 2: туман. Плотность растёт с расстоянием и уменьшается с высотой над полом.
//--------------------------------------------------------------------------------------
float FogFactor(float3 posW)
{
    float dist = distance(posW, gEyePosW);
    float byDistance = saturate((dist - gFogStart) / (gFogEnd - gFogStart));
    float byHeight = exp(-max(posW.y - gFogBaseY, 0.0f) * gFogHeightFalloff);
    return saturate(byDistance * lerp(0.35f, 1.0f, byHeight)) * gFogDensity;
}

//--------------------------------------------------------------------------------------
// PIXEL SHADER
//--------------------------------------------------------------------------------------
float4 PS(VertexOut pin) : SV_Target
{
    int2   px = int2(pin.PosH.xy);
    float2 uv = pin.TexC;

    // ---- Эффект 3a: хроматическая аберрация — R и B смещаются от центра в разные стороны ----
    float3 color;
    if (gChromaticEnabled != 0)
    {
        float2 dir = (uv - 0.5f) * gChromaticAmount;
        color.r = gSceneColor.Sample(gsamLinearClamp, uv + dir).r;
        color.g = gSceneColor.Sample(gsamLinearClamp, uv).g;
        color.b = gSceneColor.Sample(gsamLinearClamp, uv - dir).b;
    }
    else
    {
        color = gSceneColor.Sample(gsamLinearClamp, uv).rgb;
    }

    float4 posSample = gPositionMap.Load(int3(px, 0));
    bool hasGeometry = posSample.w > 0.0f;

    // ---- Эффект 1: контуры ----
    if (gOutlineEnabled != 0)
        color = lerp(color, gOutlineColor.rgb, EdgeFactor(px));

    // ---- Эффект 2: туман (только там, где есть геометрия; небо и так «в тумане») ----
    if (gFogEnabled != 0 && hasGeometry)
        color = lerp(color, gFogColor.rgb, FogFactor(posSample.xyz));

    // ---- Эффект 3b: виньетка — затемнение к краям экрана ----
    if (gVignetteEnabled != 0)
    {
        float2 d = (uv - 0.5f) * float2(gInvHeight / gInvWidth, 1.0f);   // учитываем соотношение сторон
        float v = smoothstep(0.35f, 0.95f, length(d) * 1.2f);
        color *= 1.0f - v * gVignetteStrength;
    }

    return float4(color, 1.0f);
}
