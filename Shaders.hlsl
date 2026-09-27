struct Particle
{
    float3 position;
    float age;
    float3 velocity;
    float lifetime;
    float4 color;
    float size;
    uint seed;
    float2 padding;
};

cbuffer ComputeConstants : register(b0)
{
    float gDeltaTime;
    float gTotalTime;
    uint gParticleCount;
    float gComputePadding0;
    float3 gEmitterPosition;
    float gComputePadding1;
    float3 gSphereCenter;
    float gSphereRadius;
};

ConsumeStructuredBuffer<Particle> gInputParticles : register(u0);
AppendStructuredBuffer<Particle> gOutputParticles : register(u1);

float Random01(uint value)
{
    value ^= value >> 16;
    value *= 0x7feb352d;
    value ^= value >> 15;
    value *= 0x846ca68b;
    value ^= value >> 16;
    return (value & 0x00ffffff) / 16777216.0f;
}

void ResetParticle(inout Particle particle, uint threadIndex)
{
    uint seed = particle.seed + threadIndex * 747796405u + asuint(gTotalTime * 1000.0f);
    particle.position = gEmitterPosition + float3(
        (Random01(seed + 1u) - 0.5f) * 3.6f, 0.0f,
        (Random01(seed + 2u) - 0.5f) * 3.6f);
    particle.velocity = float3(
        (Random01(seed + 3u) - 0.5f) * 2.2f,
        -lerp(1.5f, 3.0f, Random01(seed + 4u)),
        (Random01(seed + 5u) - 0.5f) * 2.2f);
    particle.age = 0.0f;
    particle.lifetime = lerp(8.0f, 12.0f, Random01(seed + 6u));
    particle.size = lerp(0.045f, 0.10f, Random01(seed + 7u));
    particle.color = lerp(float4(0.10f, 0.45f, 1.0f, 1.0f),
                          float4(0.65f, 0.90f, 1.0f, 1.0f),
                          Random01(seed + 8u));
    particle.seed = seed;
    // padding.x хранит состояние: 0 — капля летит, 1 — скользит по сфере.
    particle.padding = float2(0.0f, 0.0f);
}

[numthreads(256, 1, 1)]
void ParticleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParticleCount)
        return;

    Particle particle = gInputParticles.Consume();
    particle.age += gDeltaTime;
    const float3 gravity = float3(0.0f, -9.81f, 0.0f);
    const float collisionRadius = gSphereRadius + particle.size * 0.55f;
    if (particle.padding.x > 0.5f)
    {
        // Капля уже коснулась сферы: оставляем только движение по касательной.
        // После каждого шага возвращаем её на поверхность, чтобы она не проходила сквозь неё.
        float3 fromCenter = particle.position - gSphereCenter;
        float distanceToCenter = length(fromCenter);
        float3 normal = distanceToCenter > 0.0001f ? fromCenter / distanceToCenter : float3(0.0f, 1.0f, 0.0f);
        float3 tangentGravity = gravity - normal * dot(gravity, normal);
        float3 tangentVelocity = particle.velocity - normal * dot(particle.velocity, normal);

        // Небольшой поток воды тянет каплю от верхней точки вниз по выбранному
        // меридиану. Без него около вершины касательная гравитации почти равна нулю.
        float horizontalLength = length(normal.xz);
        float3 outward = horizontalLength > 0.0001f
            ? float3(normal.x / horizontalLength, 0.0f, normal.z / horizontalLength)
            : float3(1.0f, 0.0f, 0.0f);
        float3 flowDirection = normalize(float3(outward.x * normal.y, -horizontalLength, outward.z * normal.y));

        particle.velocity = (tangentVelocity + (tangentGravity + flowDirection * 4.5f) * gDeltaTime) * 0.994f;
        particle.position += particle.velocity * gDeltaTime;

        fromCenter = particle.position - gSphereCenter;
        distanceToCenter = length(fromCenter);
        normal = distanceToCenter > 0.0001f ? fromCenter / distanceToCenter : float3(0.0f, 1.0f, 0.0f);
        particle.position = gSphereCenter + normal * collisionRadius;

        // В нижней части сферы поверхность уже почти горизонтальна: капля отрывается
        // и дальше летит свободно вниз, как струйка воды с нижнего края.
        if (normal.y < -0.48f)
        {
            particle.padding.x = 0.0f;
            particle.velocity += float3(0.0f, -1.8f, 0.0f);
        }
    }
    else
    {
        particle.velocity += gravity * gDeltaTime;
        particle.position += particle.velocity * gDeltaTime;

        float3 fromCenter = particle.position - gSphereCenter;
        float distanceToCenter = length(fromCenter);
        if (distanceToCenter < collisionRadius)
        {
            float3 normal = distanceToCenter > 0.0001f ? fromCenter / distanceToCenter : float3(0.0f, 1.0f, 0.0f);
            particle.position = gSphereCenter + normal * collisionRadius;

            // Убираем скорость, направленную в поверхность, и переводим каплю
            // в режим скольжения. Дальше её ведёт касательная составляющая тяжести.
            particle.velocity -= normal * dot(particle.velocity, normal);
            particle.velocity *= 0.82f;
            particle.padding.x = 1.0f;
        }
    }

    // Верх платформы находится на y = 0. Капля исчезает до контакта с ней:
    // альфа плавно падает на последних 0.75 единицах высоты.
    const float platformTop = 0.0f;
    const float fadeDistance = 0.75f;
    particle.color.a = saturate((particle.position.y - platformTop - particle.size) / fadeDistance);

    if (particle.age >= particle.lifetime || particle.position.y <= platformTop + particle.size)
        ResetParticle(particle, dispatchThreadId.x);

    gOutputParticles.Append(particle);
}
// Compute Shader обновляет частицы и переносит их из Consume-буфера в Append-буфер.

cbuffer RenderConstants : register(b0)
{
    float4x4 gViewProjection;
    float4 gCameraRight;
    float4 gCameraUp;
};

StructuredBuffer<Particle> gParticles : register(t0);

struct VertexOutput
{
    float3 worldPosition : POSITION;
    float size : SIZE;
    float4 color : COLOR;
};

struct GeometryOutput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

VertexOutput ParticleVS(uint vertexId : SV_VertexID)
{
    Particle particle = gParticles[vertexId];
    VertexOutput output;
    output.worldPosition = particle.position;
    output.size = particle.size;
    output.color = particle.color;
    return output;
}

[maxvertexcount(4)]
void ParticleGS(point VertexOutput input[1], inout TriangleStream<GeometryOutput> stream)
{
    float3 right = gCameraRight.xyz * input[0].size;
    float3 up = gCameraUp.xyz * input[0].size;
    float3 corners[4] = {
        input[0].worldPosition - right - up,
        input[0].worldPosition - right + up,
        input[0].worldPosition + right - up,
        input[0].worldPosition + right + up
    };

    GeometryOutput output;
    output.color = input[0].color;
    output.position = mul(float4(corners[0], 1.0f), gViewProjection);
    stream.Append(output);
    output.position = mul(float4(corners[1], 1.0f), gViewProjection);
    stream.Append(output);
    output.position = mul(float4(corners[2], 1.0f), gViewProjection);
    stream.Append(output);
    output.position = mul(float4(corners[3], 1.0f), gViewProjection);
    stream.Append(output);
}
// Geometry Shader разворачивает каждую точку в билборд из двух треугольников.

float4 ParticlePS(GeometryOutput input) : SV_TARGET
{
    return input.color;
}

static const float3 PlatformVertices[8] = {
    float3(-3.75f, -0.6f, -3.75f),
    float3(-3.75f,  0.0f, -3.75f),
    float3( 3.75f,  0.0f, -3.75f),
    float3( 3.75f, -0.6f, -3.75f),
    float3(-3.75f, -0.6f,  3.75f),
    float3(-3.75f,  0.0f,  3.75f),
    float3( 3.75f,  0.0f,  3.75f),
    float3( 3.75f, -0.6f,  3.75f)
};

static const uint PlatformIndices[36] = {
    1, 5, 6, 1, 6, 2,
    0, 3, 7, 0, 7, 4,
    0, 1, 2, 0, 2, 3,
    3, 2, 6, 3, 6, 7,
    7, 6, 5, 7, 5, 4,
    4, 5, 1, 4, 1, 0
};

static const float4 PlatformColors[6] = {
    float4(0.22f, 0.30f, 0.42f, 1.0f),
    float4(0.09f, 0.13f, 0.19f, 1.0f),
    float4(0.14f, 0.20f, 0.29f, 1.0f),
    float4(0.12f, 0.17f, 0.25f, 1.0f),
    float4(0.10f, 0.15f, 0.22f, 1.0f),
    float4(0.16f, 0.22f, 0.31f, 1.0f)
};

struct PlatformOutput
{
    float4 position : SV_POSITION;
    float4 color : COLOR;
};

PlatformOutput PlatformVS(uint vertexId : SV_VertexID)
{
    PlatformOutput output;
    float3 position = PlatformVertices[PlatformIndices[vertexId]] + float3(0.0f, 0.0f, 20.0f);
    output.position = mul(float4(position, 1.0f), gViewProjection);
    output.color = PlatformColors[vertexId / 6];
    return output;
}

float4 PlatformPS(PlatformOutput input) : SV_TARGET
{
    return input.color;
}

// Сфера строится процедурно по SV_VertexID: 24 кольца, 32 сегмента.
// Отдельный mesh buffer для неё не требуется.
struct SphereOutput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
};

SphereOutput SphereVS(uint vertexId : SV_VertexID)
{
    const uint rings = 24u;
    const uint segments = 32u;
    const uint triangleIndex = vertexId / 6u;
    const uint corner = vertexId % 6u;
    const uint ring = triangleIndex / segments;
    const uint segment = triangleIndex % segments;
    const uint cornerOffsets[6] = {0u, 1u, 2u, 0u, 2u, 3u};
    const uint localCorner = cornerOffsets[corner];
    const uint dx = localCorner == 1u || localCorner == 2u ? 1u : 0u;
    const uint dy = localCorner >= 2u ? 1u : 0u;
    const float u = (segment + dx) / (float)segments;
    const float v = (ring + dy) / (float)rings;
    const float phi = v * 3.14159265f;
    const float theta = u * 6.2831853f;
    const float3 normal = float3(sin(phi) * cos(theta), cos(phi), sin(phi) * sin(theta));
    const float3 position = float3(0.0f, 3.0f, 20.0f) + normal * 3.0f;
    SphereOutput output;
    output.position = mul(float4(position, 1.0f), gViewProjection);
    output.normal = normal;
    return output;
}

float4 SpherePS(SphereOutput input) : SV_TARGET
{
    const float light = saturate(dot(normalize(input.normal), normalize(float3(-0.35f, 0.8f, -0.45f))));
    const float3 baseColor = float3(0.12f, 0.26f, 0.40f);
    return float4(baseColor * (0.25f + light * 0.75f), 1.0f);
}
