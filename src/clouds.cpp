// clouds.cpp - Fluffy Clouds (GTA3/VC style cloud sprites) for GTA SA 1.0 US, inside DualPass.
// Port of fluffyclouds-sa. Same maths as the original (37 billboards, 2-colour gradient, sun highlight),
// but self-contained: own sprite batching, own Im2D submit, no plugin-sdk.
//
// Hook: called from DualPass' colour-filter hook (0x704D1E, already proven on this setup), after the game's own
// filter and before the AO. The world depth buffer is intact there, so the clouds are depth-tested against it.
// Colours: read every frame from the game's CURRENT timecyc colour set -> any timecyc mod works.
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
// Game addresses, SA 1.0 US.  VERIFIED = confirmed by the fluffyclouds-sa source or by DualPass itself.
// The rest come from plugin-sdk / gta-reversed - if the game ever crashes on the first cloud frame,
// compare these against plugin-sdk's headers first.
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
#define ADDR_CurrentColours   0xB7C4A0   // CTimeCycle::m_CurrentColours (CColourSet)
// offset of m_nFluffyCloudsBottomRed/Green/Blue (3 x uint16) inside CColourSet. Override: ini colorOffset
#define DEFAULT_COLOUR_OFFSET 0x62

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
static void (*gOrigHookTarget)(void);          // what the call site pointed at before us (read by the naked stub)
static RwTexture *gTexMask, *gTexHilite;
static bool gInitDone, gFailed;
static float gCloudRotation;
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

// ---------------- cloud layout (identical to the original) ----------------
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
#define NUM_CLOUDS 37

// ---------------- sprite batch (replaces CSprite2) ----------------
struct CVert { float x, y, z, rhw; unsigned color; float u, v; };   // RwIm2DVertex, D3D9 layout
static CVert gVerts[NUM_CLOUDS * 4];
static unsigned short gIdx[NUM_CLOUDS * 6];
static int gNumSprites;
static float gNear2D, gFar2D, gNearClip, gFarClip, gScrW, gScrH;

static void flushSprites(void)
{
	if(gNumSprites <= 0) return;
	DWORD g = *(DWORD*)ADDR_RwGlobals;
	((Im2DIndexedFn)*(DWORD*)(g + 0x34))(TRILIST, gVerts, gNumSprites * 4, gIdx, gNumSprites * 6);
	gNumSprites = 0;
}

static inline float clamp01(float v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
static inline unsigned pack(int r, int g, int b, int a)
{
	if(r < 0) r = 0; if(r > 255) r = 255;
	if(g < 0) g = 0; if(g > 255) g = 255;
	if(b < 0) b = 0; if(b > 255) b = 255;
	if(a < 0) a = 0; if(a > 255) a = 255;
	return ((unsigned)a << 24) | ((unsigned)r << 16) | ((unsigned)g << 8) | (unsigned)b;
}

// one rotated quad. c0..c3: per-corner colour (already packed)
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
	if(gNumSprites >= NUM_CLOUDS) return;

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

// body cloud: colour fades from rgb1 (top) to rgb2 (bottom) along (cx,cy) rotated with the sprite
static void addSprite2Colours(float x, float y, float z, float w, float h, int r1, int g1, int b1,
	int r2, int g2, int b2, float cx, float cy, float recipz, float rot, int a)
{
	float c = cosf(rot), s = sinf(rot);
	float cf[4];
	cf[0] = clamp01((cx * (-c - s) + cy * (-c + s)) * 0.5f + 0.5f);
	cf[1] = clamp01((cx * (-c + s) + cy * ( c + s)) * 0.5f + 0.5f);
	cf[2] = clamp01((cx * ( c + s) + cy * ( c - s)) * 0.5f + 0.5f);
	cf[3] = clamp01((cx * ( c - s) + cy * (-c - s)) * 0.5f + 0.5f);
	unsigned col[4];
	for(int i = 0; i < 4; i++)
		col[i] = pack((int)(r1*cf[i] + r2*(1.0f-cf[i])), (int)(g1*cf[i] + g2*(1.0f-cf[i])),
		              (int)(b1*cf[i] + b2*(1.0f-cf[i])), a);
	addQuad(x, y, z, w, h, rot, recipz, col);
}

// highlight sprite: flat additive glow in the sun's colour (the original used a fixed red)
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

// ---------------- timecyc colour (works with any timecyc mod) ----------------
// Every timecyc mod, 24h or not, ends up filling the game's current colour set each frame; we read that.
static bool readTimecycColour(int &r, int &g, int &b)
{
	int off = cfg.colorOffset >= 0 ? cfg.colorOffset : DEFAULT_COLOUR_OFFSET;
	const uint16_t *p = (const uint16_t*)(ADDR_CurrentColours + off);
	uint16_t v[3] = { p[0], p[1], p[2] };
	if(v[0] > 255 || v[1] > 255 || v[2] > 255) return false;   // not a colour -> offset is wrong
	r = v[0]; g = v[1]; b = v[2];
	return true;
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
void Clouds_Draw(void)
{
	if(!cfg.enable || gFailed) return;
	RwCamera *cam = *(RwCamera**)ADDR_SceneCamera;
	if(!cam || !cam->frameBuffer) return;

	if(!gInitDone){
		gInitDone = true;
		if(!initTextures()){ gFailed = true; return; }
		QueryPerformanceCounter(&gLastTime);
		int r, g, b;
		LOG("fluffy clouds started. colour read: %s", readTimecycColour(r, g, b) ? "ok" : "FAILED (using fallback)");
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
	float wind = cfg.windSpeed / 100.0f;

	RwFrame *frame = (RwFrame*)cam->object.object.parent;
	if(!frame) return;
	const RwMatrix *m = &frame->ltm;
	RwV3d campos = m->pos;
	float heading = atan2f(-m->at.x, m->at.y);

	// original CClouds::Update
	gCloudRotation += wind * sinf(heading - 0.85f) * 0.001f * timeStepFix;
	gIndividualRotation += (uint32_t)((wind * timeStep * 0.5f + 0.3f * timeStepFix) * 60.0f);

	// alpha: configured alpha, faded by fog weather; optionally by camera height
	float fog = *(float*)ADDR_Foggyness;
	if(!(fog >= 0.0f && fog <= 1.0f)) fog = 0.0f;

	// time of day, from the live timecyc (brightness of the sky-bottom colour): 0 = night, 1 = day.
	// Also the sun colour (sun core). Both come from the same colour set, so any timecyc mod works.
	float day = 1.0f; int sunR = 255, sunG = 190, sunB = 130;
	{
		const uint16_t *sb = (const uint16_t*)(ADDR_CurrentColours + 0x2A);   // sky bottom RGB (verified by the dump)
		const uint16_t *sc = (const uint16_t*)(ADDR_CurrentColours + 0x30);   // sun core RGB   (verified by the dump)
		if(sb[0] <= 255 && sb[1] <= 255 && sb[2] <= 255){
			float luma = 0.30f * sb[0] + 0.59f * sb[1] + 0.11f * sb[2];
			day = clamp01((luma - 35.0f) / 95.0f);
			day = day * day * (3.0f - 2.0f * day);
		}
		if(sc[0] <= 255 && sc[1] <= 255 && sc[2] <= 255){
			int mx = sc[0] > sc[1] ? sc[0] : sc[1]; if(sc[2] > mx) mx = sc[2];
			if(mx > 40){ sunR = sc[0] * 255 / mx; sunG = sc[1] * 255 / mx; sunB = sc[2] * 255 / mx; }
		}
		sunR += (255 - sunR) / 4; sunG += (255 - sunG) / 4; sunB += (255 - sunB) / 4;   // a bit of white in the glow
	}
	int fluffyalpha = (int)((cfg.nightAlpha + (cfg.alpha - cfg.nightAlpha) * day) * (1.0f - fog));
	float baseZ = (float)cfg.height;
	if(cfg.fadeAtHeight && campos.z > baseZ)
		fluffyalpha -= (int)((campos.z - baseZ) * ((255.0f - (float)cfg.alpha) / 255.0f));
	if(fluffyalpha <= 0) return;
	if(fluffyalpha > 255) fluffyalpha = 255;

	// base colour from the CURRENT timecyc
	int br, bg, bb;
	if(!readTimecycColour(br, bg, bb)){ br = cfg.fallbackR; bg = cfg.fallbackG; bb = cfg.fallbackB; }
	int rawR = br, rawG = bg, rawB = bb;
	br = br * cfg.brightness / 100; bg = bg * cfg.brightness / 100; bb = bb * cfg.brightness / 100;
	if(br > 255) br = 255; if(bg > 255) bg = 255; if(bb > 255) bb = 255;
	int wh = (int)(cfg.whiten * day);          // pull towards white in daylight only (clouds must not glow white at night)
	if(wh > 0){
		br += (255 - br) * wh / 100;
		bg += (255 - bg) * wh / 100;
		bb += (255 - bb) * wh / 100;
	}
	{	// night: darker, with a cool moonlit blue-grey tint
		float n = 1.0f - day;
		float dim = 1.0f - n * (1.0f - cfg.nightBrightness / 100.0f);
		float tint = n * cfg.nightTint / 100.0f;
		br = (int)((br * dim) * (1.0f - tint) + 70.0f * tint);
		bg = (int)((bg * dim) * (1.0f - tint) + 85.0f * tint);
		bb = (int)((bb * dim) * (1.0f - tint) + 120.0f * tint);
	}
	// log what the timecyc really gives us every 5 s (max 60 lines) - shows how it changes with time of day
	if(cfg.log){
		static DWORD lastLog; static int nLog;
		DWORD t = GetTickCount();
		if(nLog < 60 && t - lastLog > 5000){
			lastLog = t; nLog++;
			LOG("timecyc fluffy RGB = %d %d %d -> used %d %d %d, day %.2f, sun %d %d %d, fog %.2f, alpha %d", rawR, rawG, rawB, br, bg, bb, day, sunR, sunG, sunB, fog, fluffyalpha);
		}
	}
	int tr = (int)(br * 0.85f), tg = (int)(bg * 0.85f), tb = (int)(bb * 0.85f);

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

	float rotSin = sinf(gCloudRotation), rotCos = cosf(gCloudRotation);
	float dist = cfg.distance / 100.0f;                  // ring distance multiplier; sprites grow with it so they keep their apparent size
	float sizeMul = 55.0f * cfg.size / 100.0f * dist;
	float sunX = *(float*)ADDR_SunScreenX, sunY = *(float*)ADDR_SunScreenY;
	bool sunOk = cfg.highlight && sunX > -20000.0f && sunX < 20000.0f && sunY > -20000.0f && sunY < 20000.0f;
	float indRot = (uint16_t)gIndividualRotation / 65336.0f * 6.28f;
	CalcScreenFn calc = (CalcScreenFn)ADDR_CalcScreenCoors;

	struct OnScreen { bool on; RwV3d sp; float sx, sy; } cl[NUM_CLOUDS];
	float hilight = 0.0f;

	// ----- pass 1: cloud bodies -----
	rsSet(RS_SRCBLEND, (void*)BL_SRCALPHA);
	rsSet(RS_DESTBLEND, (void*)BL_INVSRCALPHA);
	rsSet(RS_TEXTURERASTER, gTexMask->raster);
	gNumSprites = 0;
	for(int i = 0; i < NUM_CLOUDS; i++){
		RwV3d pos = { 2.0f * CoorsOffsetX[i] * dist, 2.0f * CoorsOffsetY[i] * dist, 40.0f * CoorsOffsetZ[i] + baseZ };
		RwV3d wp;
		wp.x = pos.x * rotCos + pos.y * rotSin + campos.x;
		wp.y = pos.x * rotSin - pos.y * rotCos + campos.y;
		wp.z = pos.z;
		float szx, szy; RwV3d sp;
		if(calc(wp, &sp, &szx, &szy, false, false)){
			int r1 = tr, g1 = tg, b1 = tb, r2 = br, g2 = bg, b2 = bb;
			hilight = 0.0f;
			if(sunOk){
				float dx = sp.x - sunX, dy = sp.y - sunY;
				float sundist = sqrtf(dx*dx + dy*dy);
				float distLimit = (3.0f * gScrW) / 4.0f;
				if(sundist < distLimit){
					hilight = (1.0f - fog) * (1.0f - sundist / distLimit) * day * (cfg.sunGlow / 100.0f);
					if(hilight > 1.0f) hilight = 1.0f;
					r1 = (int)(tr*(1.0f-hilight) + sunR*hilight); g1 = (int)(tg*(1.0f-hilight) + sunG*hilight); b1 = (int)(tb*(1.0f-hilight) + sunB*hilight);
					r2 = (int)(br*(1.0f-hilight) + sunR*hilight); g2 = (int)(bg*(1.0f-hilight) + sunG*hilight); b2 = (int)(bb*(1.0f-hilight) + sunB*hilight);
				}
			}
			addSprite2Colours(sp.x, sp.y, sp.z, szx * sizeMul, szy * sizeMul,
				r1, g1, b1, r2, g2, b2, 0.0f, -1.0f, 1.0f / sp.z, indRot, fluffyalpha);
			cl[i].on = true; cl[i].sp = sp; cl[i].sx = szx; cl[i].sy = szy;
		}else
			cl[i].on = false;
	}
	flushSprites();

	// ----- pass 2: additive sun highlight (same quirk as the original: uses the last cloud's hilight value) -----
	if(sunOk && hilight > 0.0f){
		rsSet(RS_SRCBLEND, (void*)BL_ONE);
		rsSet(RS_DESTBLEND, (void*)BL_ONE);
		rsSet(RS_TEXTURERASTER, gTexHilite->raster);
		float ang = atan2f(0.0f, 1.0f);
		for(int i = 0; i < NUM_CLOUDS; i++){
			if(!cl[i].on) continue;
			float dx = cl[i].sp.x - sunX, dy = cl[i].sp.y - sunY;
			ang = atan2f(dx, dy); if(ang < 0.0f) ang += 6.2831853f;     // angle from +Y, like CGeneral::GetATanOfXY
			addSpriteHighlight(cl[i].sp.x, cl[i].sp.y, cl[i].sp.z, cl[i].sx * 30.0f * cfg.size / 100.0f * dist,
				cl[i].sy * 30.0f * cfg.size / 100.0f * dist, (int)(sunR * 0.8f * hilight), (int)(sunG * 0.8f * hilight), (int)(sunB * 0.8f * hilight),
				1.0f / cl[i].sp.z, 1.7f - ang);
		}
		flushSprites();
	}

	// ----- restore -----
	rsSet(RS_ZTEST, (void*)(intptr_t)sZTest); rsSet(RS_ZWRITE, (void*)(intptr_t)sZWrite);
	rsSet(RS_SRCBLEND, (void*)(intptr_t)sSrc); rsSet(RS_DESTBLEND, (void*)(intptr_t)sDst);
	rsSet(RS_FOG, (void*)(intptr_t)sFog); rsSet(RS_VERTEXALPHA, (void*)(intptr_t)sVA);
	rsSet(RS_TEXFILTER, (void*)(intptr_t)sFilter); rsSet(RS_TEXTURERASTER, sRaster);
}

void Clouds_Install(const CloudsConfig *c)
{
	cfg = *c;
	if(!cfg.enable) return;
	if(cfg.alpha < 0) cfg.alpha = 0; if(cfg.alpha > 255) cfg.alpha = 255;
	if(cfg.brightness < 0) cfg.brightness = 0; if(cfg.brightness > 300) cfg.brightness = 300;
	if(cfg.nightAlpha < 0) cfg.nightAlpha = 0; if(cfg.nightAlpha > 255) cfg.nightAlpha = 255;
	if(cfg.nightBrightness < 0) cfg.nightBrightness = 0; if(cfg.nightBrightness > 300) cfg.nightBrightness = 300;
	if(cfg.nightTint < 0) cfg.nightTint = 0; if(cfg.nightTint > 100) cfg.nightTint = 100;
	if(cfg.sunGlow < 0) cfg.sunGlow = 0; if(cfg.sunGlow > 300) cfg.sunGlow = 300;
	if(cfg.whiten < 0) cfg.whiten = 0; if(cfg.whiten > 100) cfg.whiten = 100;
	if(cfg.distance < 50) cfg.distance = 50; if(cfg.distance > 1500) cfg.distance = 1500;
	if(cfg.size < 10) cfg.size = 10; if(cfg.size > 400) cfg.size = 400;
	if(cfg.windSpeed < 0) cfg.windSpeed = 0; if(cfg.windSpeed > 500) cfg.windSpeed = 500;
	if(cfg.log) remove("dualpass_clouds.log");
	// For the record: what sits at the original mod's hook site on this install (not used any more)
	LOG("clouds are drawn from DualPass' colour-filter hook. bytes at 0x7178F0: %02X %02X %02X %02X %02X",
		*(uint8_t*)0x7178F0, *(uint8_t*)0x7178F1, *(uint8_t*)0x7178F2, *(uint8_t*)0x7178F3, *(uint8_t*)0x7178F4);
}
