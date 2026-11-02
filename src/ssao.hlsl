// ssao.hlsl - port of NVIDIA HBAO+ 3.1 (GFSDK_SSAO) to D3D9 / ps_3_0, for GTA SA (depth only).
//
// What is taken 1:1 from NVIDIA's shaders (CoarseAO_PS / Blur_Common / ReinterleaveAO_PS / RandomTexture):
//   * AO per sample   = saturate(N.V - bias) * saturate(1 - |V|^2 / R^2), SUMMED (no max-horizon)
//   * 8 directions x 4 steps, step = (radiusPx/4)/(steps+1) in quarter-res units, samples snapped to a
//     4-pixel lattice (the deinterleaved-slice lattice), random start jitter inside the first step
//   * the 1st step feeds the small-scale AO, steps 2..4 the large-scale AO
//   * 4x4 random rotation/jitter table (the SDK's own Mersenne-Twister numbers), sRand
//   * cross-bilateral blur with depth-slope compensation, pow(AO, power) AFTER the blur
// D3D9 has no texture arrays / geometry shaders, so instead of 16 deinterleaved slices every pixel
// samples the same lattice directly. Quality 0/1 run at half resolution, quality 2 at full resolution.
//
//   VSMain  : fullscreen quad
//   PSAO    : AO (+ linear depth packed in G), at AO resolution
//   PSBlur  : cross-bilateral blur, 5 taps, run twice (horizontal, vertical)
//   PSComp  : depth-aware upsample, pow(power), dither, MULTIPLIED onto the backbuffer

sampler2D sDepth : register(s0);   // INTZ depth texture (point)
sampler2D sAO    : register(s1);   // AO target: R = ao, G = linear depth (point)
sampler2D sRand  : register(s2);   // 4x4 jitter table: R = rotation fraction r1, G = start jitter r2 (point, wrap)

float4 gProj : register(c0);   // x = near*far, y = far, z = far-near, w = radius (world units)
float4 gView : register(c1);   // x = viewWindow.x (tan half fov x), y = viewWindow.y, z = 1/fullW, w = 1/fullH
float4 gCfg  : register(c2);   // x = unused, y = blur sharpness, z = max radius (full-res px), w = fade distance
float4 gCfg2 : register(c3);   // x = power, y = 1/aoW, z = 1/aoH, w = AO texel size in full-res px (1 or 2)
float4 gDir  : register(c4);   // blur step in uv units (one AO texel along X or Y)
float4 gCfg3 : register(c5);   // x = small-scale amount, y = large-scale amount, z = -1/R^2, w = NDotV bias
float4 gRT   : register(c8);   // VS only: 1/width, 1/height of the current render target

#ifndef NDIR
#define NDIR 8
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
// paired with a depth value (otherwise position and depth disagree by up to half a pixel).
float2 SnapUV(float2 uv)
{
	return (floor(uv / gView.zw) + 0.5) * gView.zw;
}

// ---- NVIDIA ReconstructNormal_Common.hlsl ----
float3 MinDiff(float3 P, float3 Pr, float3 Pl)
{
	float3 V1 = Pr - P;
	float3 V2 = P - Pl;
	return (dot(V1, V1) < dot(V2, V2)) ? V1 : V2;
}

// ---- NVIDIA CoarseAO_PS.hlsl: ComputeAO ----
float ComputeAO(float3 P, float3 N, float3 S)
{
	float3 V = S - P;
	float VdotV = dot(V, V);
	float NdotV = dot(N, V) * rsqrt(max(VdotV, 1e-6));
	return saturate(NdotV - gCfg3.w) * saturate(VdotV * gCfg3.z + 1.0);   // cosine term * falloff
}

// ------------------------------------------------------------------------------------
float4 PSAO(float2 uv : TEXCOORD0) : COLOR
{
	// representative full-res pixel of this AO texel (exact pixel centre)
	float scale = gCfg2.w;
	uv = SnapUV(uv - (scale - 1.0) * 0.5 * gView.zw);
	float d0 = tex2Dlod(sDepth, float4(uv, 0, 0)).r;

	// Half-res: the AO texel stands for a 2x2 block. Use the NEAREST pixel of the block, so thin
	// foreground things (poles, wires, fences) are not missed by the half-res grid.
	[branch] if (scale > 1.5)
	{
		float2 tx1 = float2(gView.z, 0);
		float2 ty1 = float2(0, gView.w);
		float dB = tex2Dlod(sDepth, float4(uv + tx1, 0, 0)).r;
		float dC = tex2Dlod(sDepth, float4(uv + ty1, 0, 0)).r;
		float dD = tex2Dlod(sDepth, float4(uv + tx1 + ty1, 0, 0)).r;
		float dm = min(min(d0, dB), min(dC, dD));
		float2 pick = uv;
		if(dm == dB)      pick = uv + tx1;
		else if(dm == dC) pick = uv + ty1;
		else if(dm == dD) pick = uv + tx1 + ty1;
		d0 = dm;
		uv = pick;
	}

	float zc  = gProj.x / (gProj.y - d0 * gProj.z);
	float zs0 = min(zc, 60000.0);                              // what the blur passes see as depth
	float rPx = gProj.w / (2.0 * gView.x * zc) / gView.z;      // AO radius in full-res pixels

	// early-out: sky, beyond the fade distance, or a footprint smaller than the first lattice step (4 px)
	[branch] if (d0 >= 0.9999 || zc > gCfg.w || rPx < 4.0)
		return float4(1, zs0, 0, 1);
	rPx = min(rPx, gCfg.z);

	float3 P = ViewPos(uv, zc);

	// normal from depth (NVIDIA: cross of the smaller difference on each axis)
	float2 tx = float2(gView.z, 0);
	float2 ty = float2(0, gView.w);
	float3 Pr = ViewPos(uv + tx, LinZ(uv + tx));
	float3 Pl = ViewPos(uv - tx, LinZ(uv - tx));
	float3 Pd = ViewPos(uv + ty, LinZ(uv + ty));
	float3 Pu = ViewPos(uv - ty, LinZ(uv - ty));
	float3 N  = normalize(cross(MinDiff(P, Pr, Pl), MinDiff(P, Pd, Pu)) + 1e-8);
	N = (dot(N, P) > 0) ? -N : N;                               // always face the camera

	// per-pixel jitter from the 4x4 table (NVIDIA RandomTexture): x = rotation fraction, y = start jitter
	float2 apix = floor(uv / gCfg2.yz);
	float2 rnd  = tex2Dlod(sRand, float4((fmod(apix, 4.0) + 0.5) * 0.25, 0, 0)).rg;

	// distance LOD: beyond half the fade distance only half the directions (small on screen, already fading)
	float nd = (zc > gCfg.w * 0.5) ? max(2.0, floor(NDIR * 0.5 + 0.5)) : (float)NDIR;

	// step size in QUARTER-res pixel units, like NVIDIA ((radiusPixels / 4) / (NUM_STEPS + 1))
	float stepQ  = (rPx * 0.25) / (NSTEP + 1.0);
	float alpha  = 6.2831853 / nd;
	float theta  = alpha * rnd.x;                               // random rotation within one direction sector
	float smallAO = 0, largeAO = 0;

	[unroll] for (int i = 0; i < NDIR; i++)
	{
	  [branch] if (i < nd)
	  {
		float  ang = alpha * i + theta;
		float2 dir = float2(cos(ang), sin(ang));
		float  ray = rnd.y * stepQ + 1.0;                       // jitter the first sample inside the first step

		{   // first step -> small-scale AO
			float2 suv = SnapUV(uv + round(ray * dir) * 4.0 * gView.zw);   // 4 px lattice (= 1 quarter-res px)
			float3 S   = ViewPos(suv, LinZ(suv));
			smallAO += ComputeAO(P, N, S);
			ray += stepQ;
		}
		[unroll] for (int j = 1; j < NSTEP; j++)                // remaining steps -> large-scale AO
		{
			float2 suv = SnapUV(uv + round(ray * dir) * 4.0 * gView.zw);
			float3 S   = ViewPos(suv, LinZ(suv));
			largeAO += ComputeAO(P, N, S);
			ray += stepQ;
		}
	  }
	}

	float ao = (smallAO * gCfg3.x + largeAO * gCfg3.y) / (nd * NSTEP);

	float fade = saturate((gCfg.w - zc) / (gCfg.w * 0.35));
	fade = fade * fade * (3.0 - 2.0 * fade);                    // smoothstep: no visible edge of the AO range
	float v = 1.0 - saturate(ao * 2.0) * fade;                  // NVIDIA: saturate(1 - AO*2)
	return float4(v, zs0, 0, 1);
}

// ------------------------------------------------------------------------------------
// NVIDIA Blur_Common.hlsl: cross-bilateral weight exp2(-dz^2) with DEPTH SLOPE compensation, so that
// slanted surfaces (the ground at a grazing angle) blur fully while depth edges stay sharp.
// The spatial weights are the exact 4-texel box (.5 1 1 1 .5): it averages one whole 4x4 noise tile.
float4 PSBlur(float2 uv : TEXCOORD0) : COLOR
{
	float2 c  = tex2Dlod(sAO, float4(uv, 0, 0)).rg;            // (ao, depth)
	float  zc = c.y;
	float  sharp = gCfg.y;

	float2 d  = gDir.xy;
	float2 a1 = tex2Dlod(sAO, float4(uv + d, 0, 0)).rg;
	float2 b1 = tex2Dlod(sAO, float4(uv - d, 0, 0)).rg;
	float2 a2 = tex2Dlod(sAO, float4(uv + 2.0 * d, 0, 0)).rg;
	float2 b2 = tex2Dlod(sAO, float4(uv - 2.0 * d, 0, 0)).rg;

	// slope = depth change per texel; take the smoother side (like MinDiff) so a depth edge on one
	// side does not corrupt the prediction for the other
	float sa = a1.y - zc;
	float sb = zc - b1.y;
	float slope = (abs(sa) < abs(sb)) ? sa : sb;

	// (explicit squares: pow() of a negative number is NaN in HLSL)
	float ea1 = (a1.y - (zc + slope))       * sharp;
	float eb1 = (b1.y - (zc - slope))       * sharp;
	float ea2 = (a2.y - (zc + slope * 2.0)) * sharp;
	float eb2 = (b2.y - (zc - slope * 2.0)) * sharp;
	float wa1 = exp2(-ea1 * ea1);
	float wb1 = exp2(-eb1 * eb1);
	float wa2 = 0.5 * exp2(-ea2 * ea2);
	float wb2 = 0.5 * exp2(-eb2 * eb2);

	float sum  = c.x + a1.x * wa1 + b1.x * wb1 + a2.x * wa2 + b2.x * wb2;
	float wsum = 1.0 + wa1 + wb1 + wa2 + wb2;
	return float4(sum / wsum, zc, 0, 1);
}

// ------------------------------------------------------------------------------------
float4 PSComp(float2 uv : TEXCOORD0) : COLOR
{
	float  zc   = LinZ(uv);
	float2 hp   = uv / gCfg2.yz - 0.5;                 // position in AO texel space
	float2 base = floor(hp);
	float2 f    = hp - base;
	float2 uv00 = (base + 0.5) * gCfg2.yz;
	float sum = 0, wsum = 0;
	[unroll] for (int y = 0; y < 2; y++)
	[unroll] for (int x = 0; x < 2; x++)
	{
		float2 s  = tex2Dlod(sAO, float4(uv00 + float2(x, y) * gCfg2.yz, 0, 0)).rg;
		float  wb = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
		// joint-bilateral with a heavy tail: similar depth -> full weight, different depth -> tiny weight,
		// and if ALL four disagree the nearest-in-depth one wins smoothly (no blocky thin objects)
		float  w  = wb / (abs(s.y - zc) + zc * 0.01 + 0.01);
		sum  += s.x * w;
		wsum += w;
	}
	float v = pow(saturate(sum / wsum), gCfg2.x);      // NVIDIA: pow(AO, PowerExponent) after the blur
	// 1/255 dither: smooth AO gradients would otherwise band on an 8-bit backbuffer
	float n = frac(52.9829189 * frac(dot(uv / gView.zw, float2(0.06711056, 0.00583715))));
	v = saturate(v + (n - 0.5) * (1.0 / 255.0));
	return float4(v, v, v, 1);
}
