#pragma once
// Fluffy Clouds for GTA SA 1.0 US, ported from "fluffyclouds-sa" (TheArtemMaps, based on re3's GTA3/VC clouds)
// into DualPass. No plugin-sdk needed: raw game addresses only. Works with ANY timecyc (vanilla, 24h, mods)
// because the colours are read from the game's live timecyc state (CTimeCycle::m_CurrentColours).

struct CloudsConfig {
	int   enable;          // 1 = draw fluffy clouds
	int   alpha;           // 0..255 base cloud alpha (original: 160)
	int   brightness;      // percent multiplier on the timecyc colour (100 = unchanged)
	int   whiten;          // 0..100: blend the timecyc colour towards white (0 = pure timecyc colour)
	int   size;            // percent multiplier on the sprite size (100 = original)
	int   distance;        // percent: how far out the cloud ring sits (100 = original ~200 units, 300 = near the horizon)
	int   height;          // base height of the cloud layer in game units (original: 50)
	int   highlight;       // 1 = additive sun highlight pass
	int   windSpeed;       // percent, drift speed (original used CWeather::Wind, roughly 0..120)
	int   fadeAtHeight;    // 1 = fade clouds out when the camera is far above the layer (planes, jetpack)
	int   colorOffset;     // byte offset of "fluffy clouds bottom RGB" inside CColourSet. -1 = built-in default
	int   fallbackR, fallbackG, fallbackB;   // used only when the timecyc colour can't be read
	int   log;             // 1 = write dualpass_clouds.log (raw timecyc dump, useful to verify the offset)
};

void Clouds_Install(const CloudsConfig *cfg);   // call once from DllMain (just stores the settings)
void Clouds_Draw(void);                         // call once per frame from the colour-filter hook, before the AO
