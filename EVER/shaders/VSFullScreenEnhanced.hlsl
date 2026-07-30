struct Output
{
	float4 position : SV_POSITION;
	float2 texcoord : TEXCOORD;
};

Output main(uint id : SV_VertexID)
{
	Output output;
	output.position = float4(
		((id == 0 || id == 1) ? -1.0f : 1.0f),
		((id == 0 || id == 2) ? -1.0f : 1.0f),
		0.0f, 1.0f);
	output.texcoord = float2(
		((id == 0 || id == 1) ? 0.0f : 1.0f),
		((id == 0 || id == 2) ? 0.0f : 1.0f));
	return output;
}
