// main.cpp - DualPass + HBAO+ AO + SMAA, GTA SA 1.0 US. Hot-path optimised build.
// Based on SkyGfx 4.2b by aap. ASI, raw game addresses (no plugin-sdk globals needed for the hot path).
//
// OPTIMISATION NOTES (what changed vs. the straightforward version, and why)
//  [MEM]  Zero heap use at runtime: config is one static POD, no new/malloc/std::string anywhere in
//         the render hooks. All per-frame state is static or on the stack.
//  [CALL] RwRenderStateSet/Get used to re-read RwGlobals (0xC97B24) and the device vtable on EVERY call
//         (~10 calls per dual-pass atomic, hundreds of atomics per frame). The two function pointers are
//         now resolved ONCE on the first rendered frame and called directly -> removes 2 dependent loads
//         per state change.
//  [BRANCH] Per-category flags are folded into one bitmask (cfg.mask) after load, and the pipeline
//         pointers are compared in the order of frequency. Disabled categories cost one AND + branch.
//  [STATE] The zwrite Get() is only issued for pipelines that need it (default / car / building);
//         skin pipe (peds) and grass skip it. The alpha-test state is saved/restored around the two
//         passes only (2 Get + 4 Set), nothing else is touched.
//  [INLINE] Every helper in the render path is __forceinline; the two callbacks are the only call
//         boundaries the game sees.
//  [ONCE]  The colour-filter hook is installed once on the first frame (after all other ASIs loaded),
//         with a single predictable-not-taken branch afterwards. Config is read once in DllMain.
//  [THROTTLE] Hotkey polling (GetAsyncKeyState is a syscall-ish call) runs every 8th frame instead of
//         every frame; no per-frame work happens when AO/SMAA are disabled in the ini.
//  [NOTE] CPed/CVehicle loops, distance checks and squared-distance math are not applicable here:
//         this plugin only hooks render callbacks and never iterates entities, so there is nothing
//         to cull or throttle spatially. The 'skip when paused' rule is deliberately NOT applied to
//         AO/SMAA: skipping them in the pause menu would make the frame flicker between processed and
//         unprocessed images.

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
#include "post.h"

#define DP_INLINE __forceinline
#define DP_LIKELY(x)   (x)
#define DP_UNLIKELY(x) (x)

// alpha test render states / functions (RW 3.6 numbering)
#define rwRENDERSTATEALPHATESTFUNCTION     ((RwRenderState)29)
#define rwRENDERSTATEALPHATESTFUNCTIONREF  ((RwRenderState)30)
#define rwALPHATESTFUNCTIONLESS            2
#define rwALPHATESTFUNCTIONGREATEREQUAL    7

// [CALL] device state functions, resolved once (RwGlobals* at 0xC97B24; fpRenderStateSet = +0x20, Get = +0x24)
typedef RwBool (*DPStateFn)(int state, void *value);
static DPStateFn gSetState, gGetState;
#define SETRS(s, v) gSetState((int)(s), (void*)(uintptr_t)(v))
#define GETRS(s, p) gGetState((int)(s), (void*)(p))

// ---------- game addresses (SA 1.0 US) ----------
static int &defaultPipeIdx = *(int*)0xC9BC60;
static RxPipeline *&skinPipe   = *(RxPipeline**)0xC978C4;
static RxPipeline *&carPipe    = *(RxPipeline**)0xC02D24;   // CCustomCarEnvMapPipeline::ObjPipeline
static RxPipeline *&bldgPipe   = *(RxPipeline**)0xC02C68;   // CCustomBuildingPipeline::ObjPipeline
static RxPipeline *&bldgDNPipe = *(RxPipeline**)0xC02C1C;   // CCustomBuildingDNPipeline::ObjPipeline
static RpAtomic *(*plantTab0)[4] = (RpAtomic *(*)[4])0xC039F0;
static RpAtomic *(*plantTab1)[4] = (RpAtomic *(*)[4])0xC03A00;

WRAPPER RxPipeline *RxPipelineExecute(RxPipeline *pipeline, void *data, RwBool heapReset) { EAXJMP(0x805710); }

// ---------- config: one static POD, read once ----------
enum { M_DEF = 1, M_VEH = 2, M_PED = 4, M_GRASS = 8, M_BLDG = 16 };
static struct {
	int global, def, vehicle, ped, grass, building;
	int zwriteThreshold;
	unsigned mask;                               // [BRANCH] folded category flags
	int ssao; SSAOConfig ssaoCfg;
	PostConfig postCfg;
} cfg;

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
	cfg.ssao          = readInt("AO", "ssao", 0, path);
	cfg.ssaoCfg.quality     = readInt("AO", "quality", 1, path);
	if(cfg.ssaoCfg.quality < 0) cfg.ssaoCfg.quality = 0;
	if(cfg.ssaoCfg.quality > 3) cfg.ssaoCfg.quality = 3;
	cfg.ssaoCfg.radius      = readInt("AO", "radius", 15, path) / 10.0f;
	cfg.ssaoCfg.intensity   = readInt("AO", "intensity", 100, path) / 100.0f;
	cfg.ssaoCfg.power       = readInt("AO", "power", 200, path) / 100.0f;
	cfg.ssaoCfg.bias        = readInt("AO", "bias", 20, path) / 100.0f;
	cfg.ssaoCfg.smallScale  = readInt("AO", "smallScale", 100, path) / 100.0f;
	cfg.ssaoCfg.largeScale  = readInt("AO", "largeScale", 100, path) / 100.0f;
	cfg.ssaoCfg.blurSharpness = readInt("AO", "blurSharpness", 16, path) * 1.0f;
	cfg.ssaoCfg.normalPower   = readInt("AO", "normalPower", 8, path) * 1.0f;
	if(cfg.ssaoCfg.normalPower < 1.0f)  cfg.ssaoCfg.normalPower = 1.0f;
	if(cfg.ssaoCfg.normalPower > 32.0f) cfg.ssaoCfg.normalPower = 32.0f;
	if(cfg.ssaoCfg.power < 1.0f) cfg.ssaoCfg.power = 1.0f;
	if(cfg.ssaoCfg.power > 4.0f) cfg.ssaoCfg.power = 4.0f;
	if(cfg.ssaoCfg.bias  < 0.0f) cfg.ssaoCfg.bias  = 0.0f;
	if(cfg.ssaoCfg.bias  > 0.5f) cfg.ssaoCfg.bias  = 0.5f;
	if(cfg.ssaoCfg.smallScale < 0.0f) cfg.ssaoCfg.smallScale = 0.0f;
	if(cfg.ssaoCfg.smallScale > 2.0f) cfg.ssaoCfg.smallScale = 2.0f;
	if(cfg.ssaoCfg.largeScale < 0.0f) cfg.ssaoCfg.largeScale = 0.0f;
	if(cfg.ssaoCfg.largeScale > 2.0f) cfg.ssaoCfg.largeScale = 2.0f;
	if(cfg.ssaoCfg.blurSharpness < 0.0f)  cfg.ssaoCfg.blurSharpness = 0.0f;
	if(cfg.ssaoCfg.blurSharpness > 16.0f) cfg.ssaoCfg.blurSharpness = 16.0f;
	cfg.ssaoCfg.fadeDist    = (float)readInt("AO", "fadeDistance", 60, path);
	cfg.ssaoCfg.maxRadiusPx = readInt("AO", "maxRadiusPx", 48, path);
	cfg.ssaoCfg.minFps      = (float)readInt("AO", "minFps", 45, path);
	cfg.ssaoCfg.toggleKey   = readInt("AO", "toggleKey", 120, path);   // 120 = F9
	cfg.ssaoCfg.debugView   = readInt("AO", "debugView", 0, path);
	cfg.postCfg.smaa        = readInt("Post", "smaa", 0, path);
	cfg.postCfg.toggleKey   = readInt("Post", "toggleKey", 121, path);                 // 121 = F10
	cfg.ssaoCfg.interval    = readInt("AO", "updateInterval", 1, path);
	if(cfg.ssaoCfg.interval < 0) cfg.ssaoCfg.interval = 0;
	if(cfg.ssaoCfg.interval > 4) cfg.ssaoCfg.interval = 4;
	if(cfg.def < 0)     cfg.def = cfg.global;
	if(cfg.vehicle < 0) cfg.vehicle = cfg.global;
	if(cfg.ped < 0)     cfg.ped = cfg.global;
	if(cfg.grass < 0)   cfg.grass = cfg.global;
	if(cfg.building < 0) cfg.building = cfg.global;
	if(cfg.zwriteThreshold < 0) cfg.zwriteThreshold = 0;
	if(cfg.zwriteThreshold > 255) cfg.zwriteThreshold = 255;
}


// ---------- dual pass core ----------
// Pass 1: alpha >= threshold, z-write ON.  Pass 2: alpha < threshold, z-write OFF (no depth holes).
static DP_INLINE RxPipeline*
executeDual(RxPipeline *pipe, RpAtomic *atomic)
{
	int alphatest, alpharef;
	GETRS(rwRENDERSTATEALPHATESTFUNCTION, &alphatest);
	GETRS(rwRENDERSTATEALPHATESTFUNCTIONREF, &alpharef);
	SETRS(rwRENDERSTATEALPHATESTFUNCTIONREF, cfg.zwriteThreshold);
	SETRS(rwRENDERSTATEALPHATESTFUNCTION, rwALPHATESTFUNCTIONGREATEREQUAL);
	RxPipelineExecute(pipe, atomic, 1);
	SETRS(rwRENDERSTATEZWRITEENABLE, FALSE);
	SETRS(rwRENDERSTATEALPHATESTFUNCTION, rwALPHATESTFUNCTIONLESS);
	pipe = RxPipelineExecute(pipe, atomic, 1);
	SETRS(rwRENDERSTATEZWRITEENABLE, TRUE);
	SETRS(rwRENDERSTATEALPHATESTFUNCTION, alphatest);
	SETRS(rwRENDERSTATEALPHATESTFUNCTIONREF, alpharef);
	return pipe;
}

static DP_INLINE int
zwriteOn(void)
{
	int z = 0;
	GETRS(rwRENDERSTATEZWRITEENABLE, &z);
	return z;
}

typedef void (__cdecl *DPColourFilterFn)(unsigned c1, unsigned c2);
static DPColourFilterFn oColourFilter = (DPColourFilterFn)0x703650;
static void __cdecl myColourFilter(unsigned c1, unsigned c2);

// first rendered frame = every other ASI is loaded; hook the filter now so we chain behind them
static void
firstFrameInit(void)
{
	DWORD g = *(DWORD*)0xC97B24;
	gSetState = (DPStateFn)*(DWORD*)(g + 0x20);
	gGetState = (DPStateFn)*(DWORD*)(g + 0x24);
	if(cfg.ssao || cfg.postCfg.smaa){
		uint32_t prev = PTRFROMCALL(0x704D1E);
		if(prev != (uint32_t)myColourFilter){
			oColourFilter = (DPColourFilterFn)prev;
			InjectHook(0x704D1E, myColourFilter);
		}
	}
}

// Replaces the game's AtomicDefaultRenderCallBack (default objects, peds, vehicles, map objects)
static RpAtomic*
myDefaultCallback(RpAtomic *atomic)
{
	if(DP_UNLIKELY(gSetState == NULL)) firstFrameInit();
	if(DP_UNLIKELY(atomic == NULL)) return NULL;      // invalid pointer: bail before touching it

	RxPipeline *pipe = atomic->pipeline;
	const unsigned mask = cfg.mask;
	int dodual = 0;

	if(pipe == NULL){
		pipe = *(RxPipeline**)(*(DWORD*)0xC97B24 + 0x3C + defaultPipeIdx);
		dodual = (mask & M_DEF) && zwriteOn();
	}else if(pipe == skinPipe){
		dodual = (mask & M_PED) != 0;                 // [STATE] peds: no zwrite query
	}else if(pipe == carPipe){
		dodual = (mask & M_VEH) && zwriteOn();
	}else if(pipe == bldgPipe || pipe == bldgDNPipe){
		dodual = (mask & M_BLDG) && zwriteOn();
	}

	pipe = dodual ? executeDual(pipe, atomic) : RxPipelineExecute(pipe, atomic, 1);
	return pipe ? atomic : NULL;
}

// grass (plant atomics): always rendered dual-pass when enabled, z-write forced on
static RpAtomic*
grassRenderCallback(RpAtomic *atomic)
{
	if(DP_UNLIKELY(atomic == NULL)) return NULL;
	RxPipeline *pipe = atomic->pipeline;
	if(pipe == NULL)
		pipe = *(RxPipeline**)(*(DWORD*)0xC97B24 + 0x3C + defaultPipeIdx);

	if(cfg.mask & M_GRASS){
		SETRS(rwRENDERSTATEZWRITEENABLE, TRUE);
		pipe = executeDual(pipe, atomic);
	}else
		pipe = RxPipelineExecute(pipe, atomic, 1);
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

// once per frame, after the game's colour filter: AO, then SMAA, both before the HUD
static void __cdecl
myColourFilter(unsigned c1, unsigned c2)
{
	oColourFilter(c1, c2);                          // previous filter (game's own + any colour mod)

	RwCamera *cam = *(RwCamera**)0xC1703C;
	if(DP_UNLIKELY(cam == NULL)) return;

	// AFTER the colour filter so the AO is not tinted by it
	if(cfg.ssao)
		SSAO_Apply(cam->nearPlane, cam->farPlane, cam->viewWindow.x, cam->viewWindow.y, &cfg.ssaoCfg);
	if(cfg.postCfg.smaa)
		Post_Apply(&cfg.postCfg);
}

BOOL WINAPI
DllMain(HINSTANCE hInst, DWORD reason, LPVOID)
{
	if(reason == DLL_PROCESS_ATTACH){
		// same exe-version check SkyGfx uses (SA 1.0 US only)
		if(*(DWORD*)DynBaseAddress(0x82457C) != 0x94BF &&
		   *(DWORD*)DynBaseAddress(0x8245BC) == 0x94BF)
			return FALSE;

		DisableThreadLibraryCalls(hInst);           // no per-thread DllMain callbacks
		readConfig(hInst);
		cfg.mask = (cfg.def ? M_DEF : 0) | (cfg.vehicle ? M_VEH : 0) | (cfg.ped ? M_PED : 0) |
		           (cfg.grass ? M_GRASS : 0) | (cfg.building ? M_BLDG : 0);

		InjectHook(0x7491C0, myDefaultCallback, PATCH_JUMP);   // default atomics, peds, vehicles, map objects
		InjectHook(0x5BF8EA, CPlantMgr_Initialise_hook);       // grass
	}
	return TRUE;
}
