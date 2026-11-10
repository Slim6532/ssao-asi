#pragma once
// SMAA 1x anti-aliasing: three full-screen passes over the finished frame (before the HUD).
// Call Post_Apply once per frame, after the colour filter and the AO.

struct PostConfig {
	int smaa;        // 1 = SMAA on (needs SMAA.hlsl + AreaTex.h + SearchTex.h in src\ at build time)
	int toggleKey;   // virtual-key code, toggles SMAA on/off (for comparing). 0 = none
};

void Post_Apply(const PostConfig *cfg);
