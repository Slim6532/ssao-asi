# DualPass + HBAO+ AO + SMAA

Standalone .asi for GTA SA 1.0 US. Only three features are left:

1. **Dual pass** - SkyGfx 4.2b's PS2 dual-pass transparency (default objects, peds, vehicles, buildings, grass). `[DualPass]` in the ini.
2. **Ambient occlusion** - native port of NVIDIA HBAO+ 3.1, no ReShade. `[AO]` in the ini. Needs game antialiasing OFF. F9 cycles AO on / AO-only view / off. Log: `dualpass_ssao.log`.
3. **SMAA 1x** - 3 passes after the AO, before the HUD. `[Post]` in the ini. F10 toggles it. Log: `dualpass_post.log`.

## SMAA files (required for SMAA)
Add three files from https://github.com/iryoku/smaa to `src/` before building: `SMAA.hlsl`, `AreaTex.h`, `SearchTex.h` (the last two are in `Textures/`). Without them the mod still builds, but SMAA does nothing.

## Build on GitHub
Upload this folder to a repo (keep `.github/workflows/main.yml`), run the "main" workflow, download the `dualpass` artifact (dualpass.asi + dualpass.ini).

Don't run this together with skygfx.dll - they patch the same spots.

## AO quality 3: Scalable Ambient Obscurance
`quality = 3` in `[AO]` switches the AO pass to SAO (McGuire, Mara, Luebke, NVIDIA research, HPG 2012): 9 spiral taps per pixel at half resolution instead of HBAO+'s 8 directions x 4 steps, so it is roughly 3-4x cheaper. It shares the blur and the depth/normal-aware upsample with HBAO+. `radius`, `intensity`, `power`, `bias`, `fadeDistance` and `maxRadiusPx` apply; if it looks too dark or too light, change `intensity` first. Set `quality = 1` for HBAO+ again.
