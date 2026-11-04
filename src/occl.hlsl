// Occlusion proxy: world-space cube (centre.xyz, half extent .w) drawn with colour+depth writes off
float4x4 combined : register(c0);
float4   cubeInfo : register(c4);
float4 VSMain(float3 pos : POSITION) : POSITION
{
	return mul(float4(cubeInfo.xyz + pos*cubeInfo.w, 1.0), combined);
}
float4 PSMain() : COLOR { return float4(0, 0, 0, 0); }
