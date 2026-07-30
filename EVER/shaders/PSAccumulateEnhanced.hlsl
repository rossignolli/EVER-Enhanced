struct VSOutput
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

Texture2D<float4> SourceTexture : register(t0);

float4 main(VSOutput input) : SV_TARGET
{
	return SourceTexture.Load(int3(input.position.xy, 0));
}
