#pragma once
// Atmosphere pass for GTA SA 1.0 US (D3D9): depth fog, bloom and heat haze in screen space.
// Call Atmos_Apply once per frame, after the AO and before the post (FXAA/SMAA) pass.
// Fog and heat haze need the INTZ depth texture of the AO module (ssao = 1); bloom works without it.

struct AtmosConfig {
	int enable;
	int toggleKey;           // virtual-key code, toggles the whole pass on/off (for comparing). 0 = none
	// --- fog (depth based, colour from the live timecyc sky colour) ---
	int fog;
	int fogStrength;         // percent of the maximum haze (0-100)
	int fogStart;            // percent of the timecyc fog start distance
	int fogEnd;              // percent of the timecyc far clip: where the haze is at full strength
	int fogCurve;            // x100: 100 = linear, 150-250 = the haze gathers in the distance
	int sunScatter;          // percent: warm glow of the haze towards the sun
	// --- bloom ---
	int bloom;
	int bloomIntensity;      // percent
	int bloomThresholdDay;   // 0-100: how bright a pixel has to be to glow, in daylight
	int bloomThresholdNight; // same at night (lower = lit windows and lamps glow)
	int bloomNightBoost;     // percent extra intensity at night
	int bloomSize;           // percent: size of the glow
	// --- heat haze ---
	int haze;
	int hazeStrength;        // 0-100
	int hazeStart;           // game units: distortion starts here, full strength at hazeEnd
	int hazeEnd;
	int hazeSpeed;           // percent
	int log;
};

void Atmos_Apply(float nearZ, float farZ, const AtmosConfig *cfg);
