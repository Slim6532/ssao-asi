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
- Horizon-based AO at half resolution, separable depth-aware blur, depth-aware upsample.
- `quality` 0/1/2 picks the sample count. `minFps` is an FPS stabilizer: quality drops automatically when FPS stays below it, and recovers when there is headroom.
- Turn game antialiasing OFF (the depth buffer is swapped for an INTZ texture, which can't be multisampled).
- Shaders (`src/ssao.hlsl`) are compiled by `fxc` during the build (GitHub Actions does it for you).
- If nothing shows up, check `dualpass_ssao.log` in the GTA folder.
