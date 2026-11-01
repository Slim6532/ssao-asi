#pragma once
// FXAA + CAS-style sharpening, one full-screen pass over the finished frame (before the HUD).
// Call Post_Apply once per frame, after the colour filter and the AO.

struct PostConfig {
	int   fxaa;          // 1 = FXAA on
	float edgeThresh;    // FXAA edge threshold as a ratio of local brightness (0.125 = default, lower = more AA)
	int   sharpen;       // 1 = sharpening on
	float sharpAmount;   // 0..1
	int   toggleKey;     // virtual-key code, toggles the whole post pass on/off (for comparing). 0 = none
};

void Post_Apply(const PostConfig *cfg);
