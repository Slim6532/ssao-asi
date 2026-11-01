// DualPass - SkyGfx's PS2 "dual pass" transparency rendering, extracted as a standalone .asi
// Based on SkyGfx 4.2b by aap (https://github.com/aap/skygfx). GTA SA 1.0 US only.
//
// Objects with alpha get drawn twice like on the PS2:
//   pass 1: alpha >= threshold, z-write ON   (solid parts write depth)
//   pass 2: alpha <  threshold, z-write OFF  (faded edges blend, no depth holes)
//
// Covers: default atomics, peds (skin pipe), vehicles (car env-map pipe), buildings/map objects
// (palms, fences...) and grass.
//
// Extras (all from SkyGfx, no RenderWare SDK needed): disableClouds, disableGamma, blur edges,
// game radiosity switch, grass backface cull / placement fix.
//
// Everything works at atomic level (re-running the object's own pipeline twice),
// so no RenderWare mesh struct layouts are needed.

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <rwcore.h>
#include <rwplcore.h>
#include <rpworld.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "MemoryMgr.h"
#include "ssao.h"

// ---------- things the plugin-sdk RenderWare headers don't provide ----------
// alpha test render states / functions (RW 3.6 numbering)
#define rwRENDERSTATEALPHATESTFUNCTION     ((RwRenderState)29)
#define rwRENDERSTATEALPHATESTFUNCTIONREF  ((RwRenderState)30)
#define rwALPHATESTFUNCTIONLESS            2
#define rwALPHATESTFUNCTIONGREATEREQUAL    7
#define DP_RENDERSTATECULLMODE             ((RwRenderState)20)
#define DP_CULLMODECULLNONE                1

// RwRenderStateSet/Get: call through the game's RwGlobals device table
// (RwGlobals* lives at 0xC97B24; fpRenderStateSet = +0x20, fpRenderStateGet = +0x24)
typedef RwBool (*DPStateFn)(int state, void *value);
static RwBool DP_RenderStateSet(int state, void *value)
{
	DWORD g = *(DWORD*)0xC97B24;
	return ((DPStateFn)*(DWORD*)(g + 0x20))(state, value);
}
static RwBool DP_RenderStateGet(int state, void *value)
{
	DWORD g = *(DWORD*)0xC97B24;
	return ((DPStateFn)*(DWORD*)(g + 0x24))(state, value);
}
#define RwRenderStateSet DP_RenderStateSet
#define RwRenderStateGet DP_RenderStateGet

// ---------- game addresses (SA 1.0 US) ----------
static int &dword_C9BC60 = *(int*)0xC9BC60;
static RxPipeline *&skinPipe = *(RxPipeline**)0xC978C4;
static RxPipeline *&carPipe  = *(RxPipeline**)0xC02D24;   // CCustomCarEnvMapPipeline::ObjPipeline
static RxPipeline *&bldgPipe   = *(RxPipeline**)0xC02C68; // CCustomBuildingPipeline::ObjPipeline
static RxPipeline *&bldgDNPipe = *(RxPipeline**)0xC02C1C; // CCustomBuildingDNPipeline::ObjPipeline
static RpAtomic *(*plantTab0)[4] = (RpAtomic *(*)[4])0xC039F0;
static RpAtomic *(*plantTab1)[4] = (RpAtomic *(*)[4])0xC03A00;

WRAPPER RxPipeline *RxPipelineExecute(RxPipeline *pipeline, void *data, RwBool heapReset) { EAXJMP(0x805710); }

// ---------- config ----------
static struct {
	int global, def, vehicle, ped, grass, building;
	int zwriteThreshold;
	// extras
	int disableClouds, disableGamma;
	int blurEdges, blurAlpha;                         // experimental screen-edge blur
	int blurLeft, blurTop, blurRight, blurBottom;     // in 1/16 pixel, at 640 wide
	int radiosity;                                    // -1 = leave game default
	int radiosityFilterPasses, radiosityRenderPasses, radiosityIntensity;
	int grassBackfaceCull, grassFixPlacement;
	// native SSAO
	int ssao; SSAOConfig ssaoCfg;
} cfg;
static int gFixGrassPlacement;

static int
readInt(const char *section, const char *key, int def, const char *path)
{
	char buf[32];
	GetPrivateProfileStringA(section, key, "", buf, sizeof(buf), path);
	if(buf[0] == '\0') return def;
	return atoi(buf);
}

static void
readConfig(HINSTANCE hInst)
{
	char path[MAX_PATH];
	GetModuleFileNameA(hInst, path, MAX_PATH);
	char *dot = strrchr(path, '.');
	if(dot) strcpy(dot, ".ini"); else strcat(path, ".ini");

	// -1 = "not set, use global"
	cfg.global  = readInt("DualPass", "dualPass", 1, path);
	cfg.def     = readInt("DualPass", "dualPassDefault", -1, path);
	cfg.vehicle = readInt("DualPass", "dualPassVehicle", -1, path);
	cfg.ped     = readInt("DualPass", "dualPassPed", -1, path);
	cfg.grass   = readInt("DualPass", "dualPassGrass", -1, path);
	cfg.building = readInt("DualPass", "dualPassBuilding", -1, path);
	cfg.zwriteThreshold = readInt("DualPass", "zwriteThreshold", 128, path);
	cfg.disableClouds = readInt("Extras", "disableClouds", 0, path);
	cfg.disableGamma  = readInt("Extras", "disableGamma", 0, path);
	cfg.blurEdges  = readInt("Extras", "blurEdges", 0, path);
	cfg.blurAlpha  = readInt("Extras", "blurAlpha", 96, path);
	cfg.blurLeft   = readInt("Extras", "blurLeft", -16, path);
	cfg.blurTop    = readInt("Extras", "blurTop", -16, path);
	cfg.blurRight  = readInt("Extras", "blurRight", 16, path);
	cfg.blurBottom = readInt("Extras", "blurBottom", 16, path);
	if(cfg.blurAlpha < 0) cfg.blurAlpha = 0;
	if(cfg.blurAlpha > 255) cfg.blurAlpha = 255;
	cfg.radiosity  = (int)readInt("Extras", "radiosity", -1, path);
	cfg.radiosityFilterPasses = readInt("Extras", "radiosityFilterPasses", 2, path);
	cfg.radiosityRenderPasses = readInt("Extras", "radiosityRenderPasses", 1, path);
	cfg.radiosityIntensity    = readInt("Extras", "radiosityIntensity", 35, path);
	cfg.grassBackfaceCull = readInt("Extras", "grassBackfaceCull", 1, path);
	cfg.grassFixPlacement = readInt("Extras", "grassFixPlacement", 0, path);
	gFixGrassPlacement = cfg.grassFixPlacement;
	cfg.ssao          = readInt("AO", "ssao", 0, path);
	cfg.ssaoCfg.quality     = readInt("AO", "quality", 1, path);
	if(cfg.ssaoCfg.quality < 0) cfg.ssaoCfg.quality = 0;
	if(cfg.ssaoCfg.quality > 2) cfg.ssaoCfg.quality = 2;
	cfg.ssaoCfg.radius      = readInt("AO", "radius", 15, path) / 10.0f;
	cfg.ssaoCfg.intensity   = readInt("AO", "intensity", 150, path) / 100.0f;
	cfg.ssaoCfg.power       = readInt("AO", "power", 150, path) / 100.0f;
	cfg.ssaoCfg.bias        = readInt("AO", "bias", 10, path) / 100.0f;
	cfg.ssaoCfg.fadeDist    = (float)readInt("AO", "fadeDistance", 90, path);
	cfg.ssaoCfg.maxRadiusPx = readInt("AO", "maxRadiusPx", 40, path);
	cfg.ssaoCfg.minFps      = (float)readInt("AO", "minFps", 30, path);
	cfg.ssaoCfg.toggleKey   = readInt("AO", "toggleKey", 120, path);   // 120 = F9
	cfg.ssaoCfg.debugView   = readInt("AO", "debugView", 0, path);
	if(cfg.def < 0)     cfg.def = cfg.global;
	if(cfg.vehicle < 0) cfg.vehicle = cfg.global;
	if(cfg.ped < 0)     cfg.ped = cfg.global;
	if(cfg.grass < 0)   cfg.grass = cfg.global;
	if(cfg.building < 0) cfg.building = cfg.global;
	if(cfg.zwriteThreshold < 0) cfg.zwriteThreshold = 0;
	if(cfg.zwriteThreshold > 255) cfg.zwriteThreshold = 255;
}

// ---------- game variables we overwrite (CPostEffects) ----------
// The game loads these from stream.ini during startup, so we re-apply them while rendering.
static void
applyExtras(void)
{
	if(cfg.radiosity >= 0){
		*(bool*)0xC402CC = cfg.radiosity != 0;           // CPostEffects::m_bRadiosity
		*(int*)0x8D510C = cfg.radiosityFilterPasses;     // filter passes
		*(int*)0x8D5110 = cfg.radiosityRenderPasses;     // render passes
		*(int*)0x8D5118 = cfg.radiosityIntensity;        // intensity
	}
}

// run an atomic's pipeline twice: solid part (z-write) then transparent part (no z-write)
static RxPipeline*
executeDual(RxPipeline *pipe, RpAtomic *atomic)
{
	int alphatest, alpharef;
	RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTION, (void*)&alphatest);
	RwRenderStateGet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)&alpharef);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)cfg.zwriteThreshold);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONGREATEREQUAL);
	RxPipelineExecute(pipe, atomic, 1);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)FALSE);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)rwALPHATESTFUNCTIONLESS);
	pipe = RxPipelineExecute(pipe, atomic, 1);
	RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTION, (void*)alphatest);
	RwRenderStateSet(rwRENDERSTATEALPHATESTFUNCTIONREF, (void*)alpharef);
	return pipe;
}

// ---------- atomic level (default objects, peds, vehicles) ----------
// Replaces the game's AtomicDefaultRenderCallBack
static RpAtomic*
myDefaultCallback(RpAtomic *atomic)
{
	RxPipeline *pipe;
	int zwrite = 0;
	int dodual = 0;

	applyExtras();
	pipe = atomic->pipeline;
	if(pipe == NULL){
		pipe = *(RxPipeline**)(*(DWORD*)0xC97B24 + 0x3C + dword_C9BC60);
		RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, (void*)&zwrite);
		if(zwrite && cfg.def)
			dodual = 1;
	}else if(pipe == skinPipe){
		if(cfg.ped)
			dodual = 1;
	}else if(pipe == carPipe && cfg.vehicle){
		RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, (void*)&zwrite);
		if(zwrite)
			dodual = 1;
	}else if((pipe == bldgPipe || pipe == bldgDNPipe) && cfg.building){
		// map objects: palms, fences, etc.
		RwRenderStateGet(rwRENDERSTATEZWRITEENABLE, (void*)&zwrite);
		if(zwrite)
			dodual = 1;
	}

	if(dodual)
		pipe = executeDual(pipe, atomic);
	else
		pipe = RxPipelineExecute(pipe, atomic, 1);
	return pipe ? atomic : NULL;
}

// ---------- grass ----------
static RpAtomic*
grassRenderCallback(RpAtomic *atomic)
{
	RxPipeline *pipe;
	int cullmode = 0;

	pipe = atomic->pipeline;
	if(pipe == NULL)
		pipe = *(RxPipeline**)(*(DWORD*)0xC97B24 + 0x3C + dword_C9BC60);

	if(!cfg.grassBackfaceCull){
		RwRenderStateGet(DP_RENDERSTATECULLMODE, (void*)&cullmode);
		RwRenderStateSet(DP_RENDERSTATECULLMODE, (void*)DP_CULLMODECULLNONE);
	}

	if(cfg.grass){
		RwRenderStateSet(rwRENDERSTATEZWRITEENABLE, (void*)TRUE);
		pipe = executeDual(pipe, atomic);
	}else
		pipe = RxPipelineExecute(pipe, atomic, 1);

	if(!cfg.grassBackfaceCull)
		RwRenderStateSet(DP_RENDERSTATECULLMODE, (void*)cullmode);
	return pipe ? atomic : NULL;
}

static char
CPlantMgr_Initialise_hook(void)
{
	char (*oldfunc)(void) = (char (*)(void))0x5DD910;
	char ret = oldfunc();
	for(int i = 0; i < 4; i++){
		(*plantTab0)[i]->renderCallBack = grassRenderCallback;
		(*plantTab1)[i]->renderCallBack = grassRenderCallback;
	}
	return ret;
}

// ---------- EXPERIMENTAL: PS2-style blur on the screen edges ----------
// Runs the game's own colour filter first (colors unchanged), then draws a slightly shifted,
// semi-transparent copy of the screen on top. The shift grows towards the screen edges.
// Only installed when blurEdges = 1 in the ini.
struct DPVert { float x, y, z, rhw; unsigned color; float u, v; };   // RwIm2DVertex (D3D9)
static DPVert *cfVerts = (DPVert*)0xC400D8;                // game's full-screen filter quad
static unsigned short *cfIdx = (unsigned short*)0x8D5174;  // its indices

WRAPPER void DP_CameraEndUpdate(void *cam)   { EAXJMP(0x7EE180); }
WRAPPER void DP_CameraBeginUpdate(void *cam) { EAXJMP(0x7EE190); }
WRAPPER void *DP_RasterPushContext(void *r)  { EAXJMP(0x7FB060); }
WRAPPER void *DP_RasterPopContext(void)      { EAXJMP(0x7FB110); }
WRAPPER void *DP_RasterRenderFast(void *r, int x, int y) { EAXJMP(0x7FAF50); }

typedef int (__cdecl *DPIm2DIndexedFn)(int primType, void *verts, int numVerts, unsigned short *idx, int numIdx);
static void
DP_Im2DIndexed(void *verts)
{
	DWORD g = *(DWORD*)0xC97B24;
	// RwDevice::fpIm2DRenderIndexedPrimitive (RwGlobals + 0x34); primType 3 = triangle list
	((DPIm2DIndexedFn)*(DWORD*)(g + 0x34))(3, verts, 4, cfIdx, 6);
}

static void
DP_UpdateFrontBuffer(void)
{
	RwCamera *cam = *(RwCamera**)0xC1703C;
	void *fb = *(void**)0xC402D8;
	DP_CameraEndUpdate(cam);
	DP_RasterPushContext(fb);
	DP_RasterRenderFast(cam->frameBuffer, 0, 0);
	DP_RasterPopContext();
	DP_CameraBeginUpdate(cam);
}

typedef void (__cdecl *DPColourFilterFn)(unsigned c1, unsigned c2);

static void __cdecl
myColourFilter(unsigned c1, unsigned c2)
{
	RwCamera *cam = *(RwCamera**)0xC1703C;
	if(cfg.ssao)
		SSAO_Apply(cam->nearPlane, cam->farPlane, cam->viewWindow.x, cam->viewWindow.y, &cfg.ssaoCfg);

	((DPColourFilterFn)0x703650)(c1, c2);        // the game's own filter, unchanged
	if(!cfg.blurEdges) return;

	RwRaster *fb = *(RwRaster**)0xC402D8;
	DPVert blur[4];

	DP_UpdateFrontBuffer();                      // front buffer = filtered screen

	float rw = (float)fb->width, rh = (float)fb->height;
	float scale = (float)cam->frameBuffer->width / 640.0f;
	float leftOff   = cfg.blurLeft*scale   / 16.0f / rw;
	float rightOff  = cfg.blurRight*scale  / 16.0f / rw;
	float topOff    = cfg.blurTop*scale    / 16.0f / rh;
	float bottomOff = cfg.blurBottom*scale / 16.0f / rh;
	memcpy(blur, cfVerts, sizeof(blur));
	// vertices: 0--3 / 1--2  (top-left, bottom-left, bottom-right, top-right)
	blur[0].u += leftOff;  blur[1].u += leftOff;
	blur[2].u += rightOff; blur[3].u += rightOff;
	blur[0].v += topOff;   blur[3].v += topOff;
	blur[1].v += bottomOff; blur[2].v += bottomOff;
	unsigned col = ((unsigned)cfg.blurAlpha << 24) | 0x00FFFFFF;
	for(int i = 0; i < 4; i++)
		blur[i].color = col;

	DP_RenderStateSet(9,  (void*)2);             // texture filter: linear
	DP_RenderStateSet(14, (void*)FALSE);         // fog off
	DP_RenderStateSet(6,  (void*)FALSE);         // z test off
	DP_RenderStateSet(8,  (void*)FALSE);         // z write off
	DP_RenderStateSet(1,  (void*)fb);            // texture raster = front buffer
	DP_RenderStateSet(12, (void*)TRUE);          // vertex alpha on
	DP_RenderStateSet(10, (void*)5);             // src blend: src alpha
	DP_RenderStateSet(11, (void*)6);             // dest blend: inv src alpha
	DP_Im2DIndexed(blur);
	DP_RenderStateSet(6,  (void*)TRUE);
	DP_RenderStateSet(8,  (void*)TRUE);
	DP_RenderStateSet(1,  (void*)NULL);

	DP_UpdateFrontBuffer();                      // keep it in sync for later effects
}

// SkyGfx "fixGrassPlacement": use a fixed seed so grass doesn't move around (hook at 0x5DADB7)
void __declspec(naked)
fixSeed(void)
{
	_asm{
		cmp	dword ptr [gFixGrassPlacement], 0
		jle	dontfix
		mov	ecx, [esp+54h]
		mov	ebx, [ecx+eax*4]
		mov	ebp, [ebx+4]
		lea	edi, [ebp+10h]
		mov	eax, [esi+48h]

		push	5DADCCh
		retn

	dontfix:
		fld	dword ptr [esi+48h]
		mov	ecx, [esp+54h]
		push	5DADBEh
		retn
	}
}

BOOL WINAPI
DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH){
		// same exe-version check SkyGfx uses (SA 1.0 US only)
		if(*(DWORD*)DynBaseAddress(0x82457C) != 0x94BF &&
		   *(DWORD*)DynBaseAddress(0x8245BC) == 0x94BF)
			return FALSE;

		readConfig(hInst);

		InjectHook(0x7491C0, myDefaultCallback, PATCH_JUMP);          // default atomics, peds, vehicles
		InjectHook(0x5BF8EA, CPlantMgr_Initialise_hook);              // grass

		if(cfg.disableClouds)
			InjectHook(0x714145, 0x71422A, PATCH_JUMP);               // jump over cloud loop
		if(cfg.disableGamma)
			InjectHook(0x74721C, 0x7472F3, PATCH_JUMP);
		if(cfg.grassFixPlacement)
			InjectHook(0x5DADB7, fixSeed, PATCH_JUMP);
		if(cfg.blurEdges || cfg.ssao)
			InjectHook(0x704D1E, myColourFilter);                     // experimental edge blur
	}
	return TRUE;
}
