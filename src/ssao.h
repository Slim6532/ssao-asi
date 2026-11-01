#pragma once
// Native SSAO for GTA SA 1.0 US (D3D9). No ReShade.
// Call once per frame, after the 3D scene is drawn and before the HUD.
//   nearZ/farZ      : RwCamera nearPlane / farPlane
//   viewWinX/Y      : RwCamera viewWindow.x / .y (half extent of the view plane at z = 1)
//   radius          : AO radius in game units (1 unit ~ 1 metre)
//   intensity       : 0..2+, 1 = normal
//   fadeDist        : AO fades out towards this distance (game units)
void SSAO_Apply(float nearZ, float farZ, float viewWinX, float viewWinY,
                float radius, float intensity, float fadeDist);
