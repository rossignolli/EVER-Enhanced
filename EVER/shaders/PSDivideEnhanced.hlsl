struct VSOutput
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

Texture2D<float4> AccumulationTexture : register(t0);

cbuffer DivideConstants : register(b0)
{
	float4 values;
}

float4 main(VSOutput input) : SV_TARGET
{
	return AccumulationTexture.Load(int3(input.position.xy, 0)) / values.x;
}
