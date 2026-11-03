#pragma once
// Fluffy Clouds for GTA SA 1.0 US, ported from "fluffyclouds-sa" (TheArtemMaps, based on re3's GTA3/VC clouds)
// into DualPass. No plugin-sdk needed: raw game addresses only. Works with ANY timecyc mod because the colours
// (cloud colour, sky colour, sun colour, time of day) are read from the game's live timecyc state.

struct CloudsConfig {
	int   enable;          // 1 = draw fluffy clouds
	int   alpha;           // 0..255 base cloud alpha in daylight (original: 160)
	int   nightAlpha;      // 0..255 cloud alpha at night (blends with the time of day)
	int   nightBrightness; // percent: how dark the clouds get at night
	int   nightTint;       // 0..100: how much cool moonlit blue-grey the night clouds get
	int   sunGlow;         // percent: strength of the sun-coloured lighting (needs highlight = 1)
	int   brightness;      // percent multiplier on the timecyc colour (100 = unchanged)
	int   whiten;          // 0..100: blend the timecyc colour towards white in daylight
	int   size;            // percent multiplier on the sprite size (100 = original)
	int   distance;        // percent: how far out the cloud ring sits (100 = original ~200 units)
	int   height;          // base height of the cloud layer in game units (original: 50)
	int   highlight;       // 1 = sun lighting + additive sun highlight pass
	int   windSpeed;       // percent, drift speed (original used CWeather::Wind). <0 counts as 0
	int   fadeAtHeight;    // 1 = fade clouds out when the camera is far above the layer (planes, jetpack)
	// --- motion / transitions ---
	int   windMode;        // 1 = clouds drift across the sky in one wind direction (wrap-around), 0 = original (ring rotates around you)
	int   windDirection;   // degrees, world direction the clouds drift towards (windMode 1)
	int   parallax;        // percent of YOUR movement that shifts the clouds (0 = painted on the sky, 3-5 = subtle depth)
	int   lifespan;        // average seconds a cloud lives before it dissolves and re-forms elsewhere. 0 = clouds never change
	int   fadeTime;        // ms: fade out / in when entering / leaving interiors (0 = instant)
	int   smoothing;       // ms: eases sudden time-of-day / weather / timecyc changes (0 = instant)
	// --- lighting / look ---
	int   shading;         // 0..100: top-lit / dark-underside contrast of each cloud (0 = flat like the original)
	int   horizonFade;     // 0..100: clouds near the horizon fade out and blend into the sky colour
	int   variety;         // 0..100: random per-session jitter of cloud positions, sizes, rotation, alpha (0 = original layout)
	// --- second layer ---
	int   layer2;          // 1 = add a higher, farther, slower, softer second cloud layer
	int   layer2Count;     // 0..48 sprites in the second layer
	int   layer2Alpha;     // percent of the main alpha
	int   layer2Height;    // game units above the main layer
	int   layer2Distance;  // percent of the main ring distance
	int   layer2Speed;     // percent of the main drift speed
	// --- advanced ---
	int   colorOffset;     // byte offset of "fluffy clouds bottom RGB" inside CColourSet. -1 = built-in default
	int   fallbackR, fallbackG, fallbackB;   // used only when the timecyc colour can't be read
	int   log;             // 1 = write dualpass_clouds.log
};

void Clouds_Install(const CloudsConfig *cfg);   // call once from DllMain (just stores the settings)
void Clouds_Draw(void);                         // call once per frame from the colour-filter hook, before the AO
