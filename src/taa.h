#pragma once
// Temporal anti-aliasing for GTA SA 1.0 US (D3D9). Sub-pixel camera jitter (RwCamera::viewOffset), history buffer,
// camera-motion reprojection from the AO module's INTZ depth, variance clipping. Needs [AO] ssao = 1.
struct TAAConfig {
	int enable;
	int blend;        // base weight of the new frame in percent (10 = smooth, 20 = less ghosting)
	int clipGamma;    // neighbourhood clip width in percent of sigma (125 default; lower = less ghosting, more shimmer)
	int jitter;       // jitter amplitude in percent of +-0.5 px (100 default)
	int toggleKey;    // virtual key, 0 = none
	int log;
};
void TAA_Init(const TAAConfig *cfg);
// Runs after the colour filter + AO, before the sharpen / HUD. Returns true if the frame was resolved (then FXAA/SMAA are skipped).
bool TAA_Apply(void);
void TAA_OnReset(void);   // release default-pool resources
