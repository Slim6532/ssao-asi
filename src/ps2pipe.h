#pragma once
// SkyGfx 4.2b "PS2" building pipeline (map objects: day/night vertex colour blend in a shader, PS2 modulate,
// env-map pass, per-mesh dual pass) as a module of DualPass. GTA SA 1.0 US.
struct PS2PipeConfig {
	int enable;       // replace the game's two building pipelines
	int modulate;     // PS2 colour scale 255/128 on textured meshes
	int dualPass;     // per-mesh dual pass inside the pipe (uses zwriteThreshold)
	int zwriteThreshold;
};
// call from DllMain (hooks the two pipeline creators; the pipes are created later by the game)
void PS2Pipe_Install(const PS2PipeConfig *cfg);
