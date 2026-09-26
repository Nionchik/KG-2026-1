cbuffer DrawConstants : register(b0)
{
    float4x4 gWorld;
    float4x4 gViewProjection;
    float4 gColor;
};

cbuffer ShadowConstants : register(b1)
{
    float4x4 gLightViewProjection[4];
    float4 gCascadeSplits;
    float4 gLightDirection;
    float4 gCameraPosition;
    float4 gCameraForward;
    float4 gShadowMapSize;
};

Texture2DArray<float> gShadowMap : register(t0);
SamplerComparisonState gShadowSampler : register(s0);

struct VertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct MainPixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 normal : NORMAL;
};

struct ShadowVertexOutput
{
    float4 position : SV_POSITION;
};

MainPixelInput MainVS(VertexInput input)
{
    MainPixelInput output;
    float4 worldPosition = mul(float4(input.position, 1.0f), gWorld);
    output.position = mul(worldPosition, gViewProjection);
    output.worldPosition = worldPosition.xyz;
    output.normal = normalize(mul(input.normal, (float3x3)gWorld));
    return output;
}

ShadowVertexOutput ShadowVS(VertexInput input)
{
    ShadowVertexOutput output;
    float4 worldPosition = mul(float4(input.position, 1.0f), gWorld);
    output.position = mul(worldPosition, gViewProjection);
    return output;
}

float CalculateShadow(float3 worldPosition, float3 normal, uint cascadeIndex)
{
    float4 shadowPosition = mul(float4(worldPosition, 1.0f), gLightViewProjection[cascadeIndex]);
    shadowPosition.xyz /= shadowPosition.w;
    float2 shadowUv = float2(shadowPosition.x * 0.5f + 0.5f, -shadowPosition.y * 0.5f + 0.5f);
    if (shadowUv.x <= 0.0f || shadowUv.x >= 1.0f || shadowUv.y <= 0.0f || shadowUv.y >= 1.0f
        || shadowPosition.z <= 0.0f || shadowPosition.z >= 1.0f)
        return 1.0f;

    float normalBias = 0.0008f + 0.0025f * (1.0f - saturate(dot(normal, normalize(-gLightDirection.xyz))));
    float2 texelSize = gShadowMapSize.zw;
    float shadow = 0.0f;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            shadow += gShadowMap.SampleCmpLevelZero(
                gShadowSampler,
                float3(shadowUv + float2(x, y) * texelSize, cascadeIndex),
                shadowPosition.z - normalBias);
        }
    }
    return shadow / 9.0f;
}

// PCF усредняет девять сравнений глубины вокруг текущего пикселя тени.

float4 MainPS(MainPixelInput input) : SV_TARGET
{
    float viewDepth = dot(input.worldPosition - gCameraPosition.xyz, normalize(gCameraForward.xyz));
    uint cascadeIndex = 0;
    cascadeIndex += viewDepth > gCascadeSplits.x;
    cascadeIndex += viewDepth > gCascadeSplits.y;
    cascadeIndex += viewDepth > gCascadeSplits.z;
    cascadeIndex = min(cascadeIndex, 3u);
    // Каскад выбирается по глубине пикселя вдоль направления камеры.

    float3 normal = normalize(input.normal);
    float diffuse = saturate(dot(normal, normalize(-gLightDirection.xyz)));
    float shadow = CalculateShadow(input.worldPosition, normal, cascadeIndex);
    float lighting = 0.2f + diffuse * shadow * 0.8f;
    return float4(gColor.rgb * lighting, 1.0f);
}
