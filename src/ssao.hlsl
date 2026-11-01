// ssao.hlsl - HBAO-style ambient occlusion for GTA SA (depth only), fxc / ps_3_0
//
//   VSMain  : fullscreen quad
//   PSAO    : horizon-based AO at HALF resolution (NDIR directions x NSTEP steps, set with /D)
//   PSBlur  : 7-tap depth-aware blur, run twice (horizontal, vertical) at half resolution
//   PSComp  : depth-aware 2x2 upsample to full resolution, output is MULTIPLIED onto the backbuffer

sampler2D sDepth : register(s0);   // INTZ depth texture (point)
sampler2D sAO    : register(s1);   // half-res AO target (linear)

float4 gProj : register(c0);   // x = near*far, y = far, z = far-near, w = radius (world units)
float4 gView : register(c1);   // x = viewWindow.x, y = viewWindow.y, z = 1/fullW, w = 1/fullH
float4 gCfg  : register(c2);   // x = intensity, y = bias, z = max radius (full-res px), w = fade distance
float4 gCfg2 : register(c3);   // x = power, y = 1/halfW, z = 1/halfH
float4 gDir  : register(c4);   // blur step in uv units (one half-res texel along X or Y)
float4 gRT   : register(c8);   // VS only: 1/width, 1/height of the current render target

#ifndef NDIR
#define NDIR 4
#endif
#ifndef NSTEP
#define NSTEP 4
#endif

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float2 p : POSITION)
{
	VSOut o;
	o.pos = float4(p.x - gRT.x, p.y + gRT.y, 0, 1);      // D3D9 half-pixel offset
	o.uv  = p * float2(0.5, -0.5) + 0.5;
	return o;
}

float LinZ(float2 uv)
{
	float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	return gProj.x / (gProj.y - d * gProj.z);             // z = n*f / (f - d*(f-n))
}

float3 ViewPos(float2 uv, float z)
{
	return float3((uv.x * 2 - 1) * gView.x * z,
	              (1 - uv.y * 2) * gView.y * z,
	              z);
}

// Depth is point sampled, so every position MUST be snapped to a full-res pixel centre before it is
// paired with a depth value. Otherwise position and depth disagree by up to half a pixel, which on
// steep (grazing) surfaces looks like real geometry and draws moire / horizontal lines in the AO.
float2 SnapUV(float2 uv)
{
	return (floor(uv / gView.zw) + 0.5) * gView.zw;
}

// depth at a HALF-res texel centre (lies exactly on a full-res pixel border): always use full-res
// pixel (2i, 2j), the same one PSAO used for that texel
float ZHalf(float2 uv)
{
	return LinZ(SnapUV(uv - 0.5 * gView.zw));
}

// ------------------------------------------------------------------------------------
float4 PSAO(float2 uv : TEXCOORD0) : COLOR
{
	uv = SnapUV(uv - 0.5 * gView.zw);                          // full-res pixel (2i,2j), exact centre
	float d0 = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	float zc = gProj.x / (gProj.y - d0 * gProj.z);
	float rPx = gProj.w / (2.0 * gView.x * zc) / gView.z;      // AO radius in full-res pixels

	// cheap early-out: sky, far away, or too small on screen to matter
	[branch] if (d0 >= 0.9999 || zc > gCfg.w || rPx < 2.0)
		return float4(1, 1, 1, 1);
	rPx = min(rPx, gCfg.z);

	float3 P = ViewPos(uv, zc);

	// normal from depth: pick the smaller depth difference on each axis (no edge smearing)
	float2 tx = float2(gView.z, 0);
	float2 ty = float2(0, gView.w);
	float3 Pr = ViewPos(uv + tx, LinZ(uv + tx));
	float3 Pl = ViewPos(uv - tx, LinZ(uv - tx));
	float3 Pd = ViewPos(uv + ty, LinZ(uv + ty));
	float3 Pu = ViewPos(uv - ty, LinZ(uv - ty));
	float3 dx = (abs(Pr.z - zc) < abs(zc - Pl.z)) ? (Pr - P) : (P - Pl);
	float3 dy = (abs(Pd.z - zc) < abs(zc - Pu.z)) ? (Pd - P) : (P - Pu);
	float3 N  = normalize(cross(dx, dy) + 1e-8);
	N = (dot(N, P) > 0) ? -N : N;                               // always face the camera

	// per-pixel noise: direction rotation + step jitter.
	// 4x4 interleaved tile in HALF-res pixel coords. PSBlur averages exactly one 4x4 tile, so the
	// pattern cancels out completely (the old full-res gradient noise left horizontal lines).
	float2 hpix = floor(uv / gCfg2.yz);
	float2 nb  = fmod(hpix, 4.0);
	float2 b0  = fmod(nb, 2.0);
	float2 b1  = floor(nb * 0.5);
	float bay  = abs(b0.x - b0.y) * 8.0 + b0.y * 4.0 + abs(b1.x - b1.y) * 2.0 + b1.y;   // 0..15
	float rot  = (bay + 0.5) / 16.0;
	float jit  = (fmod(bay * 5.0 + 3.0, 16.0) + 0.5) / 16.0;   // different permutation than rot

	float R2 = gProj.w * gProj.w;
	float ao = 0;
	[unroll] for (int i = 0; i < NDIR; i++)
	{
		float  a   = (i + rot) * (6.2831853 / NDIR);
		float2 dir = float2(cos(a), sin(a));
		float  hmax = gCfg.y;                                   // horizon starts at the bias angle
		float  aoDir = 0;
		[unroll] for (int j = 0; j < NSTEP; j++)
		{
			float t = ((j + 0.25 + 0.75 * jit) / NSTEP) * rPx;  // pixels
			t = max(t, 1.5);
			float2 suv = SnapUV(uv + dir * t * gView.zw);       // snap: position must match the depth texel
			float3 V   = ViewPos(suv, LinZ(suv)) - P;
			float  d2  = dot(V, V);
			float  h   = dot(N, V) * rsqrt(d2 + 1e-6);          // sin(elevation above tangent plane)
			float  w   = saturate(1.0 - d2 / R2);               // HBAO distance falloff
			float  hn  = max(h, hmax);
			aoDir += (hn - hmax) * w;                           // only count NEW horizon height
			hmax = hn;
		}
		ao += aoDir;
	}
	ao /= NDIR;

	float fade = saturate((gCfg.w - zc) / (gCfg.w * 0.35));
	float occ  = saturate(ao * gCfg.x * fade);
	float v    = pow(1.0 - occ, gCfg2.x);
	return float4(v, v, v, 1);
}

// ------------------------------------------------------------------------------------
// Exact 4-pixel box (weights .5 1 1 1 .5) = one full 4x4 noise tile per H+V pass pair
static const float kOff[4] = { -2, -1, 1, 2 };
static const float kWt[4]  = { 0.5, 1.0, 1.0, 0.5 };

float4 PSBlur(float2 uv : TEXCOORD0) : COLOR
{
	float zc  = ZHalf(uv);
	float sum = tex2Dlod(sAO, float4(uv, 0, 0)).r * 1.0;
	float wsum = 1.0;
	float tol = zc * 0.04 + 0.05;
	[unroll] for (int i = 0; i < 4; i++)
	{
		float2 suv = uv + gDir.xy * kOff[i];
		float  zs  = ZHalf(suv);
		float  w   = kWt[i] * saturate(1.0 - abs(zs - zc) / tol);
		sum  += tex2Dlod(sAO, float4(suv, 0, 0)).r * w;
		wsum += w;
	}
	float v = sum / wsum;
	return float4(v, v, v, 1);
}

// ------------------------------------------------------------------------------------
float4 PSComp(float2 uv : TEXCOORD0) : COLOR
{
	float  zc   = LinZ(uv);
	float2 hp   = uv / gCfg2.yz - 0.5;                 // position in half-res texel space
	float2 base = floor(hp);
	float2 f    = hp - base;
	float2 uv00 = (base + 0.5) * gCfg2.yz;
	float  tol  = zc * 0.03 + 0.05;
	float sum = 0, wsum = 0;
	[unroll] for (int y = 0; y < 2; y++)
	[unroll] for (int x = 0; x < 2; x++)
	{
		float2 suv = uv00 + float2(x, y) * gCfg2.yz;
		float  zs  = ZHalf(suv);
		float  wb  = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
		float  w   = wb * saturate(1.0 - abs(zs - zc) / tol) + 1e-4;
		sum  += tex2Dlod(sAO, float4(suv, 0, 0)).r * w;
		wsum += w;
	}
	float v = sum / wsum;
	return float4(v, v, v, 1);
}
