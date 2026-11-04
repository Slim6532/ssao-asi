#pragma once
// Hardware occlusion culling for map objects (buildings), D3D9 occlusion queries, GTA SA 1.0 US.
// Every candidate building atomic gets a world-space cube proxy tested against the finished depth buffer at
// the end of the frame (before the colour filter / AO). The result is read 1+ frames later without ever
// blocking; an atomic is skipped only after it was fully hidden for `hiddenFrames` results in a row.
struct OcclusionConfig {
	int enable;
	int minDistance;    // never cull closer than this (metres, measured to the sphere surface)
	int minRadius;      // ignore tiny objects (not worth a query), x10 => 15 = 1.5 m
	int maxRadius;      // ignore huge objects (terrain-like), metres
	int margin;         // proxy cube scale in percent of the bounding sphere radius, >= 100
	int hiddenFrames;   // consecutive fully-hidden results required before culling
	int interval;       // re-test each object every N frames
	int maxQueries;     // queries issued per frame
	int toggleKey;      // virtual key, 0 = none
	int log;
};
struct RpAtomic;
void Occlusion_Init(const OcclusionConfig *cfg);
// true = skip drawing this building atomic. Also registers it as a candidate for this frame's tests.
bool Occlusion_Cull(RpAtomic *atomic);
// call once per frame after the scene is drawn (depth buffer complete), before the AO/post passes
void Occlusion_EndFrame(void);
// release queries (device reset)
void Occlusion_OnReset(void);
