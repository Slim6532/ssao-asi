// ssao.hlsl - HBAO-style ambient occlusion for GTA SA (depth only), fxc / ps_3_0
//
//   VSMain  : fullscreen quad
//   PSAO    : horizon-based AO at HALF resolution (NDIR directions x NSTEP steps, set with /D)
//             writes (ao, linear depth) so the later passes never have to touch the depth buffer again
//   PSBlur  : 5-tap depth-aware blur (exact 4 px box), run twice (horizontal, vertical), half res
//   PSComp  : depth-aware 2x2 upsample to full resolution (+ dither), MULTIPLIED onto the backbuffer

sampler2D sDepth : register(s0);   // INTZ depth texture (point)
sampler2D sAO    : register(s1);   // half-res AO target: R = ao, G = linear depth (point)

float4 gProj : register(c0);   // x = near*far, y = far, z = far-near, w = radius (world units)
float4 gView : register(c1);   // x = viewWindow.x, y = viewWindow.y, z = 1/fullW, w = 1/fullH
float4 gCfg  : register(c2);   // x = intensity, y = bias, z = max radius (full-res px), w = fade distance
float4 gCfg2 : register(c3);   // x = power, y = 1/halfW, z = 1/halfH
float4 gDir  : register(c4);   // blur step in uv units (one half-res texel along X or Y)
float4 gCfg3 : register(c5);   // x = contact (small-scale) strength, y = large-scale strength, z = contact radius / radius
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

// ------------------------------------------------------------------------------------
float4 PSAO(float2 uv : TEXCOORD0) : COLOR
{
	uv = SnapUV(uv - 0.5 * gView.zw);                          // full-res pixel (2i,2j), exact centre
	// This half-res pixel stands for a 2x2 block of full-res pixels. Use the NEAREST pixel of the block
	// (lowest depth) as its representative, so thin foreground things (poles, wires, fences, limbs)
	// are not missed by the half-res grid and do not turn into blocky stair-steps.
	float2 tx1 = float2(gView.z, 0);
	float2 ty1 = float2(0, gView.w);
	float dA = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	float dB = tex2Dlod(sDepth, float4(uv + tx1, 0, 0)).r;
	float dC = tex2Dlod(sDepth, float4(uv + ty1, 0, 0)).r;
	float dD = tex2Dlod(sDepth, float4(uv + tx1 + ty1, 0, 0)).r;
	float d0 = min(min(dA, dB), min(dC, dD));
	float2 pick = uv;
	if(d0 == dB)      pick = uv + tx1;
	else if(d0 == dC) pick = uv + ty1;
	else if(d0 == dD) pick = uv + tx1 + ty1;
	uv = pick;                                                 // exact centre of the chosen full-res pixel
	float zc = gProj.x / (gProj.y - d0 * gProj.z);
	float zs0 = min(zc, 60000.0);                              // what the blur passes see as depth
	float rPx = gProj.w / (2.0 * gView.x * zc) / gView.z;      // AO radius in full-res pixels

	// cheap early-out: sky, far away, or too small on screen to matter
	[branch] if (d0 >= 0.9999 || zc > gCfg.w || rPx < 2.0)
		return float4(1, zs0, 0, 1);
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
	// pattern cancels out completely.
	float2 hpix = floor(uv / gCfg2.yz);
	float2 nb  = fmod(hpix, 4.0);
	float2 b0  = fmod(nb, 2.0);
	float2 b1  = floor(nb * 0.5);
	float bay  = abs(b0.x - b0.y) * 8.0 + b0.y * 4.0 + abs(b1.x - b1.y) * 2.0 + b1.y;   // 0..15
	float rot  = (bay + 0.5) / 16.0;
	float jit  = (fmod(bay * 5.0 + 3.0, 16.0) + 0.5) / 16.0;   // different permutation than rot

	// distance LOD: beyond half the fade distance a pixel only gets about half the directions
	// (small on screen + already faded, so invisible, but ~25% cheaper overall).
	// The noise tile / blur still cancel because the count only depends on distance.
	float nd = (zc > gCfg.w * 0.5) ? max(2.0, floor(NDIR * 0.5 + 0.5)) : (float)NDIR;

	// Two-scale AO, the way NVIDIA's HBAO+ does it (SmallScaleAO + LargeScaleAO):
	//   large scale = the horizon term over the whole radius (soft, broad shading)
	//   contact     = the same horizon term over a short radius only (crisp creases, grounds feet/props)
	// Both come from the SAME samples, so the second term costs a few ALU ops, no extra fetches.
	float R2     = gProj.w * gProj.w;
	float Rs     = gProj.w * gCfg3.z;
	float invR2  = 1.0 / R2;
	float invRs2 = 1.0 / (Rs * Rs);
	float aoL = 0, aoS = 0;
	[unroll] for (int i = 0; i < NDIR; i++)
	{
	  [branch] if (i < nd)
	  {
		float  a   = (i + rot) * (6.2831853 / nd);
		float2 dir = float2(cos(a), sin(a));
		float  hL = gCfg.y;                                     // horizons start at the bias angle
		float  hS = gCfg.y;
		float  aoDirL = 0, aoDirS = 0;
		[unroll] for (int j = 0; j < NSTEP; j++)
		{
			float u = (j + 0.25 + 0.75 * jit) / NSTEP;
			u = u * (0.4 + 0.6 * u);                            // denser near the pixel: 2 taps in the contact zone, 2 reach the full radius
			float t = max(u * rPx, 1.5);                        // pixels
			float2 suv = SnapUV(uv + dir * t * gView.zw);       // snap: position must match the depth texel
			float3 V   = ViewPos(suv, LinZ(suv)) - P;
			float  d2  = dot(V, V);
			float  h   = dot(N, V) * rsqrt(d2 + 1e-6);          // sin(elevation above tangent plane)
			float  wL  = saturate(1.0 - d2 * invR2);            // large-scale distance falloff
			float  wS  = saturate(1.0 - d2 * invRs2);           // contact distance falloff (short radius)
			// samples outside a scale's radius must not raise ITS horizon (otherwise a far pole or wall
			// "shadows" the real nearby occluders and leaves halos around foreground objects)
			float  hnL = max(hL, (wL > 0.0) ? h : -1.0);
			float  hnS = max(hS, (wS > 0.0) ? h : -1.0);
			aoDirL += (hnL - hL) * wL;                          // only count NEW horizon height
			aoDirS += (hnS - hS) * wS;
			hL = hnL;
			hS = hnS;
		}
		aoL += aoDirL;
		aoS += aoDirS;
	  }
	}
	// contact term is an ADDITION on top of the large-scale term (0.6 keeps overall brightness close to
	// the single-scale version at the default strengths)
	float ao = (aoL * gCfg3.y + aoS * gCfg3.x * 0.6) / nd;

	float fade = saturate((gCfg.w - zc) / (gCfg.w * 0.35));
	fade = fade * fade * (3.0 - 2.0 * fade);                    // smoothstep: no visible edge of the AO range
	float occ  = saturate(ao * gCfg.x * fade);
	float v    = pow(1.0 - occ, gCfg2.x);
	return float4(v, zs0, 0, 1);
}

// ------------------------------------------------------------------------------------
// Exact 4-pixel box (weights .5 1 1 1 .5) = one full 4x4 noise tile per H+V pass pair
static const float kOff[4] = { -2, -1, 1, 2 };
static const float kWt[4]  = { 0.5, 1.0, 1.0, 0.5 };

float4 PSBlur(float2 uv : TEXCOORD0) : COLOR
{
	float2 c   = tex2Dlod(sAO, float4(uv, 0, 0)).rg;           // (ao, depth)
	float zc   = c.y;
	float sum  = c.x;
	float wsum = 1.0;
	float tol  = zc * 0.04 + 0.05;
	[unroll] for (int i = 0; i < 4; i++)
	{
		float2 s = tex2Dlod(sAO, float4(uv + gDir.xy * kOff[i], 0, 0)).rg;
		float  w = kWt[i] * saturate(1.0 - abs(s.y - zc) / tol);
		sum  += s.x * w;
		wsum += w;
	}
	return float4(sum / wsum, zc, 0, 1);
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
		float2 s  = tex2Dlod(sAO, float4(uv00 + float2(x, y) * gCfg2.yz, 0, 0)).rg;
		float  wb = (x ? f.x : 1.0 - f.x) * (y ? f.y : 1.0 - f.y);
		// joint-bilateral with a heavy tail: similar depth -> full weight, different depth -> tiny weight,
		// and if ALL four disagree the nearest-in-depth one wins smoothly (no blocky thin objects)
		float  w  = wb / (abs(s.y - zc) + zc * 0.01 + 0.01);
		sum  += s.x * w;
		wsum += w;
	}
	float v = sum / wsum;
	// 1/255 dither: smooth AO gradients would otherwise band on an 8-bit backbuffer
	float n = frac(52.9829189 * frac(dot(uv / gView.zw, float2(0.06711056, 0.00583715))));
	v = saturate(v + (n - 0.5) * (1.0 / 255.0));
	return float4(v, v, v, 1);
}
