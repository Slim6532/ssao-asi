// clouds.cpp - Fluffy Clouds (GTA3/VC style cloud sprites) for GTA SA 1.0 US, inside DualPass.
// Port of fluffyclouds-sa, extended: sun-lit clouds (bright sun side, dark underside, warm rim), horizon fade,
// per-session variety and a second, higher/farther/slower layer. Self-contained: own sprite batching, own Im2D
// submit, no plugin-sdk.
//
// Hook: called from DualPass' colour-filter hook (0x704D1E, already proven on this setup), after the game's own
// filter and before the AO. The world depth buffer is intact there, so the clouds are depth-tested against it.
// Colours: read every frame from the game's live timecyc state -> any timecyc mod works.
// Depth: clouds are z-tested but never write depth, so DualPass' HBAO never sees them as geometry.
//
// Needs fluffycloudsSA.txd in <GTA>\models\ (textures "cloudmasked" + "cloudhilit").

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <rwcore.h>
#include <rwplcore.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include "MemoryMgr.h"
#include "clouds.h"

// =====================================================================================================
// Game addresses, SA 1.0 US.  VERIFIED = confirmed by the fluffyclouds-sa source, by DualPass itself, or by
// the dualpass_clouds.log dump. The rest come from plugin-sdk / gta-reversed.
// =====================================================================================================
#define ADDR_RwTextureRead    0x7F3AC0   // VERIFIED (fluffyclouds-sa Utility.h)
#define ADDR_RwGlobals        0xC97B24   // VERIFIED (DualPass)
#define ADDR_SceneCamera      0xC1703C   // VERIFIED (DualPass uses it as the scene RwCamera*)
#define ADDR_CalcScreenCoors  0x70CE30   // CSprite::CalcScreenCoors(RwV3d const&, RwV3d*, float*, float*, bool, bool)
#define ADDR_TxdPush          0x7316A0   // CTxdStore::PushCurrentTxd()
#define ADDR_TxdPop           0x7316B0   // CTxdStore::PopCurrentTxd()
#define ADDR_TxdAddSlot       0x731C80   // int CTxdStore::AddTxdSlot(const char*)
#define ADDR_TxdFindSlot      0x731850   // int CTxdStore::FindTxdSlot(const char*)
#define ADDR_TxdSetCurrent    0x7319C0   // CTxdStore::SetCurrentTxd(int)
#define ADDR_TxdLoad          0x7320B0   // bool CTxdStore::LoadTxd(int, const char* path)
#define ADDR_CurrArea         0xB72914   // CGame::currArea (0 = outside world)
#define ADDR_Foggyness        0xC81300   // CWeather::Foggyness (float 0..1)  (address confirmed by MTA's source)
#define ADDR_SunScreenX       0xC3E028   // CCoronas::SunScreenX
#define ADDR_SunScreenY       0xC3E02C   // CCoronas::SunScreenY
#define ADDR_CurrentColours   0xB7C4A0   // CTimeCycle::m_CurrentColours (CColourSet)  VERIFIED by the log dump
// byte offsets inside CColourSet, VERIFIED by the log dump (sky bottom, sun core, fluffy bottom neighbours)
#define OFF_SKY_BOTTOM        0x2A       // 3 x uint16
#define OFF_SUN_CORE          0x30       // 3 x uint16
#define DEFAULT_COLOUR_OFFSET 0x62       // fluffy clouds bottom RGB, 3 x uint16

// RW render states / values (RW 3.6 numbering, same numbers DualPass already uses)
enum {
	RS_TEXTURERASTER = 1, RS_ZTEST = 6, RS_ZWRITE = 8, RS_TEXFILTER = 9,
	RS_SRCBLEND = 10, RS_DESTBLEND = 11, RS_VERTEXALPHA = 12, RS_FOG = 14,
	BL_ONE = 2, BL_SRCALPHA = 5, BL_INVSRCALPHA = 6, TRILIST = 3
};

typedef RwBool (*StateFn)(int, void*);
typedef int (__cdecl *Im2DIndexedFn)(int, void*, int, unsigned short*, int);
typedef bool (__cdecl *CalcScreenFn)(const RwV3d&, RwV3d*, float*, float*, bool, bool);
typedef RwTexture *(__cdecl *TexReadFn)(const char*, const char*);

static void rsSet(int s, void *v) { DWORD g = *(DWORD*)ADDR_RwGlobals; ((StateFn)*(DWORD*)(g + 0x20))(s, v); }
static void rsGet(int s, void *v) { DWORD g = *(DWORD*)ADDR_RwGlobals; ((StateFn)*(DWORD*)(g + 0x24))(s, v); }

static CloudsConfig cfg;
static RwTexture *gTexMask, *gTexHilite;
static bool gInitDone, gFailed;
static float gCloudRotation, gCloudRotation2;
static uint32_t gIndividualRotation;
static LARGE_INTEGER gLastTime;

static void LOG(const char *fmt, ...)
{
	if(!cfg.log) return;
	FILE *f = fopen("dualpass_clouds.log", "a");
	if(!f) return;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fclose(f);
}

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static inline float smooth01(float t) { t = clamp01(t); return t * t * (3.0f - 2.0f * t); }
static inline unsigned pack(int r, int g, int b, int a)
{
	if(r < 0) r = 0; if(r > 255) r = 255;
	if(g < 0) g = 0; if(g > 255) g = 255;
	if(b < 0) b = 0; if(b > 255) b = 255;
	if(a < 0) a = 0; if(a > 255) a = 255;
	return ((unsigned)a << 24) | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

// ---------------- cloud layout ----------------
// Layer 1 = the original 37 sprites (offsets as in the original mod), optionally jittered per session.
// Layer 2 = random sprites, higher, farther and slower.
static const float CoorsOffsetX[37] = {
	0.0f, 60.0f, 72.0f, 48.0f, 21.0f, 12.0f, 9.0f, -3.0f, -8.4f, -18.0f, -15.0f, -36.0f,
	-40.0f, -48.0f, -60.0f, -24.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f,
	100.0f, 100.0f, -30.0f, -20.0f, 10.0f, 30.0f, 0.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f };
static const float CoorsOffsetY[37] = {
	100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f,
	100.0f, 100.0f, 100.0f, 100.0f, -30.0f, 10.0f, -25.0f, -5.0f, 28.0f, -10.0f, 10.0f, 0.0f,
	15.0f, 40.0f, -100.0f, -100.0f, -100.0f, -100.0f, -100.0f, -40.0f, -20.0f, 0.0f, 10.0f, 30.0f, 35.0f };
static const float CoorsOffsetZ[37] = {
	2.0f, 1.0f, 0.0f, 0.3f, 0.7f, 1.4f, 1.7f, 0.24f, 0.7f, 1.3f, 1.6f, 1.0f,
	1.2f, 0.3f, 0.7f, 1.4f, 0.0f, 0.1f, 0.5f, 0.4f, 0.55f, 0.75f, 1.0f, 1.4f,
	1.7f, 2.0f, 2.0f, 2.3f, 1.9f, 2.4f, 2.0f, 2.0f, 1.5f, 1.2f, 1.7f, 1.5f, 2.1f };
#define NUM_L1 37
#define MAX_L2 48
#define MAX_SPRITES (NUM_L1 + MAX_L2)

struct CloudDef {
	float ox, oy, oz;    // offset from the camera before the distance multiplier (oz is height above the layer base)
	float size;          // size multiplier
	float alpha;         // alpha multiplier
	float rot;           // extra rotation (radians)
	int   layer;         // 0 or 1
};
static CloudDef gClouds[MAX_SPRITES];
static int gNumClouds;
static uint32_t gRng = 2463534242u;
static float rnd01(void) { gRng ^= gRng << 13; gRng ^= gRng >> 17; gRng ^= gRng << 5; return (gRng & 0xFFFFFF) / 16777216.0f; }
static float rndS(void) { return rnd01() * 2.0f - 1.0f; }

static void buildLayout(void)
{
	LARGE_INTEGER q; QueryPerformanceCounter(&q);
	gRng = ((uint32_t)q.QuadPart ^ (uint32_t)GetTickCount() * 2654435761u) | 1u;   // different layout every session
	float v = cfg.variety / 100.0f;
	gNumClouds = 0;
	for(int i = 0; i < NUM_L1; i++){
		CloudDef &c = gClouds[gNumClouds++];
		c.ox = 2.0f * CoorsOffsetX[i] + rndS() * 24.0f * v;
		c.oy = 2.0f * CoorsOffsetY[i] + rndS() * 24.0f * v;
		c.oz = 40.0f * CoorsOffsetZ[i] + rndS() * 20.0f * v;
		c.size  = 1.0f + rndS() * 0.30f * v;
		c.alpha = 1.0f - rnd01() * 0.30f * v;
		c.rot   = rndS() * 3.14159f * v;
		c.layer = 0;
	}
	if(cfg.layer2){
		for(int i = 0; i < cfg.layer2Count && gNumClouds < MAX_SPRITES; i++){
			CloudDef &c = gClouds[gNumClouds++];
			float ang = rnd01() * 6.2831853f, r = 150.0f + rnd01() * 150.0f;
			c.ox = cosf(ang) * r; c.oy = sinf(ang) * r;
			c.oz = rnd01() * 70.0f;
			c.size  = 0.60f + rnd01() * 0.80f;
			c.alpha = 0.60f + rnd01() * 0.40f;
			c.rot   = rnd01() * 6.2831853f;
			c.layer = 1;
		}
	}
}

// ---------------- sprite batch ----------------
struct CVert { float x, y, z, rhw; unsigned color; float u, v; };   // RwIm2DVertex, D3D9 layout
static CVert gVerts[MAX_SPRITES * 4];
static unsigned short gIdx[MAX_SPRITES * 6];
static int gNumSprites;
static float gNear2D, gFar2D, gNearClip, gFarClip, gScrW, gScrH;

static void flushSprites(void)
{
	if(gNumSprites <= 0) return;
	DWORD g = *(DWORD*)ADDR_RwGlobals;
	((Im2DIndexedFn)*(DWORD*)(g + 0x34))(TRILIST, gVerts, gNumSprites * 4, gIdx, gNumSprites * 6);
	gNumSprites = 0;
}

// one rotated quad with a colour per corner
static void addQuad(float x, float y, float z, float w, float h, float rot, float recipz, const unsigned col[4])
{
	float c = cosf(rot), s = sinf(rot);
	float xs[4] = { x + w*(-c - s), x + w*(-c + s), x + w*(+c + s), x + w*(+c - s) };
	float ys[4] = { y + h*(-c + s), y + h*(+c + s), y + h*(+c - s), y + h*(-c - s) };
	static const float us[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
	static const float vs[4] = { 0.0f, 1.0f, 1.0f, 0.0f };

	// cull only (no clipping), like the original
	if(xs[0] < 0 && xs[1] < 0 && xs[2] < 0 && xs[3] < 0) return;
	if(ys[0] < 0 && ys[1] < 0 && ys[2] < 0 && ys[3] < 0) return;
	if(xs[0] > gScrW && xs[1] > gScrW && xs[2] > gScrW && xs[3] > gScrW) return;
	if(ys[0] > gScrH && ys[1] > gScrH && ys[2] > gScrH && ys[3] > gScrH) return;
	if(gNumSprites >= MAX_SPRITES) return;

	// camera depth -> screen z (same formula the game's sprite code uses, with the device's real near/far z)
	float screenz = gNear2D + (z - gNearClip) * (gFar2D - gNear2D) * gFarClip / ((gFarClip - gNearClip) * z);
	screenz = clamp01(screenz);
	if(screenz > 0.9999f) screenz = 0.9999f;      // clouds beyond the draw distance stay at the far plane

	CVert *v = &gVerts[gNumSprites * 4];
	for(int i = 0; i < 4; i++){
		v[i].x = xs[i]; v[i].y = ys[i]; v[i].z = screenz; v[i].rhw = recipz;
		v[i].color = col[i]; v[i].u = us[i]; v[i].v = vs[i];
	}
	unsigned short b = (unsigned short)(gNumSprites * 4);
	unsigned short *ix = &gIdx[gNumSprites * 6];
	ix[0] = b+0; ix[1] = b+1; ix[2] = b+2; ix[3] = b+3; ix[4] = b+0; ix[5] = b+2;
	gNumSprites++;
}

// Lit cloud sprite. Each corner gets its own colour:
//   - top of the cloud is lit by the sky, the underside is darker                (shading)
//   - the side facing the sun takes the sun's colour, the far side is a bit darker (sun, sunC, sdx/sdy = screen direction to the sun)
static void addLitSprite(float x, float y, float z, float w, float h, float rot, float recipz,
	const float base[3], float sdx, float sdy, float sun, const float sunC[3], float shading, int a)
{
	float c = cosf(rot), s = sinf(rot);
	const float ux[4] = { -c - s, -c + s, c + s, c - s };
	const float uy[4] = { -c + s, c + s, c - s, -c - s };
	unsigned col[4];
	for(int i = 0; i < 4; i++){
		float nx = ux[i] * 0.7071f, ny = uy[i] * 0.7071f;          // corner offset, -1..1, y points down
		float top = clamp01(0.5f - ny * 0.5f);                       // 1 at the top of the sprite, 0 at the bottom
		float f = 0.62f + 0.46f * top;                               // 0.62 (underside) .. 1.08 (top)
		f = 1.0f + (f - 1.0f) * shading;
		float side = clamp01((nx * sdx + ny * sdy) * 0.5f + 0.5f);   // 1 = corner facing the sun
		float lit = side * side * sun;
		float dark = (1.0f - side) * sun * 0.25f * shading;
		float rgb[3];
		for(int k = 0; k < 3; k++){
			float v = base[k] * f;
			v = lerpf(v, sunC[k], lit * 0.80f);
			rgb[k] = v * (1.0f - dark);
		}
		col[i] = pack((int)rgb[0], (int)rgb[1], (int)rgb[2], a);
	}
	addQuad(x, y, z, w, h, rot, recipz, col);
}

// additive sun glow sprite (the original used a fixed red; this uses the sun's colour)
static void addSpriteHighlight(float x, float y, float z, float w, float h, int r, int g, int b, float recipz, float rot)
{
	unsigned c = pack(r, g, b, 255);
	unsigned col[4] = { c, c, c, c };
	addQuad(x, y, z, w, h, rot, recipz, col);
}

// ---------------- init ----------------
typedef void (__cdecl *V0)(void);
typedef int  (__cdecl *I_PC)(const char*);
typedef void (__cdecl *V_I)(int);
typedef bool (__cdecl *B_IPC)(int, const char*);

static bool initTextures(void)
{
	((V0)ADDR_TxdPush)();
	int slot = ((I_PC)ADDR_TxdFindSlot)("fluffycloudsSA");
	if(slot < 0) slot = ((I_PC)ADDR_TxdAddSlot)("fluffycloudsSA");
	bool ok = ((B_IPC)ADDR_TxdLoad)(slot, "MODELS\\FLUFFYCLOUDSSA.TXD");
	if(ok){
		((V_I)ADDR_TxdSetCurrent)(slot);
		gTexMask   = ((TexReadFn)ADDR_RwTextureRead)("cloudmasked", NULL);
		gTexHilite = ((TexReadFn)ADDR_RwTextureRead)("cloudhilit", NULL);
	}
	((V0)ADDR_TxdPop)();
	if(!ok) LOG("could not load models\\fluffycloudsSA.txd (is it in the models folder?)");
	if(!gTexMask)   LOG("texture 'cloudmasked' not found");
	if(!gTexHilite) LOG("texture 'cloudhilit' not found");
	return ok && gTexMask && gTexHilite;
}

// ---------------- timecyc (works with any timecyc mod) ----------------
// Every timecyc mod, 24h or not, ends up filling the game's current colour set each frame; we read that.
static bool readColour16(int off, int &r, int &g, int &b)
{
	const uint16_t *p = (const uint16_t*)(ADDR_CurrentColours + off);
	if(p[0] > 255 || p[1] > 255 || p[2] > 255) return false;   // not a colour
	r = p[0]; g = p[1]; b = p[2];
	return true;
}
static bool readTimecycColour(int &r, int &g, int &b)
{
	return readColour16(cfg.colorOffset >= 0 ? cfg.colorOffset : DEFAULT_COLOUR_OFFSET, r, g, b);
}

static void dumpColourSet(const char *tag)
{
	if(!cfg.log) return;
	const uint8_t *p = (const uint8_t*)ADDR_CurrentColours;
	char line[256];
	LOG("--- m_CurrentColours dump (%s) ---", tag);
	for(int row = 0; row < 0xC0; row += 16){
		int n = sprintf(line, "+%02X:", row);
		for(int i = 0; i < 16; i++) n += sprintf(line + n, " %02X", p[row + i]);
		LOG("%s", line);
	}
}

// ---------------- per frame ----------------
struct Visible { int idx; RwV3d sp; float sx, sy; float hil; float sdx, sdy; };

void Clouds_Draw(void)
{
	if(!cfg.enable || gFailed) return;
	RwCamera *cam = *(RwCamera**)ADDR_SceneCamera;
	if(!cam || !cam->frameBuffer) return;

	if(!gInitDone){
		gInitDone = true;
		if(!initTextures()){ gFailed = true; return; }
		QueryPerformanceCounter(&gLastTime);
		buildLayout();
		int r, g, b;
		LOG("fluffy clouds started (%d sprites). colour read: %s", gNumClouds, readTimecycColour(r, g, b) ? "ok" : "FAILED (using fallback)");
		dumpColourSet("first frame");
	}

	if(*(int*)ADDR_CurrArea != 0) return;       // CGame::CanSeeOutSideFromCurrArea (interiors)

	// frame time -> the game's timestep units (1.0 = 20 ms), clamped
	LARGE_INTEGER now, freq;
	QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
	float dtms = (float)((now.QuadPart - gLastTime.QuadPart) * 1000.0 / (double)freq.QuadPart);
	gLastTime = now;
	if(dtms < 0.0f) dtms = 0.0f; if(dtms > 100.0f) dtms = 100.0f;
	float timeStep = dtms / 20.0f;
	float timeStepFix = timeStep / (50.0f / 30.0f);
	float wind = (cfg.windSpeed > 0 ? cfg.windSpeed : 0) / 100.0f;

	RwFrame *frame = (RwFrame*)cam->object.object.parent;
	if(!frame) return;
	const RwMatrix *m = &frame->ltm;
	RwV3d campos = m->pos;
	float heading = atan2f(-m->at.x, m->at.y);

	// original CClouds::Update (layer 2 drifts slower)
	float drift = wind * sinf(heading - 0.85f) * 0.001f * timeStepFix;
	gCloudRotation  += drift;
	gCloudRotation2 += drift * (cfg.layer2Speed / 100.0f);
	gIndividualRotation += (uint32_t)((wind * timeStep * 0.5f + 0.3f * timeStepFix) * 60.0f);

	float fog = *(float*)ADDR_Foggyness;
	if(!(fog >= 0.0f && fog <= 1.0f)) fog = 0.0f;

	// time of day + sun colour + horizon colour, all from the live timecyc
	float day = 1.0f; int sunR = 255, sunG = 190, sunB = 130;
	int skyR = 150, skyG = 180, skyB = 220;
	{
		int r, g, b;
		if(readColour16(OFF_SKY_BOTTOM, r, g, b)){
			skyR = r; skyG = g; skyB = b;
			float luma = 0.30f * r + 0.59f * g + 0.11f * b;
			day = smooth01((luma - 35.0f) / 95.0f);
		}
		if(readColour16(OFF_SUN_CORE, r, g, b)){
			int mx = r > g ? r : g; if(b > mx) mx = b;
			if(mx > 40){ sunR = r * 255 / mx; sunG = g * 255 / mx; sunB = b * 255 / mx; }
		}
		sunR += (255 - sunR) / 4; sunG += (255 - sunG) / 4; sunB += (255 - sunB) / 4;   // a bit of white in the glow
	}

	// alpha: day/night blend, fog, optional fade with camera height
	int fluffyalpha = (int)((cfg.nightAlpha + (cfg.alpha - cfg.nightAlpha) * day) * (1.0f - fog));
	float baseZ = (float)cfg.height;
	if(cfg.fadeAtHeight && campos.z > baseZ)
		fluffyalpha -= (int)((campos.z - baseZ) * ((255.0f - (float)cfg.alpha) / 255.0f));
	if(fluffyalpha <= 0) return;
	if(fluffyalpha > 255) fluffyalpha = 255;

	// base cloud colour from the CURRENT timecyc
	int br, bg, bb;
	if(!readTimecycColour(br, bg, bb)){ br = cfg.fallbackR; bg = cfg.fallbackG; bb = cfg.fallbackB; }
	int rawR = br, rawG = bg, rawB = bb;
	br = br * cfg.brightness / 100; bg = bg * cfg.brightness / 100; bb = bb * cfg.brightness / 100;
	if(br > 255) br = 255; if(bg > 255) bg = 255; if(bb > 255) bb = 255;
	int wh = (int)(cfg.whiten * day);          // pull towards white in daylight only
	if(wh > 0){
		br += (255 - br) * wh / 100; bg += (255 - bg) * wh / 100; bb += (255 - bb) * wh / 100;
	}
	{	// night: darker, with a cool moonlit blue-grey tint
		float n = 1.0f - day;
		float dim = 1.0f - n * (1.0f - cfg.nightBrightness / 100.0f);
		float tint = n * cfg.nightTint / 100.0f;
		br = (int)((br * dim) * (1.0f - tint) + 70.0f * tint);
		bg = (int)((bg * dim) * (1.0f - tint) + 85.0f * tint);
		bb = (int)((bb * dim) * (1.0f - tint) + 120.0f * tint);
	}
	if(cfg.log){
		static DWORD lastLog; static int nLog;
		DWORD t = GetTickCount();
		if(nLog < 60 && t - lastLog > 5000){
			lastLog = t; nLog++;
			LOG("timecyc fluffy RGB = %d %d %d -> used %d %d %d, day %.2f, sun %d %d %d, sky %d %d %d, fog %.2f, alpha %d",
				rawR, rawG, rawB, br, bg, bb, day, sunR, sunG, sunB, skyR, skyG, skyB, fog, fluffyalpha);
		}
	}

	gScrW = (float)cam->frameBuffer->width;
	gScrH = (float)cam->frameBuffer->height;
	{
		DWORD g = *(DWORD*)ADDR_RwGlobals;
		gNear2D = *(float*)(g + 0x18);   // RwDevice zBufferNear / zBufferFar
		gFar2D  = *(float*)(g + 0x1C);
	}
	gNearClip = cam->nearPlane;
	gFarClip  = cam->farPlane;
	if(gFarClip <= gNearClip) return;

	// ----- save state, set ours -----
	int sZTest, sZWrite, sSrc, sDst, sFog, sVA, sFilter; void *sRaster = NULL;
	rsGet(RS_ZTEST, &sZTest); rsGet(RS_ZWRITE, &sZWrite);
	rsGet(RS_SRCBLEND, &sSrc); rsGet(RS_DESTBLEND, &sDst);
	rsGet(RS_FOG, &sFog); rsGet(RS_VERTEXALPHA, &sVA); rsGet(RS_TEXFILTER, &sFilter);
	rsGet(RS_TEXTURERASTER, &sRaster);

	rsSet(RS_ZTEST, (void*)TRUE);
	rsSet(RS_ZWRITE, (void*)FALSE);          // never write depth: keeps the AO pass from treating clouds as geometry
	rsSet(RS_FOG, (void*)FALSE);
	rsSet(RS_VERTEXALPHA, (void*)TRUE);
	rsSet(RS_TEXFILTER, (void*)2);           // linear

	float rotSin1 = sinf(gCloudRotation),  rotCos1 = cosf(gCloudRotation);
	float rotSin2 = sinf(gCloudRotation2), rotCos2 = cosf(gCloudRotation2);
	float dist = cfg.distance / 100.0f;
	float dist2 = dist * cfg.layer2Distance / 100.0f;
	float sunX = *(float*)ADDR_SunScreenX, sunY = *(float*)ADDR_SunScreenY;
	bool sunOk = cfg.highlight && sunX > -20000.0f && sunX < 20000.0f && sunY > -20000.0f && sunY < 20000.0f;
	float indRot = (uint16_t)gIndividualRotation / 65336.0f * 6.28f;
	float shading = cfg.shading / 100.0f;
	float hfade = cfg.horizonFade / 100.0f;
	float glow = cfg.sunGlow / 100.0f;
	const float sunC[3] = { (float)sunR, (float)sunG, (float)sunB };
	CalcScreenFn calc = (CalcScreenFn)ADDR_CalcScreenCoors;

	// ----- project every cloud -----
	static Visible vis[MAX_SPRITES];
	int nVis = 0;
	float distLimit = (3.0f * gScrW) / 4.0f;
	for(int i = 0; i < gNumClouds; i++){
		const CloudDef &c = gClouds[i];
		float dm = c.layer ? dist2 : dist;
		float rs = c.layer ? rotSin2 : rotSin1, rc = c.layer ? rotCos2 : rotCos1;
		float px = c.ox * dm, py = c.oy * dm;
		RwV3d wp;
		wp.x = px * rc + py * rs + campos.x;
		wp.y = px * rs - py * rc + campos.y;
		wp.z = c.oz + baseZ + (c.layer ? (float)cfg.layer2Height : 0.0f);
		float szx, szy; RwV3d sp;
		if(!calc(wp, &sp, &szx, &szy, false, false)) continue;

		Visible &v = vis[nVis++];
		v.idx = i; v.sp = sp; v.sx = szx; v.sy = szy; v.hil = 0.0f; v.sdx = 0.0f; v.sdy = -1.0f;
		if(sunOk){
			float dx = sunX - sp.x, dy = sunY - sp.y;
			float sd = sqrtf(dx*dx + dy*dy);
			if(sd > 1.0f){ v.sdx = dx / sd; v.sdy = dy / sd; }
			float nearSun = sd < distLimit ? (1.0f - sd / distLimit) : 0.0f;
			v.hil = clamp01((1.0f - fog) * nearSun * day * glow);
		}
	}

	// back-to-front (far first) so the soft alpha blends correctly
	for(int a = 1; a < nVis; a++){
		Visible t = vis[a]; int b = a - 1;
		while(b >= 0 && vis[b].sp.z < t.sp.z){ vis[b + 1] = vis[b]; b--; }
		vis[b + 1] = t;
	}

	// ----- pass 1: cloud bodies -----
	rsSet(RS_SRCBLEND, (void*)BL_SRCALPHA);
	rsSet(RS_DESTBLEND, (void*)BL_INVSRCALPHA);
	rsSet(RS_TEXTURERASTER, gTexMask->raster);
	gNumSprites = 0;
	for(int n = 0; n < nVis; n++){
		const Visible &v = vis[n];
		const CloudDef &c = gClouds[v.idx];
		float dm = c.layer ? dist2 : dist;

		// horizon fade: low elevation -> fainter and blended into the horizon (sky bottom) colour
		float px = c.ox * dm, py = c.oy * dm;
		float hd = sqrtf(px*px + py*py);
		float dz = (c.oz + baseZ + (c.layer ? (float)cfg.layer2Height : 0.0f)) - campos.z;
		float len = sqrtf(hd*hd + dz*dz);
		float sinE = len > 0.001f ? dz / len : 1.0f;
		float t = smooth01((sinE - 0.03f) / 0.27f);          // 0 at/below the horizon, 1 well above it
		float fadeA = 1.0f - hfade * (1.0f - t);
		float mixSky = hfade * (1.0f - t) * 0.65f * day;

		float base[3] = { (float)br, (float)bg, (float)bb };
		if(c.layer){ base[0] += (255.0f - base[0]) * 0.15f; base[1] += (255.0f - base[1]) * 0.15f; base[2] += (255.0f - base[2]) * 0.15f; }
		base[0] = lerpf(base[0], (float)skyR, mixSky);
		base[1] = lerpf(base[1], (float)skyG, mixSky);
		base[2] = lerpf(base[2], (float)skyB, mixSky);

		float a = fluffyalpha * c.alpha * fadeA * (c.layer ? cfg.layer2Alpha / 100.0f : 1.0f) * (c.layer ? 1.0f : 1.0f);
		// a little directional light on every cloud in daylight, more near the sun
		float sun = sunOk ? (v.hil > 0.30f * day * (1.0f - fog) ? v.hil : 0.30f * day * (1.0f - fog)) : 0.0f;
		float sz = 55.0f * cfg.size / 100.0f * dm * c.size;
		addLitSprite(v.sp.x, v.sp.y, v.sp.z, v.sx * sz, v.sy * sz, indRot + c.rot, 1.0f / v.sp.z,
			base, v.sdx, v.sdy, sun, sunC, shading, (int)a);
	}
	flushSprites();

	// ----- pass 2: additive sun highlight (per cloud: strongest for clouds near the sun) -----
	if(sunOk){
		bool any = false;
		for(int n = 0; n < nVis; n++) if(vis[n].hil > 0.02f){ any = true; break; }
		if(any){
			rsSet(RS_SRCBLEND, (void*)BL_ONE);
			rsSet(RS_DESTBLEND, (void*)BL_ONE);
			rsSet(RS_TEXTURERASTER, gTexHilite->raster);
			for(int n = 0; n < nVis; n++){
				const Visible &v = vis[n];
				if(v.hil <= 0.02f) continue;
				const CloudDef &c = gClouds[v.idx];
				float dm = c.layer ? dist2 : dist;
				float dx = v.sp.x - sunX, dy = v.sp.y - sunY;
				float ang = atan2f(dx, dy); if(ang < 0.0f) ang += 6.2831853f;     // angle from +Y, like CGeneral::GetATanOfXY
				float sz = 30.0f * cfg.size / 100.0f * dm * c.size;
				float k = v.hil * 0.8f * (c.layer ? cfg.layer2Alpha / 100.0f : 1.0f);
				addSpriteHighlight(v.sp.x, v.sp.y, v.sp.z, v.sx * sz, v.sy * sz,
					(int)(sunR * k), (int)(sunG * k), (int)(sunB * k), 1.0f / v.sp.z, 1.7f - ang);
			}
			flushSprites();
		}
	}

	// ----- restore -----
	rsSet(RS_ZTEST, (void*)(intptr_t)sZTest); rsSet(RS_ZWRITE, (void*)(intptr_t)sZWrite);
	rsSet(RS_SRCBLEND, (void*)(intptr_t)sSrc); rsSet(RS_DESTBLEND, (void*)(intptr_t)sDst);
	rsSet(RS_FOG, (void*)(intptr_t)sFog); rsSet(RS_VERTEXALPHA, (void*)(intptr_t)sVA);
	rsSet(RS_TEXFILTER, (void*)(intptr_t)sFilter); rsSet(RS_TEXTURERASTER, sRaster);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

void Clouds_Install(const CloudsConfig *c)
{
	cfg = *c;
	if(!cfg.enable) return;
	cfg.alpha = clampi(cfg.alpha, 0, 255);
	cfg.nightAlpha = clampi(cfg.nightAlpha, 0, 255);
	cfg.nightBrightness = clampi(cfg.nightBrightness, 0, 300);
	cfg.nightTint = clampi(cfg.nightTint, 0, 100);
	cfg.sunGlow = clampi(cfg.sunGlow, 0, 300);
	cfg.brightness = clampi(cfg.brightness, 0, 300);
	cfg.whiten = clampi(cfg.whiten, 0, 100);
	cfg.size = clampi(cfg.size, 10, 400);
	cfg.distance = clampi(cfg.distance, 50, 1500);
	cfg.shading = clampi(cfg.shading, 0, 100);
	cfg.horizonFade = clampi(cfg.horizonFade, 0, 100);
	cfg.variety = clampi(cfg.variety, 0, 100);
	cfg.layer2Count = clampi(cfg.layer2Count, 0, MAX_L2);
	cfg.layer2Alpha = clampi(cfg.layer2Alpha, 0, 200);
	cfg.layer2Distance = clampi(cfg.layer2Distance, 50, 400);
	cfg.layer2Speed = clampi(cfg.layer2Speed, 0, 300);
	cfg.windSpeed = clampi(cfg.windSpeed, -1, 500);
	if(cfg.log) remove("dualpass_clouds.log");
	LOG("clouds are drawn from DualPass' colour-filter hook");
}
