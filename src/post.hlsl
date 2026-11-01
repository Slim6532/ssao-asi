// post.hlsl - FXAA + CAS-style sharpening in ONE full-screen pass for GTA SA (D3D9, ps_3_0)
//
//   VSMain : fullscreen quad
//   PSPost : reads a copy of the finished frame (sScene), writes the anti-aliased / sharpened result
//            back over the frame. Non-edge pixels cost 5 texture reads; only edges do the extra taps.

sampler2D sScene : register(s0);   // copy of the finished frame (bilinear, clamp)

float4 gPx   : register(c0);   // x = 1/width, y = 1/height, z = FXAA edge threshold (ratio), w = sharpen strength 0..1
float4 gFlag : register(c1);   // x = FXAA on, y = sharpen on
float4 gRT   : register(c8);   // VS only: 1/width, 1/height of the render target

struct VSOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(float2 p : POSITION)
{
	VSOut o;
	o.pos = float4(p.x - gRT.x, p.y + gRT.y, 0, 1);      // D3D9 half-pixel offset
	o.uv  = p * float2(0.5, -0.5) + 0.5;
	return o;
}

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 T(float2 uv) { return tex2Dlod(sScene, float4(uv, 0, 0)).rgb; }

float4 PSPost(float2 uv : TEXCOORD0) : COLOR
{
	float2 px  = gPx.xy;
	float3 cM  = T(uv);
	float3 cNW = T(uv + float2(-px.x, -px.y));
	float3 cNE = T(uv + float2( px.x, -px.y));
	float3 cSW = T(uv + float2(-px.x,  px.y));
	float3 cSE = T(uv + float2( px.x,  px.y));

	float lM  = Luma(cM);
	float lNW = Luma(cNW);
	float lNE = Luma(cNE);
	float lSW = Luma(cSW);
	float lSE = Luma(cSE);
	float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
	float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

	float3 res = cM;

	// ---- FXAA: only on pixels that sit on a visible edge ----
	[branch] if (gFlag.x > 0.5 && (lMax - lMin) >= max(0.0312, lMax * gPx.z))
	{
		float2 dir;
		dir.x = -((lNW + lNE) - (lSW + lSE));
		dir.y =  ((lNW + lSW) - (lNE + lSE));
		float dirReduce = max((lNW + lNE + lSW + lSE) * (0.25 * (1.0 / 8.0)), 1.0 / 128.0);
		float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
		dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;

		float3 rgbA = 0.5 * (T(uv + dir * -0.16666667) + T(uv + dir * 0.16666667));
		float3 rgbB = rgbA * 0.5 + 0.25 * (T(uv + dir * -0.5) + T(uv + dir * 0.5));
		float  lB   = Luma(rgbB);
		res = (lB < lMin || lB > lMax) ? rgbA : rgbB;
	}

	// ---- CAS-style adaptive sharpen: less on already-contrasty pixels, so no halos / ringing ----
	[branch] if (gFlag.y > 0.5)
	{
		float3 mn  = min(min(min(cNW, cNE), min(cSW, cSE)), res);
		float3 mx  = max(max(max(cNW, cNE), max(cSW, cSE)), res);
		float3 amp = saturate(min(mn, 1.0 - mx) / max(mx, 1e-3));
		amp = sqrt(amp);
		float  peak = -1.0 / lerp(8.0, 5.0, saturate(gPx.w));
		float3 wgt  = amp * peak;
		res = saturate(((cNW + cNE + cSW + cSE) * wgt + res) / (1.0 + 4.0 * wgt));
	}

	return float4(res, 1);
}
