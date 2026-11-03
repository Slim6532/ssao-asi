// atmos.hlsl - depth fog, bloom and heat haze for GTA SA (D3D9, ps_3_0)
//
//   VSMain   : fullscreen quad
//   PSBright : quarter-res bright-pass (soft threshold) from the finished frame
//   PSBlur   : separable gaussian blur (5 bilinear taps = 9-tap), direction in gDir
//   PSAtmos  : final pass. heat haze (UV wobble on distant pixels) -> depth fog -> + bloom

sampler2D sScene : register(s0);   // copy of the finished frame (bilinear, clamp). For the blur: the bloom source
sampler2D sDepth : register(s1);   // INTZ depth (point)
sampler2D sBloom : register(s2);   // blurred bloom (bilinear)

float4 gPx    : register(c0);   // x = 1/width, y = 1/height (of the SOURCE), z = near, w = far
float4 gFogC  : register(c1);   // rgb = fog colour 0..1, a = fog strength 0..1
float4 gFogP  : register(c2);   // x = start, y = end (game units), z = curve, w = sun scatter 0..1
float4 gSun   : register(c3);   // xy = sun uv, z = sun valid, w = aspect (width / height)
float4 gSunC  : register(c4);   // rgb = sun colour 0..1
float4 gBloom : register(c5);   // x = intensity, y = threshold, z = knee
float4 gHaze  : register(c6);   // x = strength (uv), y = time, z = start dist, w = end dist
float4 gFlag  : register(c7);   // x = fog on, y = bloom on, z = haze on
float4 gDir   : register(c9);   // blur direction in uv units of the blur target
float4 gRT    : register(c8);   // VS only: 1/width, 1/height of the render target

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float2 p : POSITION)
{
	VSOut o;
	o.pos = float4(p.x - gRT.x, p.y + gRT.y, 0, 1);      // D3D9 half-pixel offset
	o.uv  = p * float2(0.5, -0.5) + 0.5;
	return o;
}

float3 S(float2 uv) { return tex2Dlod(sScene, float4(uv, 0, 0)).rgb; }

// ---------------- bloom ----------------
// One tap of the bright-pass: the sky (depth 1) is masked out, so the bright sky never blooms over the scenery.
float3 BT(float2 uv)
{
	float3 c = S(uv);
	if (gFlag.w > 0.5)
	{
		float d = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
		c *= (d < 0.99999) ? 1.0 : 0.0;
	}
	return c;
}

float4 PSBright(float2 uv : TEXCOORD0) : COLOR
{
	float2 p = gPx.xy;
	float3 c = BT(uv + p * float2(-1, -1)) + BT(uv + p * float2(1, -1))
	         + BT(uv + p * float2(-1,  1)) + BT(uv + p * float2(1,  1));
	c *= 0.25;
	float l = dot(c, float3(0.299, 0.587, 0.114));
	float w = saturate((l - gBloom.y) / max(gBloom.z, 0.001));
	w = w * w * (3.0 - 2.0 * w);
	return float4(c * w, 1);
}

float4 PSBlur(float2 uv : TEXCOORD0) : COLOR
{
	float2 d = gDir.xy;
	float3 s = S(uv) * 0.2270270270;
	s += (S(uv + d * 1.3846153846) + S(uv - d * 1.3846153846)) * 0.3162162162;
	s += (S(uv + d * 3.2307692308) + S(uv - d * 3.2307692308)) * 0.0702702703;
	return float4(s, 1);
}

// ---------------- fog + heat haze ----------------
float LinZ(float d)
{
	float n = gPx.z;
	float f = gPx.w;
	return n * f / (f - d * (f - n));                      // z = n*f / (f - d*(f-n))
}

float HazeT(float d)                                       // 0 near .. 1 far, 0 on the sky
{
	float t = saturate((LinZ(d) - gHaze.z) / max(gHaze.w - gHaze.z, 1.0));
	return (d < 0.99999) ? t : 0.0;
}

float2 HazeOffset(float2 uv, float t)
{
	float band = 1.0 - smoothstep(0.0, 0.40, abs(uv.y - 0.55));     // strongest in the band around the horizon
	float ph = gHaze.y;
	float2 o;
	o.x = sin(uv.y * 70.0 + ph * 2.7 + sin(uv.x * 9.0 + ph) * 1.5);
	o.y = sin(uv.x * 55.0 - ph * 2.1 + sin(uv.y * 11.0 - ph * 0.7) * 1.5) * 0.35;
	return o * gHaze.x * t * band;
}

float4 PSAtmos(float2 uv : TEXCOORD0) : COLOR
{
	float d0 = tex2Dlod(sDepth, float4(uv, 0, 0)).r;
	float z0 = LinZ(d0);
	float2 uvS = uv;

	// heat haze: wobble the lookup of distant pixels. Never pull in foreground pixels (no smearing at silhouettes)
	if (gFlag.z > 0.5)
	{
		float t0 = HazeT(d0);
		float2 off = HazeOffset(uv, t0);
		float d1 = tex2Dlod(sDepth, float4(uv + off, 0, 0)).r;
		float k = saturate(HazeT(d1) / max(t0, 0.001));
		uvS = uv + off * k;
	}

	float3 col = S(uvS);

	// depth fog in the sky colour, warm towards the sun. The sky itself (depth 1) is left alone.
	if (gFlag.x > 0.5 && d0 < 0.99999)
	{
		float3 fc = gFogC.rgb;
		float scat = 0.0;
		if (gSun.z > 0.5)
		{
			float2 dv = uv - gSun.xy;
			dv.x *= gSun.w;
			float s = saturate(1.0 - length(dv) * 1.4);
			scat = s * s * gFogP.w;
		}
		fc = fc + gSunC.rgb * scat;
		float f = saturate((z0 - gFogP.x) / max(gFogP.y - gFogP.x, 1.0));
		f = pow(f, gFogP.z) * gFogC.a;
		col = lerp(col, fc, saturate(f));
	}

	if (gFlag.y > 0.5)
	{
		float3 b = saturate(tex2Dlod(sBloom, float4(uv, 0, 0)).rgb * gBloom.x);
		col = 1.0 - (1.0 - saturate(col)) * (1.0 - b);        // screen blend: brightens, never clips to flat white
	}

	return float4(col, 1);
}
