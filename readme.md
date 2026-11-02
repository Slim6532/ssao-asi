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
