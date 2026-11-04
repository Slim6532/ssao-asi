// env-map pass pixel shader (only the env term; building pass has no specular)
sampler2D tex1 : register(s1);
struct PS_INPUT { float2 texcoord0 : TEXCOORD0; float4 envcolor : COLOR0; };
float4 main(PS_INPUT IN) : COLOR
{
	return tex2D(tex1, IN.texcoord0)*IN.envcolor;
}
