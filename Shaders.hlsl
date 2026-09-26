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
    float angle = Random01(seed) * 6.2831853f;
    float horizontalSpeed = lerp(0.6f, 2.2f, Random01(seed + 1u));
    particle.position = gEmitterPosition + float3(0.0f, 0.15f, 0.0f);
    particle.velocity = float3(cos(angle) * horizontalSpeed,
                               lerp(7.0f, 12.0f, Random01(seed + 2u)),
                               sin(angle) * horizontalSpeed);
    particle.age = 0.0f;
    particle.lifetime = lerp(1.6f, 3.4f, Random01(seed + 3u));
    particle.size = lerp(0.08f, 0.18f, Random01(seed + 4u));
    particle.color = lerp(float4(0.10f, 0.45f, 1.0f, 1.0f),
                          float4(0.65f, 0.90f, 1.0f, 1.0f),
                          Random01(seed + 5u));
    particle.seed = seed;
}

[numthreads(256, 1, 1)]
void ParticleCS(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (dispatchThreadId.x >= gParticleCount)
        return;

    Particle particle = gInputParticles.Consume();
    particle.age += gDeltaTime;
    particle.velocity += float3(0.0f, -9.81f, 0.0f) * gDeltaTime;
    particle.position += particle.velocity * gDeltaTime;

    if (particle.age >= particle.lifetime || particle.position.y < gEmitterPosition.y)
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
    float3(-2.5f, -0.6f, -2.5f),
    float3(-2.5f,  0.0f, -2.5f),
    float3( 2.5f,  0.0f, -2.5f),
    float3( 2.5f, -0.6f, -2.5f),
    float3(-2.5f, -0.6f,  2.5f),
    float3(-2.5f,  0.0f,  2.5f),
    float3( 2.5f,  0.0f,  2.5f),
    float3( 2.5f, -0.6f,  2.5f)
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
