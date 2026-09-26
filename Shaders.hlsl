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

float4 ModelPS(ModelOutput input) : SV_TARGET
{
    float3 albedo = gAlbedo.Sample(gMaterialSampler, input.uv).rgb;
    float metallic = saturate(gMetallic.Sample(gMaterialSampler, input.uv).r);
    float roughness = clamp(gRoughness.Sample(gMaterialSampler, input.uv).r, 0.045f, 1.0f);
    float3 N = MaterialNormal(input);
    float3 V = normalize(gCameraPosition.xyz - input.worldPosition);
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
    // PBR Cook–Torrance: GGX, геометрическое затенение и Френель; диффузный вклад уменьшается у металлов.

    if (gOptions.x > 0.5f)
    {
        float3 ambientF = F0 + (max(float3(1.0f-roughness,1.0f-roughness,1.0f-roughness), F0) - F0)
                              * pow(1.0f - saturate(NdotV), 5.0f);
        float3 ambientKD = (1.0f - ambientF) * (1.0f - metallic);
        float3 diffuseIBL = gIrradiance.Sample(gEnvironmentSampler, N).rgb * albedo;
        // Готовая irradiance map даёт диффузное освещение от окружения.

        float3 R = reflect(-V, N);
        float3 reflected = gPrefiltered.SampleLevel(gEnvironmentSampler, R, roughness * gOptions.y).rgb;
        // Готовая pre-filtered environment map: шероховатость выбирает уровень размытия отражения.

        float2 brdf = gIntegration.SampleLevel(gEnvironmentSampler, float2(saturate(NdotV),1.0f-roughness),0).rg;
        float3 specularIBL = reflected * (ambientF * brdf.x + brdf.y);
        // BRDF integration map использует N·V и 1−roughness; результат объединяется с отражениями (split-sum).

        color += ambientKD * diffuseIBL + specularIBL;
        // IBL добавляет диффузное освещение и зеркальные отражения окружения к прямому свету.
    }
    return float4(ToneMap(color), 1.0f);
}

struct SkyOutput
{
    float4 position : SV_POSITION;
    float2 ndc : TEXCOORD;
};

SkyOutput SkyVS(uint vertexId : SV_VertexID)
{
    SkyOutput output;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.ndc = uv * 2.0f - 1.0f;
    output.position = float4(output.ndc, 1.0f, 1.0f);
    return output;
}

float4 SkyPS(SkyOutput input) : SV_TARGET
{
    float4 world = mul(float4(input.ndc, 1.0f, 1.0f), gInverseViewProjection);
    float3 direction = normalize(world.xyz / world.w - gCameraPosition.xyz);
    return float4(ToneMap(gPrefiltered.SampleLevel(gEnvironmentSampler, direction, 0).rgb), 1.0f);
}
