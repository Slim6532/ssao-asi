// post_smaa.hlsl - SMAA 1x (3 passes) wrappers around the official SMAA.hlsl (iryoku/smaa).
// Compiled ONLY if SMAA.hlsl, AreaTex.h and SearchTex.h are present in src\ (see build.yml).
//
//   pass 1  VSEdge  / PSEdge  : luma edge detection           scene          -> edges  (RG)
//   pass 2  VSWeight/ PSWeight: blending weight calculation   edges+area+search -> weights (RGBA)
//   pass 3  VSBlend / PSBlend : neighbourhood blending        scene+weights  -> backbuffer

#define SMAA_HLSL_3 1
#define SMAA_PRESET_HIGH 1
float4 gMetrics : register(c0);              // (1/w, 1/h, w, h); same value in the VS and PS constant files
#define SMAA_RT_METRICS gMetrics
#include "SMAA.hlsl"

float4 gRT : register(c8);                   // VS only: 1/width, 1/height of the render target (half-pixel offset)

sampler2D colorTex  : register(s0);          // copy of the finished frame
sampler2D edgesTex  : register(s1);
sampler2D areaTex   : register(s2);
sampler2D searchTex : register(s3);
sampler2D blendTex  : register(s4);

float4 ScreenPos(float2 p) { return float4(p.x - gRT.x, p.y + gRT.y, 0, 1); }   // D3D9 half-pixel offset
float2 ScreenUV(float2 p)  { return p * float2(0.5, -0.5) + 0.5; }

// ---- pass 1: edges ----
struct EdgeVS { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };

EdgeVS VSEdge(float2 p : POSITION)
{
	EdgeVS o;
	o.pos = ScreenPos(p);
	o.uv  = ScreenUV(p);
	float4 off[3];
	SMAAEdgeDetectionVS(o.uv, off);
	o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2];
	return o;
}

float4 PSEdge(EdgeVS i) : COLOR
{
	float4 off[3] = { i.o0, i.o1, i.o2 };
	return float4(SMAALumaEdgeDetectionPS(i.uv, off, colorTex), 0, 1);
}

// ---- pass 2: blending weights ----
struct WeightVS { float4 pos : POSITION; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };

WeightVS VSWeight(float2 p : POSITION)
{
	WeightVS o;
	o.pos = ScreenPos(p);
	o.uv  = ScreenUV(p);
	float4 off[3];
	SMAABlendingWeightCalculationVS(o.uv, o.pix, off);
	o.o0 = off[0]; o.o1 = off[1]; o.o2 = off[2];
	return o;
}

float4 PSWeight(WeightVS i) : COLOR
{
	float4 off[3] = { i.o0, i.o1, i.o2 };
	return SMAABlendingWeightCalculationPS(i.uv, i.pix, off, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}

// ---- pass 3: neighbourhood blending ----
struct BlendVS { float4 pos : POSITION; float2 uv : TEXCOORD0; float4 off : TEXCOORD1; };

BlendVS VSBlend(float2 p : POSITION)
{
	BlendVS o;
	o.pos = ScreenPos(p);
	o.uv  = ScreenUV(p);
	float4 off;
	SMAANeighborhoodBlendingVS(o.uv, off);
	o.off = off;
	return o;
}

float4 PSBlend(BlendVS i) : COLOR
{
	return SMAANeighborhoodBlendingPS(i.uv, i.off, colorTex, blendTex);
}
