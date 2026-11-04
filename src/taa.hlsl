// taa.hlsl - temporal anti-aliasing resolve for GTA SA (D3D9, ps_3_0)
// Camera-motion reprojection from the INTZ depth, 3x3 variance clipping, adaptive blend. No motion vectors:
// moving objects are handled by the clamp (ghosts are clipped away), not tracked.

sampler2D sCur  : register(s0);   // copy of this frame (point)
sampler2D sHist : register(s1);   // previous resolved frame (bilinear)
sampler2D sDepth: register(s2);   // INTZ depth (point)

float4 gPx   : register(c0);      // x,y = 1/width,1/height   z = base blend (0..1)   w = clip gamma
float4 gVW   : register(c1);      // x,y = view window now     z,w = view window previous
float4 gJit  : register(c2);      // xy = jitter now (NDC)     zw = jitter previous (NDC)
float4 gZ    : register(c3);      // x = n*f  y = f  z = f-n  w = history valid (1/0)
float4 gCR   : register(c4);      // camera now:  right, up, at, position
float4 gCU   : register(c5);
float4 gCA   : register(c6);
float4 gCP   : register(c7);
float4 gPR   : register(c8);      // camera previous
float4 gPU   : register(c9);
float4 gPA   : register(c10);
float4 gPP   : register(c11);
float4 gRT   : register(c8);      // VS only: 1/width, 1/height of the target

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float2 p : POSITION)
{
	VSOut o;
	o.pos = float4(p.x - gRT.x, p.y + gRT.y, 0, 1);
	o.uv  = p * float2(0.5, -0.5) + 0.5;
	return o;
}

float3 C(float2 uv) { return tex2Dlod(sCur, float4(uv, 0, 0)).rgb; }

float4 PSTaa(float2 uv : TEXCOORD0) : COLOR
{
	float2 px = gPx.xy;
	float3 c0 = C(uv);
	if(gZ.w < 0.5) return float4(c0, 1);

	// neighbourhood
	float3 n1 = C(uv + float2(-px.x, -px.y)), n2 = C(uv + float2(0, -px.y)), n3 = C(uv + float2(px.x, -px.y));
	float3 n4 = C(uv + float2(-px.x, 0)),                                      n6 = C(uv + float2(px.x, 0));
	float3 n7 = C(uv + float2(-px.x, px.y)),  n8 = C(uv + float2(0, px.y)),   n9 = C(uv + float2(px.x, px.y));
	float3 mn = min(c0, min(min(min(n1, n2), min(n3, n4)), min(min(n6, n7), min(n8, n9))));
	float3 mx = max(c0, max(max(max(n1, n2), max(n3, n4)), max(max(n6, n7), max(n8, n9))));
	float3 m1 = (c0 + n1 + n2 + n3 + n4 + n6 + n7 + n8 + n9) * (1.0/9.0);
	float3 m2 = (c0*c0 + n1*n1 + n2*n2 + n3*n3 + n4*n4 + n6*n6 + n7*n7 + n8*n8 + n9*n9) * (1.0/9.0);
	float3 sg = sqrt(max(m2 - m1*m1, 0.0));

	// reproject: pixel -> view ray (jitter removed) -> world -> previous camera -> previous uv
	float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	float z = gZ.x / (gZ.y - d * gZ.z);
	float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - gJit.xy;
	float3 v = float3(ndc.x * gVW.x * z, ndc.y * gVW.y * z, z);
	float3 w = gCP.xyz + gCR.xyz * v.x + gCU.xyz * v.y + gCA.xyz * v.z;
	float3 q = w - gPP.xyz;
	float3 p = float3(dot(q, gPR.xyz), dot(q, gPU.xyz), dot(q, gPA.xyz));
	if(p.z < 0.05) return float4(c0, 1);
	float2 pn = float2(p.x / (p.z * gVW.z), p.y / (p.z * gVW.w)) + gJit.zw;
	float2 puv = float2(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5);
	if(puv.x < 0.0 || puv.x > 1.0 || puv.y < 0.0 || puv.y > 1.0) return float4(c0, 1);

	float3 h  = tex2Dlod(sHist, float4(puv, 0, 0)).rgb;
	float3 lo = max(mn, m1 - gPx.w * sg);
	float3 hi = min(mx, m1 + gPx.w * sg);
	float3 hc = clamp(h, lo, hi);

	// the further the clamp had to move the history, the less it is trusted (moving objects, disocclusion)
	float diff  = length(h - hc);
	float alpha = saturate(gPx.z + diff * 6.0);
	return float4(lerp(hc, c0, alpha), 1);
}
