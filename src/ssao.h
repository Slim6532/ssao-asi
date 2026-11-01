#pragma once
// Native HBAO-style ambient occlusion for GTA SA 1.0 US (D3D9). No ReShade.
// Call SSAO_Apply once per frame, after the 3D scene is drawn and before the HUD.

struct SSAOConfig {
	int   quality;       // 0 = low (3x3 taps), 1 = medium (4x4), 2 = high (6x5)
	float radius;        // AO radius in game units (1 unit ~ 1 metre)
	float intensity;     // 1 = normal
	float power;         // contrast exponent (1 = linear)
	float bias;          // horizon bias 0..0.5 (hides self-occlusion on curved surfaces)
	float fadeDist;      // AO fades out towards this distance
	int   maxRadiusPx;   // cap on the screen-space radius (keeps texture fetches cache friendly)
	float minFps;        // adaptive quality: drop quality when FPS stays below this. 0 = off
	int   toggleKey;     // virtual-key code. Press to cycle: AO on -> AO-only debug view -> AO off. 0 = none
	int   debugView;     // 1 = start in the AO-only debug view
};

void SSAO_Apply(float nearZ, float farZ, float viewWinX, float viewWinY, const SSAOConfig *cfg);
