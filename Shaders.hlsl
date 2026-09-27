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
    float gGeometryPadding0;
    float2 gGeometryPadding12;
    float gTessellationMin;
    float gTessellationMax;
    float gTessellationNear;
    float gTessellationFar;
    float gDisplacementScale;
    float gIsWater;
    float gTessellationPadding0;
    float2 gTessellationPadding12;
};

Texture2D gDiffuseMap : register(t0);
Texture2D gNormalMap : register(t1);
Texture2D gDisplacementMap : register(t2);
SamplerState gMaterialSampler : register(s0);

struct GeometryVertexInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
};

struct TessellationControlPoint
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
};

struct TessellationConstants
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

struct GeometryPixelInput
{
    float4 position : SV_POSITION;
    float3 worldPosition : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
};

struct GBufferOutput
{
    float4 albedo : SV_Target0;
    float4 normal : SV_Target1;
    float4 position : SV_Target2;
};

TessellationControlPoint GeometryVS(GeometryVertexInput input)
{
    TessellationControlPoint output;
    output.position = input.position;
    output.normal = input.normal;
    output.texCoord = input.texCoord * float2(gTexTilingX, gTexTilingY);
    output.tangent = input.tangent;
    return output;
}

float CalculateTessellationFactor(float3 objectPosition)
{
    float3 worldPosition = mul(float4(objectPosition, 1.0f), gWorld).xyz;
    float distanceToCamera = distance(worldPosition, gEyePos.xyz);
    float distanceFactor = saturate(
        (distanceToCamera - gTessellationNear)
        / max(gTessellationFar - gTessellationNear, 0.0001f));
    float factor = lerp(gTessellationMax, gTessellationMin, distanceFactor);
    if (factor >= 24.0f) return 32.0f;
    if (factor >= 12.0f) return 16.0f;
    if (factor >= 6.0f) return 8.0f;
    if (factor >= 3.0f) return 4.0f;
    if (factor >= 1.5f) return 2.0f;
    return 1.0f;
}

// вода: value noise и fBm вместо обычных синусоид.
// движение достигается сдвигом координат каждого шумового слоя во времени.
float Hash21(float2 value)
{
    value = frac(value * float2(0.1031f, 0.11369f));
    value += dot(value, value.yx + 19.19f);
    return frac((value.x + value.y) * value.x);
}

float ValueNoise(float2 position)
{
    const float2 cell = floor(position);
    float2 local = frac(position);
    local = local * local * (3.0f - 2.0f * local);
    const float a = Hash21(cell);
    const float b = Hash21(cell + float2(1.0f, 0.0f));
    const float c = Hash21(cell + float2(0.0f, 1.0f));
    const float d = Hash21(cell + float2(1.0f, 1.0f));
    return lerp(lerp(a, b, local.x), lerp(c, d, local.x), local.y);
}

float FractalWaterNoise(float2 position, float time)
{
    float result = 0.0f;
    float amplitude = 0.56f;
    float2 p = position;
    [unroll]
    for (int octave = 0; octave < 5; ++octave)
    {
        const float speed = 0.225f + 0.117f * octave;
        const float2 flow = float2(0.83f + octave * 0.17f, -0.51f + octave * 0.11f);
        result += (ValueNoise(p + flow * time * speed) * 2.0f - 1.0f) * amplitude;
        p = p * 2.07f + float2(11.7f, 5.3f);
        amplitude *= 0.5f;
    }
    return result;
}

float WaterHeight(float2 position, float time)
{
    const float largeWaves = FractalWaterNoise(position * 0.052f, time) * 2.15f;
    const float smallRipples = FractalWaterNoise(position * 0.165f + 41.0f, time * 1.65f) * 0.42f;
    return (largeWaves + smallRipples) * gDisplacementScale;
}

void ApplyWaterWaves(inout float3 position, inout float3 normal)
{
    const float epsilon = 0.10f;
    const float height = WaterHeight(position.xz, gTotalTime);
    const float heightX = WaterHeight(position.xz + float2(epsilon, 0.0f), gTotalTime);
    const float heightZ = WaterHeight(position.xz + float2(0.0f, epsilon), gTotalTime);
    position.y += height;
    normal = normalize(float3(-(heightX - height) / epsilon, 1.0f, -(heightZ - height) / epsilon));
}
// Уровень тесселяции дискретно уменьшается при удалении участка модели от камеры.

TessellationConstants PatchConstants(InputPatch<TessellationControlPoint, 3> patch)
{
    TessellationConstants output;
    output.edges[0] = CalculateTessellationFactor((patch[1].position + patch[2].position) * 0.5f);
    output.edges[1] = CalculateTessellationFactor((patch[2].position + patch[0].position) * 0.5f);
    output.edges[2] = CalculateTessellationFactor((patch[0].position + patch[1].position) * 0.5f);
    output.inside = (output.edges[0] + output.edges[1] + output.edges[2]) / 3.0f;
    return output;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("PatchConstants")]
[maxtessfactor(32.0f)]
TessellationControlPoint GeometryHS(
    InputPatch<TessellationControlPoint, 3> patch,
    uint controlPointId : SV_OutputControlPointID)
{
    return patch[controlPointId];
}
// Hull Shader передаёт контрольные точки и рассчитанные коэффициенты тесселяции.

[domain("tri")]
GeometryPixelInput GeometryDS(
    TessellationConstants constants,
    float3 barycentricCoordinates : SV_DomainLocation,
    const OutputPatch<TessellationControlPoint, 3> patch)
{
    GeometryPixelInput output;
    float3 objectPosition =
        patch[0].position * barycentricCoordinates.x
        + patch[1].position * barycentricCoordinates.y
        + patch[2].position * barycentricCoordinates.z;
    float3 objectNormal = normalize(
        patch[0].normal * barycentricCoordinates.x
        + patch[1].normal * barycentricCoordinates.y
        + patch[2].normal * barycentricCoordinates.z);
    float4 objectTangent =
        patch[0].tangent * barycentricCoordinates.x
        + patch[1].tangent * barycentricCoordinates.y
        + patch[2].tangent * barycentricCoordinates.z;
    output.texCoord =
        patch[0].texCoord * barycentricCoordinates.x
        + patch[1].texCoord * barycentricCoordinates.y
        + patch[2].texCoord * barycentricCoordinates.z;
    if (gIsWater > 0.5f)
    {
        ApplyWaterWaves(objectPosition, objectNormal);
        output.texCoord = objectPosition.xz * 0.035f;
    }
    else
    {
        float height = gDisplacementMap.SampleLevel(gMaterialSampler, output.texCoord, 0).r;
        objectPosition += objectNormal * ((height - 0.5f) * gDisplacementScale);
    }
    // Domain Shader смещает вершины либо по карте высот, либо по процедуре волн.
    float4 worldPosition = mul(float4(objectPosition, 1.0f), gWorld);
    output.worldPosition = worldPosition.xyz;
    output.position = mul(mul(worldPosition, gView), gProj);
    output.normal = normalize(mul(objectNormal, (float3x3)gWorldInvTranspose));
    output.tangent.xyz = normalize(mul(objectTangent.xyz, (float3x3)gWorld));
    output.tangent.w = objectTangent.w;
    return output;
}

GeometryPixelInput FlatGeometryVS(GeometryVertexInput input)
{
    GeometryPixelInput output;
    float3 objectPosition = input.position;
    float3 objectNormal = input.normal;
    if (gIsWater > 0.5f)
        ApplyWaterWaves(objectPosition, objectNormal);
    float4 worldPosition = mul(float4(objectPosition, 1.0f), gWorld);
    output.worldPosition = worldPosition.xyz;
    output.position = mul(mul(worldPosition, gView), gProj);
    output.normal = normalize(mul(objectNormal, (float3x3)gWorldInvTranspose));
    output.tangent.xyz = normalize(mul(input.tangent.xyz, (float3x3)gWorld));
    output.tangent.w = input.tangent.w;
    output.texCoord = input.texCoord * float2(gTexTilingX, gTexTilingY)
        + float2(gTexScrollX, gTexScrollY) * gTotalTime;
    return output;
}
// При выключенной тесселяции вершины проходят обычный путь без Hull и Domain Shader.

GBufferOutput GeometryPS(GeometryPixelInput input)
{
    GBufferOutput output;
    if (gIsWater > 0.5f)
    {
        float3 normal = normalize(input.normal);
        float3 viewDirection = normalize(gEyePos.xyz - input.worldPosition);
        float fresnel = pow(1.0f - saturate(dot(normal, viewDirection)), 5.0f);
        float3 deepWater = float3(0.005f, 0.06f, 0.16f);
        float3 shallowWater = float3(0.015f, 0.34f, 0.56f);
        const float2 waterUv = input.texCoord + float2(0.018f, -0.011f) * gTotalTime;
        const float3 oceanTexture = gDiffuseMap.Sample(gMaterialSampler, waterUv).rgb;
        const float3 waterColor = lerp(deepWater, shallowWater, 0.42f + fresnel * 0.58f);
        output.albedo = float4(lerp(waterColor, oceanTexture, 0.68f), 1.0f);
        output.normal = float4(normal, 180.0f);
        output.position = float4(input.worldPosition, 1.0f);
        return output;
    }
    float4 textureColor = gDiffuseMap.Sample(gMaterialSampler, input.texCoord);
    float3 tangentNormal = gNormalMap.Sample(gMaterialSampler, input.texCoord).xyz * 2.0f - 1.0f;
    float3 normal = normalize(input.normal);
    float3 tangent = normalize(input.tangent.xyz - normal * dot(input.tangent.xyz, normal));
    float3 bitangent = normalize(cross(normal, tangent)) * input.tangent.w;
    float3 worldNormal = normalize(mul(tangentNormal, float3x3(tangent, bitangent, normal)));
    // Normal-карта преобразуется из касательного пространства в мировое.
    output.albedo = textureColor * gMaterialDiffuse;
    output.normal = float4(worldNormal, max(gSpecularPower, 1.0f));
    output.position = float4(input.worldPosition, 1.0f);
    return output;
}

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
    illumination += EvaluateLight(normal, viewDirection, normalize(-gDirectionalDirection.xyz),
        gDirectionalColorAndIntensity.rgb, gDirectionalColorAndIntensity.w, 1.0f, specularPower);
    int pointLightCount = min((int)gPointLightInfo.x, 8);
    for (int lightIndex = 0; lightIndex < pointLightCount; ++lightIndex)
    {
        float3 toLight = gPointLights[lightIndex].positionAndRange.xyz - worldPosition;
        float distanceToLight = length(toLight);
        float range = gPointLights[lightIndex].positionAndRange.w;
        float attenuation = saturate(1.0f - distanceToLight / range);
        attenuation = attenuation * (2.0f - attenuation);
        illumination += EvaluateLight(normal, viewDirection,
            toLight / max(distanceToLight, 0.0001f),
            gPointLights[lightIndex].colorAndIntensity.rgb,
            gPointLights[lightIndex].colorAndIntensity.w,
            attenuation, specularPower);
    }
    float3 toSpot = gSpotPositionAndRange.xyz - worldPosition;
    float spotDistance = length(toSpot);
    float3 surfaceToSpot = toSpot / max(spotDistance, 0.0001f);
    float3 spotToSurface = -surfaceToSpot;
    float coneCosine = dot(normalize(gSpotDirectionAndInnerCone.xyz), spotToSurface);
    float coneAttenuation = saturate((coneCosine - gSpotColorAndOuterCone.w)
        / max(gSpotDirectionAndInnerCone.w - gSpotColorAndOuterCone.w, 0.0001f));
    float distanceAttenuation = saturate(1.0f - spotDistance / gSpotPositionAndRange.w);
    illumination += EvaluateLight(normal, viewDirection, surfaceToSpot,
        gSpotColorAndOuterCone.rgb, 1.8f,
        coneAttenuation * distanceAttenuation, specularPower);
    float3 hdrColor = albedo.rgb * illumination;
    float3 mappedColor = hdrColor / (hdrColor + 1.0f);
    mappedColor = pow(saturate(mappedColor), 1.0f / 2.2f);
    return float4(mappedColor, 1.0f);
}
