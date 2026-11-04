# DualPass (SkyGfx dual pass only)

Standalone .asi with only the PS2 dual-pass transparency from SkyGfx 4.2b (by aap). GTA SA 1.0 US.

Covers default objects, peds, vehicles, buildings/map objects (palms, fences) and grass. Everything is done at atomic level, so no RenderWare struct layouts are guessed.

## Build (Windows, 32-bit)
1. Set env var `RWSDK36` to `<RWSDK>\RW36\Graphics\rwsdk\include\d3d9`
2. `premake5 vs2015` (or vs2019/vs2022), open `build/dualpass.sln`, pick Release / Win32, build
3. Copy `bin/Release/dualpass.asi` and `dualpass.ini` into your GTA SA folder (needs an ASI loader)

No shaders, no DirectX SDK and no injector submodule needed.
Don't run this together with skygfx.dll - they patch the same spots.

## Build on GitHub (no installs)
1. Make a free GitHub repo, upload everything in this folder (keep `.github/workflows/build.yml`)
2. Actions tab -> "build" -> Run workflow (it also runs on every upload)
3. When it's green, open the run and download the `dualpass` artifact -> dualpass.asi + dualpass.ini

## Extras
`[Extras]` in dualpass.ini: disableClouds, disableGamma, blur edges, game radiosity, grass backface cull / placement fix.

## Ambient occlusion (native, HBAO-style)
`[AO]` in dualpass.ini. No ReShade needed.
- Horizon-based AO at half resolution, separable depth-aware blur, depth-aware upsample. AO and linear depth travel together in one float target, so the blur/upsample passes never re-read the depth buffer.
- `quality` 0/1/2 picks the sample count. Distant pixels automatically use half the directions.
- `updateInterval` (0 = auto): at very high FPS the AO is recomputed every 2nd/3rd frame and reused in between; the composite still runs every frame.
- `quality` 0/1/2 picks the sample count. `minFps` is an FPS stabilizer: quality drops automatically when FPS stays below it, and recovers when there is headroom.
- Turn game antialiasing OFF (the depth buffer is swapped for an INTZ texture, which can't be multisampled).
- Shaders (`src/ssao.hlsl`) are compiled by `fxc` during the build (GitHub Actions does it for you).
- If nothing shows up, check `dualpass_ssao.log` in the GTA folder.
- Press F9 in game (`toggleKey`) to cycle AO on / AO-only view / AO off. The AO-only view helps tell AO artifacts from map textures.

## FXAA + sharpening
`[Post]` in dualpass.ini. One extra full-screen pass (a StretchRect copy of the frame + one draw), applied after the colour filter and the AO and before the HUD.
- FXAA only does extra work on edge pixels; sharpening is CAS-style (adaptive, avoids halos). Both are in the same shader (`src/post.hlsl`).
- Press F10 (`toggleKey`) in game to switch it on/off and compare.

## SMAA (optional)
Add three files from https://github.com/iryoku/smaa to `src/` before building: `SMAA.hlsl`, `Textures/AreaTex.h`, `Textures/SearchTex.h`.
The build then also compiles `src/post_smaa.hlsl` (3 passes: edges, blend weights, neighbourhood blend, SMAA 1x HIGH preset, luma edges)
and `smaa = 1` in `[Post]` uses it. Without these files the mod still builds and uses FXAA. `dualpass_post.log` says which one runs.

## AO = NVIDIA HBAO+ 3.1 port
`src/ssao.hlsl` ports the shaders from the HBAO+ 3.1 SDK to D3D9 / ps_3_0: the same per-sample AO term
`saturate(N.V - bias) * falloff` (summed, small-scale first step + large-scale steps 2-4), 8 directions x 4 steps on the
4-pixel lattice, the SDK's own 4x4 random table, the cross-bilateral blur with depth-slope compensation and
`pow(AO, power)` after the blur. D3D9 has no texture arrays / geometry shaders, so instead of the 16 deinterleaved slices
every pixel samples the same lattice directly. `quality = 2` renders at full resolution like the original.

## Fluffy Clouds (built in)
`[FluffyClouds]` in dualpass.ini. Port of fluffyclouds-sa (GTA3/VC-style cloud sprites with the sun highlight), no plugin-sdk needed.
- Copy `resources/fluffycloudsSA.txd` to `<GTA>\models\fluffycloudsSA.txd`.
- Works with ANY timecyc mod: the colour is read every frame from the game's current timecyc state, so vanilla, 24h and custom timecycs all work and the clouds follow time of day and weather.
- Clouds are depth-tested but never write depth, so the HBAO pass doesn't treat them as geometry.
- Don't run it together with the standalone `FluffyCloudsSA.asi`.
- `src/clouds.cpp` lists every game address it uses at the top; `dualpass_clouds.log` shows what it read.

## Atmosphere (fog + bloom)
`[Atmosphere]` in dualpass.ini, source in `src/atmos.cpp` + `src/atmos.hlsl`. One extra screen-space pass after the AO and before FXAA/SMAA.
- Fog is depth based, coloured with the live timecyc sky colour, warm towards the sun. Needs `ssao = 1` (it uses the AO module's INTZ depth).
- Bloom (quarter-res bright-pass + separable blur) makes lit windows, lamps, headlights and siren lights glow, more at night.
- Not included (needs hooks into the game's own lighting / particle code): new headlight beams, siren lights, neon interior lights with shadows, soft particles.

## PS2 building pipeline (from SkyGfx 4.2b)
`[PS2Pipe]` in dualpass.ini, source in `src/ps2pipe.cpp` + `src/ps2_*.hlsl`. Replaces the game's two building pipelines (plain + day/night) like SkyGfx's `buildingPipe = 0`: day/night vertex colours are blended in a vertex shader, PS2 modulate, env-map pass, dual pass per mesh. The pipelines are created by the game after the ASI loads, so the hooks are installed in `DllMain` and a restart is needed. Not ported: Xbox/sphere-map variants, detail maps (TexDB), vehicles.
`src/rwd3d9_wrap.h` holds the RW D3D9 wrappers and the three RW struct layouts (`RxD3D9ResEntryHeader`, `RxD3D9InstanceData`, vertex stream) - written from the RW 3.6 SDK layout, check them if your headers differ.

## Occlusion culling
`[Occlusion]` in dualpass.ini, source in `src/occlusion.cpp`. D3D9 occlusion queries: at the end of the frame each map-object atomic's bounding cube is drawn against the finished depth buffer (colour/depth writes off); the result is read without blocking 1-2 frames later. An object is skipped only after `hiddenFrames` hidden results in a row, never closer than `minDistance`, never on env-map cameras, and everything is reset on camera cuts and device reset. F8 toggles it, `dualpass_occlusion.log` shows tracked/hidden counts. Peds, vehicles and grass are not culled.
