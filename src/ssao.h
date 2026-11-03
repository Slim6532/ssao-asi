#pragma once
// Native ambient occlusion for GTA SA 1.0 US (D3D9), a port of NVIDIA HBAO+ 3.1. No ReShade.
// Call SSAO_Apply once per frame, after the 3D scene is drawn and before the HUD.

struct SSAOConfig {
	int   quality;       // 0 = fast (4 directions, half res), 1 = HBAO+ (8 directions, half res), 2 = HBAO+ at FULL resolution
	float radius;        // AO radius in game units (1 unit ~ 1 metre)
	float intensity;     // multiplier on both scales, 1 = NVIDIA default
	float power;         // NVIDIA PowerExponent: final AO = pow(AO, power), 1..4, default 2
	float bias;          // NVIDIA Bias 0..0.5 (hides low-tessellation artifacts), default 0.1
	float smallScale;    // NVIDIA SmallScaleAO 0..2 (first step, crisp contact darkening), default 1
	float largeScale;    // NVIDIA LargeScaleAO 0..2 (steps 2..4, soft broad shading), default 1
	float blurSharpness; // NVIDIA Blur.Sharpness 0..16 (higher = blur preserves edges more), default 16
	float normalPower;   // how strictly the blur/upsample match surface normals (1..32, default 8)
	float fadeDist;      // AO fades out towards this distance
	int   maxRadiusPx;   // cap on the screen-space radius (keeps texture fetches cache friendly)
	float minFps;        // adaptive quality: drop quality when FPS stays below this. 0 = off
	int   toggleKey;     // virtual-key code. Press to cycle: AO on -> AO-only debug view -> AO off. 0 = none
	int   debugView;     // 1 = start in the AO-only debug view
	int   interval;      // AO is recomputed every N frames (cheap composite every frame). 0 = auto by FPS
};

void SSAO_Apply(float nearZ, float farZ, float viewWinX, float viewWinY, const SSAOConfig *cfg);

// The INTZ depth texture of the main scene (NULL until the AO module has swapped it in, or when INTZ is unavailable).
// Other passes (atmosphere: fog / heat haze) sample it. Valid only while SSAO is enabled.
struct IDirect3DTexture9;
IDirect3DTexture9 *SSAO_GetDepthTexture(void);
