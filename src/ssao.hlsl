// ssao.hlsl - depth-only SSAO for GTA SA. Compiled offline with fxc (see build.yml).
//   VSMain : fullscreen quad
//   PSAO   : writes occlusion (1 = unoccluded) into a render target
//   PSBlur : depth-aware blur, output is MULTIPLIED onto the backbuffer (dest * src)

sampler2D sDepth : register(s0);   // INTZ depth texture, point sampled
sampler2D sAO    : register(s1);   // AO render target, linear sampled

float4 gProj : register(c0);       // x = near*far, y = far, z = far-near, w = radius (world units)
float4 gView : register(c1);       // x = viewWindow.x, y = viewWindow.y, z = 1/width, w = 1/height
float4 gCfg  : register(c2);       // x = intensity, y = bias, z = max radius (pixels), w = fade distance

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float2 p : POSITION)
{
	VSOut o;
	// half-pixel offset for D3D9
	o.pos = float4(p.x - gView.z, p.y + gView.w, 0, 1);
	o.uv  = p * float2(0.5, -0.5) + 0.5;
	return o;
}

float LinZ(float2 uv)
{
	float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	return gProj.x / (gProj.y - d * gProj.z);      // z = n*f / (f - d*(f-n))
}

float3 ViewPos(float2 uv, float z)
{
	return float3((uv.x * 2 - 1) * gView.x * z,
	              (1 - uv.y * 2) * gView.y * z,
	              z);
}

#define NSAMPLES 12

float4 PSAO(float2 uv : TEXCOORD0) : COLOR
{
	float d0  = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	float zc  = gProj.x / (gProj.y - d0 * gProj.z);
	float3 pc = ViewPos(uv, zc);

	// normal from depth derivatives (computed before any branching on purpose)
	float3 nn  = cross(ddx(pc), ddy(pc));
	float3 nrm = nn / max(length(nn), 1e-6);        // flip the cross() order if AO looks inverted

	// per-pixel rotation (interleaved gradient noise)
	float2 pix = uv / gView.zw;
	float  ign = frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
	float  ang = ign * 6.2831853;
	float2 cs  = float2(cos(ang), sin(ang));

	// world radius -> pixels at this depth
	float rPx = gProj.w / (2.0 * gView.x * zc) / gView.z;
	rPx = min(rPx, gCfg.z);

	float occ = 0;
	[unroll]
	for (int i = 0; i < NSAMPLES; i++)
	{
		float  a = i * 2.39996323;                  // golden angle spiral
		float2 k = float2(cos(a), sin(a)) * sqrt((i + 0.5) / NSAMPLES);
		float2 r = float2(k.x * cs.x - k.y * cs.y, k.x * cs.y + k.y * cs.x);
		float2 suv = uv + r * rPx * gView.zw;

		float3 v    = ViewPos(suv, LinZ(suv)) - pc;
		float  dist = length(v);
		float  h    = dot(nrm, v) / max(dist, 1e-4);
		occ += saturate(h - gCfg.y) * saturate(1.0 - dist / gProj.w);
	}

	float ao   = 1.0 - saturate(occ / NSAMPLES * 2.0 * gCfg.x);
	float sky  = step(0.9999, d0);
	float fade = saturate(1.0 - zc / gCfg.w);
	ao = lerp(1.0, ao, fade);
	ao = lerp(ao, 1.0, sky);
	return float4(ao, ao, ao, 1);
}

float4 PSBlur(float2 uv : TEXCOORD0) : COLOR
{
	float zc = LinZ(uv);
	float sum = 0, wsum = 0;
	[unroll] for (int y = -2; y <= 2; y++)
	[unroll] for (int x = -2; x <= 2; x++)
	{
		float2 suv = uv + float2(x, y) * gView.zw;
		float  zs  = LinZ(suv);
		float  w   = saturate(1.0 - abs(zs - zc) / max(zc * 0.05, 0.05));
		sum  += tex2Dlod(sAO, float4(suv, 0, 0)).r * w;
		wsum += w;
	}
	float ao = sum / max(wsum, 1e-4);
	return float4(ao, ao, ao, 1);
}
