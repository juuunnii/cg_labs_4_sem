//======================================================================================
// СИСТЕМА ЧАСТИЦ НА GPU
//   CSEmit      — рождение частиц: Append в буфер A
//   CSUpdate    — физика: Consume из A, живые частицы Append в B
//   VSParticle  — читает частицу из StructuredBuffer по номеру вершины
//   GSParticle  — разворачивает точку в квадрат, повёрнутый к камере (билборд)
//   PSParticle  — непрозрачный «шарик» в G-буфер (освещается deferred-светом)
//======================================================================================

// Совпадает с struct Particle в ParticleSystem.h (64 байта)
struct Particle
{
    float3 Position;
    float  Age;
    float3 Velocity;
    float  Lifetime;
    float4 Color;
    float  Size;
    float3 Pad;
};

// Совпадает с ParticleConstants в ParticleSystem.h
cbuffer cbSim : register(b0)
{
    float4x4 gViewProj;

    float3 gEyePosW;     float gDeltaTime;
    float3 gCameraRight; float gTime;
    float3 gCameraUp;    uint  gEmitCount;
    float3 gGravity;     uint  gMaxParticles;

    float4 gEmitters[4];

    uint  gEmitterCount;
    float gFloorY;
    float gBaseSize;
    float gBaseSpeed;

    float gSpread;
    float gMinLife;
    float gMaxLife;
    uint  gSeed;
};

// Сколько частиц сейчас в буфере A (копия UAV-счётчика, сделанная на GPU)
cbuffer cbCount : register(b1)
{
    uint  gParticleCount;
    uint3 gCountPad;
};

//======================================================================================
// COMPUTE
//======================================================================================
ConsumeStructuredBuffer<Particle> gConsume : register(u0);
AppendStructuredBuffer<Particle>  gAppend  : register(u1);

// Генератор псевдослучайных чисел на GPU
uint WangHash(uint s)
{
    s = (s ^ 61u) ^ (s >> 16);
    s *= 9u;
    s ^= s >> 4;
    s *= 0x27d4eb2du;
    s ^= s >> 15;
    return s;
}

float Rand(inout uint state)
{
    state = WangHash(state);
    return (state & 0x00FFFFFFu) / 16777216.0f;   // [0, 1)
}

float3 HueToRGB(float h)
{
    float3 k = abs(frac(h + float3(0.0f, 2.0f / 3.0f, 1.0f / 3.0f)) * 6.0f - 3.0f) - 1.0f;
    return saturate(k);
}

[numthreads(64, 1, 1)]
void CSEmit(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gEmitCount)
        return;
    // Не переполняем буфер: живые + новые <= максимум
    if (gParticleCount + id.x >= gMaxParticles)
        return;

    uint rng = WangHash(id.x * 1973u + gSeed * 9277u + 26699u);

    uint e = min((uint)(Rand(rng) * gEmitterCount), gEmitterCount - 1);
    float3 origin = gEmitters[e].xyz;

    // Направление в горизонтальной плоскости + разброс
    float angle  = Rand(rng) * 6.2831853f;
    float spread = Rand(rng) * gSpread;
    float3 dir   = float3(cos(angle), 0.0f, sin(angle));

    Particle p;
    p.Position = origin + dir * gBaseSize * 4.0f * Rand(rng) + float3(0.0f, gBaseSize, 0.0f);
    p.Velocity = (float3(0.0f, 1.0f, 0.0f) * lerp(0.8f, 1.1f, Rand(rng)) + dir * spread) * gBaseSpeed;
    p.Age      = 0.0f;
    p.Lifetime = lerp(gMinLife, gMaxLife, Rand(rng));

    // Каждый фонтан — свой оттенок
    float hue = frac(e * 0.33f + Rand(rng) * 0.12f);
    p.Color = float4(lerp(float3(1, 1, 1), HueToRGB(hue), 0.85f), 1.0f);

    p.Size = gBaseSize * lerp(0.6f, 1.4f, Rand(rng));
    p.Pad  = float3(0.0f, 0.0f, 0.0f);

    gAppend.Append(p);
}

[numthreads(256, 1, 1)]
void CSUpdate(uint3 id : SV_DispatchThreadID)
{
    // Потоков запускается с запасом — лишние сразу выходят
    if (id.x >= gParticleCount)
        return;

    Particle p = gConsume.Consume();

    p.Age += gDeltaTime;
    if (p.Age >= p.Lifetime)
        return;                       // частица умерла — просто не добавляем её в B

    // Интегрирование: гравитация -> скорость -> позиция
    p.Velocity += gGravity * gDeltaTime;
    p.Position += p.Velocity * gDeltaTime;

    // Отскок от пола с потерей энергии
    if (p.Position.y < gFloorY + p.Size)
    {
        p.Position.y = gFloorY + p.Size;
        if (p.Velocity.y < 0.0f)
            p.Velocity.y = -p.Velocity.y * 0.45f;
        p.Velocity.xz *= 0.8f;
    }

    gAppend.Append(p);
}

//======================================================================================
// ОТРИСОВКА
//======================================================================================
StructuredBuffer<Particle> gParticles : register(t0);

struct VSOut
{
    float3 PosW  : POSITION;
    float  Size  : SIZE;
    float4 Color : COLOR;
};

VSOut VSParticle(uint vid : SV_VertexID)
{
    Particle p = gParticles[vid];

    // В последней четверти жизни частица плавно уменьшается до нуля
    float lifeLeft = 1.0f - p.Age / p.Lifetime;

    VSOut o;
    o.PosW  = p.Position;
    o.Size  = p.Size * saturate(lifeLeft * 4.0f);
    o.Color = p.Color;
    return o;
}

struct GSOut
{
    float4 PosH    : SV_POSITION;
    float3 CenterW : POSITION;
    float2 Corner  : TEXCOORD;   // [-1, 1] внутри квадрата
    float  Size    : SIZE;
    float4 Color   : COLOR;
};

// Одна точка -> 4 вершины (два треугольника), развёрнутые к камере
[maxvertexcount(4)]
void GSParticle(point VSOut gin[1], inout TriangleStream<GSOut> stream)
{
    const float2 corners[4] =
    {
        float2(-1.0f, -1.0f), float2(-1.0f, 1.0f),
        float2( 1.0f, -1.0f), float2( 1.0f, 1.0f)
    };

    if (gin[0].Size <= 0.0f)
        return;   // частица «догорела» — ничего не рисуем

    float3 right = gCameraRight * gin[0].Size;
    float3 up    = gCameraUp    * gin[0].Size;

    [unroll]
    for (int i = 0; i < 4; ++i)
    {
        GSOut o;
        float3 posW = gin[0].PosW + right * corners[i].x + up * corners[i].y;
        o.PosH    = mul(float4(posW, 1.0f), gViewProj);
        o.CenterW = gin[0].PosW;
        o.Corner  = corners[i];
        o.Size    = gin[0].Size;
        o.Color   = gin[0].Color;
        stream.Append(o);
    }
}

struct GBufferOutput
{
    float4 Albedo   : SV_Target0;
    float4 Normal   : SV_Target1;
    float4 Position : SV_Target2;
};

// Непрозрачная частица-«шарик»: вырезаем круг и строим нормаль сферы —
// тогда deferred-освещение делает её объёмной, а сортировка не нужна (есть буфер глубины)
GBufferOutput PSParticle(GSOut pin)
{
    float r2 = dot(pin.Corner, pin.Corner);
    clip(1.0f - r2);

    float z = sqrt(1.0f - r2);
    float3 toEye = normalize(gEyePosW - pin.CenterW);
    float3 N = normalize(gCameraRight * pin.Corner.x + gCameraUp * pin.Corner.y + toEye * z);

    GBufferOutput o;
    // Лаба 8: капли — гладкий диэлектрик (roughness 0.25, metallic 0)
    o.Albedo   = float4(pin.Color.rgb, 0.25f);
    o.Normal   = float4(N, 0.0f);
    o.Position = float4(pin.CenterW + N * pin.Size, 1.0f);
    return o;
}
