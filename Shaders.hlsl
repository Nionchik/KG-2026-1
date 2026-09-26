cbuffer SceneConstants : register(b0)
{
    float4x4 gViewProjection;
    float4x4 gInverseViewProjection;
    float4 gCameraPosition;
    float4 gLightDirection;
    float4 gLightColor;
    float4 gOptions;
};

Texture2D gAlbedo : register(t0);
Texture2D gNormal : register(t1);
Texture2D gMetallic : register(t2);
Texture2D gRoughness : register(t3);
TextureCube gIrradiance : register(t4);
Texture2D gIntegration : register(t5);
TextureCube gPrefiltered : register(t6);
SamplerState gMaterialSampler : register(s0);
SamplerState gEnvironmentSampler : register(s1);

Texture2D<float4> gBufferAlbedoMetallic : register(t7);
Texture2D<float4> gBufferNormalRoughness : register(t8);
Texture2D<float4> gBufferPosition : register(t9);
Texture2D<float4> gSceneHDR : register(t10);

static const float PI = 3.14159265359f;

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

struct ModelOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
};

ModelOutput ModelVS(VertexInput input)
{
    ModelOutput output;
    output.position = mul(float4(input.position, 1.0f), gViewProjection);
    output.worldPosition = input.position;
    output.normal = input.normal;
    output.uv = input.uv;
    return output;
}

float3 MaterialNormal(ModelOutput input)
{
    float3 N = normalize(input.normal);
    float3 q1 = ddx(input.worldPosition);
    float3 q2 = ddy(input.worldPosition);
    float2 uv1 = ddx(input.uv);
    float2 uv2 = ddy(input.uv);
    float3 p1 = cross(q2, N);
    float3 p2 = cross(N, q1);
    float3 T = p1 * uv1.x + p2 * uv2.x;
    float3 B = p1 * uv1.y + p2 * uv2.y;
    float scale = rsqrt(max(max(dot(T,T), dot(B,B)), 1e-12f));
    float3 sampled = gNormal.Sample(gMaterialSampler, input.uv).xyz * 2.0f - 1.0f;
    return normalize(T * scale * sampled.x + B * scale * sampled.y + N * sampled.z);
}

float DistributionGGX(float NdotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float denominator = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / max(PI * denominator * denominator, 1e-7f);
}

float GeometrySchlickGGX(float NdotX, float roughness)
{
    float r = roughness + 1.0f;
    float k = r * r / 8.0f;
    return NdotX / max(NdotX * (1.0f - k) + k, 1e-6f);
}

float3 FresnelSchlick(float cosine, float3 F0)
{
    return F0 + (1.0f - F0) * pow(1.0f - saturate(cosine), 5.0f);
}

float3 ToneMap(float3 hdr)
{
    hdr = max(hdr, 0.0f);
    return hdr / (hdr + 1.0f);
}


struct GBufferOutput
{
    float4 albedoMetallic : SV_TARGET0;
    float4 normalRoughness : SV_TARGET1;
    float4 position : SV_TARGET2;
};

GBufferOutput GBufferPS(ModelOutput input)
{
    GBufferOutput output;
    output.albedoMetallic = float4(gAlbedo.Sample(gMaterialSampler,input.uv).rgb,
                                   saturate(gMetallic.Sample(gMaterialSampler,input.uv).r));
    output.normalRoughness = float4(MaterialNormal(input),
                                    clamp(gRoughness.Sample(gMaterialSampler,input.uv).r,0.045f,1.0f));
    output.position = float4(input.worldPosition,1.0f);
    return output;
}
// G-buffer сохраняет цвет и металличность, нормаль и шероховатость, мировую позицию и маску объекта

struct QuadOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
    float2 ndc : TEXCOORD1;
};

QuadOutput FullscreenVS(uint vertexId : SV_VertexID)
{
    QuadOutput output;
    output.uv = float2(vertexId & 1u, (vertexId >> 1u) & 1u);
    output.ndc = float2(output.uv.x*2.0f-1.0f,1.0f-output.uv.y*2.0f);
    output.position = float4(output.ndc,0.0f,1.0f);
    return output;
}
// SV_VertexID создаёт четыре вершины full-screen quad без вершинного буфера

float3 Background(float2 ndc)
{
    float4 world = mul(float4(ndc,1.0f,1.0f),gInverseViewProjection);
    float3 direction = normalize(world.xyz/world.w-gCameraPosition.xyz);
    return gPrefiltered.SampleLevel(gEnvironmentSampler,direction,0).rgb;
}

float4 LightingPS(QuadOutput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float4 position = gBufferPosition.Load(int3(pixel,0));
    if (position.w < 0.5f)
        return float4(Background(input.ndc),1.0f);
    float4 albedoMetallic = gBufferAlbedoMetallic.Load(int3(pixel,0));
    float4 normalRoughness = gBufferNormalRoughness.Load(int3(pixel,0));
    float3 albedo = albedoMetallic.rgb;
    float metallic = albedoMetallic.a;
    float roughness = normalRoughness.a;
    float3 N = normalize(normalRoughness.xyz);
    float3 worldPosition = position.xyz;
    // пиксельный шейдер принимает текстуры G-buffer и использует их для расчёта освещения
    float3 V = normalize(gCameraPosition.xyz - worldPosition);
    float3 L = normalize(gLightDirection.xyz);
    float3 H = normalize(V + L);
    float NdotV = max(dot(N, V), 0.0001f);
    float NdotL = saturate(dot(N, L));
    float3 F0 = lerp(float3(0.04f,0.04f,0.04f), albedo, metallic);
    float3 F = FresnelSchlick(dot(H,V), F0);
    float D = DistributionGGX(saturate(dot(N,H)), roughness);
    float G = GeometrySchlickGGX(NdotV, roughness) * GeometrySchlickGGX(NdotL, roughness);
    float3 specular = D * G * F / max(4.0f * NdotV * NdotL, 0.0001f);
    float3 kD = (1.0f - F) * (1.0f - metallic);
    float3 color = (kD * albedo / PI + specular) * gLightColor.rgb * NdotL;

    if (gOptions.x > 0.5f)
    {
        float3 ambientF = F0 + (max(float3(1.0f-roughness,1.0f-roughness,1.0f-roughness), F0) - F0)
                              * pow(1.0f - saturate(NdotV), 5.0f);
        float3 ambientKD = (1.0f - ambientF) * (1.0f - metallic);
        float3 diffuseIBL = gIrradiance.Sample(gEnvironmentSampler, N).rgb * albedo;

        float3 R = reflect(-V, N);
        float3 reflected = gPrefiltered.SampleLevel(gEnvironmentSampler, R, roughness * gOptions.y).rgb;

        float2 brdf = gIntegration.SampleLevel(gEnvironmentSampler, float2(saturate(NdotV),1.0f-roughness),0).rg;
        float3 specularIBL = reflected * (ambientF * brdf.x + brdf.y);

        color += ambientKD * diffuseIBL + specularIBL;
    }
    return float4(color, 1.0f);
}


float4 PostProcessPS(QuadOutput input) : SV_TARGET
{
    float2 uv = input.uv;
    float2 radial = uv-0.5f;
    float3 hdr;
    if (gOptions.w > 0.5f)
    {
        float2 offset = radial * dot(radial,radial) * 0.025f;
        hdr.r = gSceneHDR.SampleLevel(gEnvironmentSampler,uv+offset,0).r;
        hdr.g = gSceneHDR.SampleLevel(gEnvironmentSampler,uv,0).g;
        hdr.b = gSceneHDR.SampleLevel(gEnvironmentSampler,uv-offset,0).b;
        // хроматическая аберрация: красный и синий каналы читаются с разными смещениями к краям экрана
    }
    else
        hdr = gSceneHDR.SampleLevel(gEnvironmentSampler,uv,0).rgb;
    float3 color = ToneMap(hdr);
    if (gOptions.z > 0.5f)
    {
        float radius = length(radial*2.0f)/1.41421356f;
        color *= 1.0f-0.65f*smoothstep(0.25f,1.0f,radius);
        // виньетирование: яркость плавно уменьшается по мере удаления от центра изображения
    }
    return float4(color,1.0f);
}
