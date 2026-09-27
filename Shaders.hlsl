cbuffer ObjectConstants : register(b0)
{
  float4x4 gWorld;
  float4x4 gViewProjection;
  float4 gColor;
  float4 gLightDirection;
};

struct VertexInput
{
  float3 position : POSITION;
  float3 normal : NORMAL;
};

struct PixelInput
{
  float4 position : SV_POSITION;
  float3 normal : NORMAL;
};

PixelInput VSMain(VertexInput input)
{
  PixelInput output;
  float4 worldPosition = mul(float4(input.position, 1.0f), gWorld);
  output.position = mul(worldPosition, gViewProjection);
  output.normal = normalize(mul(input.normal, (float3x3)gWorld));
  return output;
}

float4 PSMain(PixelInput input) : SV_TARGET
{
  float diffuse = saturate(dot(normalize(input.normal), normalize(-gLightDirection.xyz)));
  float lighting = 0.18f + diffuse * 0.82f;
  return float4(gColor.rgb * lighting, 1.0f);
}
