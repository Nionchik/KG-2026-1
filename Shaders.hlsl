cbuffer GeometryConstants : register(b0)
{
    float4x4 gWorld;
    float4x4 gView;
    float4x4 gProj;
    float4x4 gWorldInvTranspose;
    float4 gLightDir;
    float4 gLightColor;
    float4 gAmbientColor;
    float4 gEyePos;
    float4 gMaterialDiffuse;
    float4 gMaterialSpecular;
    float gSpecularPower;
    float gTotalTime;
    float gTexTilingX;
    float gTexTilingY;
    float gTexScrollX;
    float gTexScrollY;
    int gHasTexture;
    float3 gGeometryPadding;
};

Texture2D gDiffuseMap : register(t0);
SamplerState gMaterialSampler : register(s0);

struct GeometryVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
};

struct GeometryPixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
};

struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

GeometryPixelInput GeometryVS(GeometryVertexInput input)
{
    GeometryPixelInput output;
    float4 worldPosition = mul(float4(input.position, 1.0f), gWorld);
    output.worldPosition = worldPosition.xyz;
    output.position = mul(mul(worldPosition, gView), gProj);
    output.normal = normalize(mul(input.normal, (float3x3)gWorldInvTranspose));
    output.texCoord = input.texCoord * float2(gTexTilingX, gTexTilingY)
        + float2(gTexScrollX, gTexScrollY) * gTotalTime;
    return output;
}

GBufferOutput GeometryPS(GeometryPixelInput input)
{
    GBufferOutput output;
    float4 textureColor = gHasTexture
        ? gDiffuseMap.Sample(gMaterialSampler, input.texCoord)
        : float4(1.0f, 1.0f, 1.0f, 1.0f);
    output.albedo = textureColor * gMaterialDiffuse;
    output.normal = float4(normalize(input.normal), max(gSpecularPower, 1.0f));
    output.position = float4(input.worldPosition, 1.0f);
    return output;
}
// Геометрический проход сохраняет цвет, нормаль и позицию в три текстуры GBuffer.

struct PointLight
{
    float4 positionAndRange;
    float4 colorAndIntensity;
};

cbuffer LightingConstants : register(b0)
{
    float4 gLightingEyePosition;
    float4 gDirectionalDirection;
    float4 gDirectionalColorAndIntensity;
    float4 gSpotPositionAndRange;
    float4 gSpotDirectionAndInnerCone;
    float4 gSpotColorAndOuterCone;
    float4 gLightingAmbientColor;
    PointLight gPointLights[8];
    float4 gPointLightInfo;
};

Texture2D gAlbedoBuffer : register(t0);
Texture2D gNormalBuffer : register(t1);
Texture2D gPositionBuffer : register(t2);
SamplerState gGBufferSampler : register(s0);

struct LightingPixelInput
{
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD;
};

LightingPixelInput LightingVS(uint vertexId : SV_VertexID)
{
    LightingPixelInput output;
    float2 texCoord = float2((vertexId << 1) & 2, vertexId & 2);
    output.texCoord = texCoord;
    output.position = float4(texCoord * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    return output;
}

float3 EvaluateLight(
    float3 normal,
    float3 viewDirection,
    float3 lightDirection,
    float3 lightColor,
    float intensity,
    float attenuation,
    float specularPower)
{
    float diffuseFactor = max(dot(normal, lightDirection), 0.0f);
    float3 halfVector = normalize(lightDirection + viewDirection);
    float specularFactor = pow(max(dot(normal, halfVector), 0.0f), specularPower);
    return (diffuseFactor + 0.18f * specularFactor) * lightColor * intensity * attenuation;
}

float4 LightingPS(LightingPixelInput input) : SV_Target
{
    float4 albedo = gAlbedoBuffer.Sample(gGBufferSampler, input.texCoord);
    float4 positionSample = gPositionBuffer.Sample(gGBufferSampler, input.texCoord);
    if (positionSample.w < 0.5f)
        return albedo;

    float4 normalSample = gNormalBuffer.Sample(gGBufferSampler, input.texCoord);
    float3 normal = normalize(normalSample.xyz);
    float3 worldPosition = positionSample.xyz;
    float3 viewDirection = normalize(gLightingEyePosition.xyz - worldPosition);
    float specularPower = max(normalSample.w, 1.0f);

    float3 illumination = gLightingAmbientColor.rgb;
    illumination += EvaluateLight(
        normal,
        viewDirection,
        normalize(-gDirectionalDirection.xyz),
        gDirectionalColorAndIntensity.rgb,
        gDirectionalColorAndIntensity.w,
        1.0f,
        specularPower);
    // Направленное освещение рассчитывается для каждого пикселя экрана.

    int pointLightCount = min((int)gPointLightInfo.x, 8);
    for (int lightIndex = 0; lightIndex < pointLightCount; ++lightIndex)
    {
        float3 toLight = gPointLights[lightIndex].positionAndRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        float range = gPointLights[lightIndex].positionAndRange.w;
        float attenuation = saturate(1.0f - distanceToLight / range);
        attenuation = attenuation * (2.0f - attenuation);
        illumination += EvaluateLight(
            normal,
            viewDirection,
            toLight / max(distanceToLight, 0.0001f),
            gPointLights[lightIndex].colorAndIntensity.rgb,
            gPointLights[lightIndex].colorAndIntensity.w,
            attenuation,
            specularPower);
    }
    // Цикл добавляет вклад всех точечных источников, находящихся внутри Sponza.

    float3 toSpot = gSpotPositionAndRange.xyz - worldPosition;
    float spotDistance = length(toSpot);
    float3 surfaceToSpot = toSpot / max(spotDistance, 0.0001f);
    float3 spotToSurface = -surfaceToSpot;
    float coneCosine = dot(normalize(gSpotDirectionAndInnerCone.xyz), spotToSurface);
    float coneAttenuation = saturate(
        (coneCosine - gSpotColorAndOuterCone.w)
        / max(gSpotDirectionAndInnerCone.w - gSpotColorAndOuterCone.w, 0.0001f));
    float distanceAttenuation = saturate(1.0f - spotDistance / gSpotPositionAndRange.w);
    illumination += EvaluateLight(
        normal,
        viewDirection,
        surfaceToSpot,
        gSpotColorAndOuterCone.rgb,
        5.0f,
        coneAttenuation * distanceAttenuation * distanceAttenuation,
        specularPower);
    // Прожектор Spot учитывает дальность и положение пикселя внутри светового конуса.

    float3 hdrColor = albedo.rgb * illumination;
    float3 mappedColor = hdrColor / (hdrColor + 1.0f);
    mappedColor = pow(saturate(mappedColor), 1.0f / 2.2f);
    return float4(mappedColor, 1.0f);
}
