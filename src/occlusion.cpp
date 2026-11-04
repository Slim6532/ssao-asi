#define _CRT_SECURE_NO_WARNINGS
#include <unordered_map>
#include <vector>
#include <math.h>
#include <stdio.h>
#include "rwd3d9_wrap.h"
#include "occlusion.h"
#include "occl_vs.h"
#include "occl_ps.h"

static const OcclusionConfig *cfg;
static IDirect3DDevice9 *&gDev = *(IDirect3DDevice9**)0xC97C28;

struct Entry {
	IDirect3DQuery9 *q;
	bool pending, hidden;
	int zeroStreak;
	unsigned lastSeen, lastIssue;
	float cx, cy, cz, r;
};
static std::unordered_map<RpAtomic*, Entry> gMap;
static std::vector<RpAtomic*> gCand;
static unsigned gFrame = 1;
static bool gOn = true, gKeyDown;
static float gCamX, gCamY, gCamZ;
static bool gHaveCam;
static void *gVS, *gPS, *gDecl;
static unsigned gStatSkipped, gStatTested, gStatHidden;
static FILE *gLog;
static int gBadFrames;          // consecutive frames with an implausible number of hidden objects
static bool gBroken;            // culling shut itself off

static const float kCube[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
static const unsigned short kIdx[36] = { 0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 2,3,7, 2,7,6, 1,2,6, 1,6,5, 3,0,4, 3,4,7 };

void Occlusion_Init(const OcclusionConfig *c) { cfg = c; gOn = c->enable != 0; }

static void releaseEntry(Entry &e) { if(e.q){ e.q->Release(); e.q = NULL; } e.pending = false; }
static void releaseAll(void) { for(auto &kv : gMap) releaseEntry(kv.second); gMap.clear(); gCand.clear(); }
void Occlusion_OnReset(void) { releaseAll(); gHaveCam = false; }

static bool worldSphere(RpAtomic *a, float *c, float *r)
{
	RpGeometry *g = a->geometry;
	if(!g || !g->morphTarget) return false;
	RwMatrix *ltm = dpFrameLTM((RwFrame*)a->object.object.parent);
	RwV3d wc;
	dpTransformPoint(&wc, &g->morphTarget->boundingSphere.center, ltm);
	float sx = sqrtf(ltm->right.x*ltm->right.x + ltm->right.y*ltm->right.y + ltm->right.z*ltm->right.z);
	float sy = sqrtf(ltm->up.x*ltm->up.x + ltm->up.y*ltm->up.y + ltm->up.z*ltm->up.z);
	float sz = sqrtf(ltm->at.x*ltm->at.x + ltm->at.y*ltm->at.y + ltm->at.z*ltm->at.z);
	float s = sx > sy ? sx : sy; if(sz > s) s = sz;
	c[0] = wc.x; c[1] = wc.y; c[2] = wc.z;
	*r = g->morphTarget->boundingSphere.radius * s;
	return true;
}

bool Occlusion_Cull(RpAtomic *atomic)
{
	if(!cfg || !gOn || gBroken || !gHaveCam) return false;
	// only the main camera (not env-map / reflection cameras)
	if(*(void**)(*(DWORD*)0xC97B24) != *(void**)0xC1703C) return false;
	float c[3], r;
	if(!worldSphere(atomic, c, &r)) return false;
	if(r < cfg->minRadius/10.0f || r > (float)cfg->maxRadius) return false;
	float dx = c[0]-gCamX, dy = c[1]-gCamY, dz = c[2]-gCamZ;
	if(sqrtf(dx*dx + dy*dy + dz*dz) - r < (float)cfg->minDistance) return false;

	Entry &e = gMap[atomic];            // value-initialised on first use
	if(e.lastSeen != 0 && (fabsf(e.cx-c[0]) > 1.0f || fabsf(e.cy-c[1]) > 1.0f || fabsf(e.cz-c[2]) > 1.0f)){
		releaseEntry(e);                // atomic pointer reused by a different object
		e.hidden = false; e.zeroStreak = 0;
	}
	e.cx = c[0]; e.cy = c[1]; e.cz = c[2]; e.r = r;
	if(e.lastSeen != gFrame){ e.lastSeen = gFrame; gCand.push_back(atomic); }
	if(e.hidden) gStatSkipped++;
	return e.hidden;
}

static void pollResults(void)
{
	for(auto &kv : gMap){
		Entry &e = kv.second;
		if(!e.pending || !e.q) continue;
		DWORD pixels = 0;
		HRESULT hr = e.q->GetData(&pixels, sizeof(pixels), 0);     // 0 = never flush / never stall
		if(hr == S_OK){
			e.pending = false;
			if(pixels == 0){ if(++e.zeroStreak >= cfg->hiddenFrames) e.hidden = true; }
			else           { e.zeroStreak = 0; e.hidden = false; }
		}else if(FAILED(hr)){
			releaseEntry(e);
		}
	}
}

static void issueQueries(void)
{
	if(gCand.empty()) return;
	if(!gVS){
		dpCreateVS(g_vsOccl, &gVS);
		dpCreatePS(g_psOccl, &gPS);
		static const D3DVERTEXELEMENT9 el[2] = { {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END() };
		dpCreateDecl(el, &gDecl);
		if(!gVS || !gPS || !gDecl) return;
	}
	// hidden ones first: they are the only way an object ever comes back
	std::vector<RpAtomic*> order; order.reserve(gCand.size());
	for(RpAtomic *a : gCand) if(gMap[a].hidden) order.push_back(a);
	for(RpAtomic *a : gCand) if(!gMap[a].hidden) order.push_back(a);

	enum { N = 8 };
	static const unsigned st[N] = { D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE, D3DRS_COLORWRITEENABLE,
	                                D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_STENCILENABLE };
	DWORD saved[N];
	for(int i = 0; i < N; i++) dpGetRS(st[i], &saved[i]);
	dpSetRS(D3DRS_ZENABLE, TRUE); dpSetRS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); dpSetRS(D3DRS_ZWRITEENABLE, FALSE);
	dpSetRS(D3DRS_COLORWRITEENABLE, 0); dpSetRS(D3DRS_CULLMODE, D3DCULL_NONE);
	dpSetRS(D3DRS_ALPHATESTENABLE, FALSE); dpSetRS(D3DRS_ALPHABLENDENABLE, FALSE); dpSetRS(D3DRS_STENCILENABLE, FALSE);
	dpSetVS(gVS); dpSetPS(gPS); dpSetDecl(gDecl);
	float comb[16];
	dpComposed(NULL, comb);
	dpSetVSConst(0, comb, 4);

	int budget = cfg->maxQueries;
	for(RpAtomic *a : order){
		if(budget <= 0) break;
		Entry &e = gMap[a];
		if(e.pending || gFrame - e.lastIssue < (unsigned)cfg->interval) continue;
		if(!e.q){
			if(FAILED(gDev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &e.q))){ e.q = NULL; continue; }
		}
		static bool logged;
		if(!logged && cfg->log){
			logged = true;
			if(!gLog) gLog = fopen("dualpass_occlusion.log", "w");
			if(gLog){
				fprintf(gLog, "first cube: centre %.1f %.1f %.1f half %.1f cam %.1f %.1f %.1f\n", e.cx, e.cy, e.cz, e.r, gCamX, gCamY, gCamZ);
				fprintf(gLog, "combined row0 %.3f %.3f %.3f %.3f | row3 %.3f %.3f %.3f %.3f\n", comb[0], comb[1], comb[2], comb[3], comb[12], comb[13], comb[14], comb[15]);
				fflush(gLog);
			}
		}
		float info[4] = { e.cx, e.cy, e.cz, e.r*cfg->margin/100.0f };
		dpSetVSConst(4, info, 1);
		e.q->Issue(D3DISSUE_BEGIN);
		dpDrawIndexedUP(D3DPT_TRIANGLELIST, 0, 8, 12, kIdx, kCube, 12);
		e.q->Issue(D3DISSUE_END);
		e.pending = true; e.lastIssue = gFrame;
		budget--; gStatTested++;
	}
	for(int i = 0; i < N; i++) dpSetRS(st[i], saved[i]);
}

void Occlusion_EndFrame(void)
{
	if(!cfg) return;
	if(cfg->toggleKey){
		bool down = (GetAsyncKeyState(cfg->toggleKey) & 0x8000) != 0;
		if(down && !gKeyDown){ gOn = !gOn; if(!gOn) for(auto &kv : gMap){ kv.second.hidden = false; kv.second.zeroStreak = 0; } }
		gKeyDown = down;
	}
	if(!gOn){ gCand.clear(); gFrame++; return; }
	if(gDev && gDev->TestCooperativeLevel() != D3D_OK){ Occlusion_OnReset(); gFrame++; return; }

	// camera cut / teleport: forget everything that was hidden, nothing may pop
	RwCamera *cam = *(RwCamera**)0xC1703C;
	if(cam){
		RwMatrix *m = dpFrameLTM((RwFrame*)cam->object.object.parent);
		if(gHaveCam){
			float dx = m->pos.x-gCamX, dy = m->pos.y-gCamY, dz = m->pos.z-gCamZ;
			if(dx*dx + dy*dy + dz*dz > 40.0f*40.0f)
				for(auto &kv : gMap){ kv.second.hidden = false; kv.second.zeroStreak = 0; }
		}
		gCamX = m->pos.x; gCamY = m->pos.y; gCamZ = m->pos.z; gHaveCam = true;
	}

	pollResults();

	// sanity guard: if nearly everything is "hidden" the proxy test itself is wrong (bad matrices / depth), not the world
	{
		unsigned tracked = 0, hid = 0;
		for(auto &kv : gMap){ if(kv.second.lastSeen + 2 >= gFrame){ tracked++; if(kv.second.hidden) hid++; } }
		if(tracked >= 30 && hid*100 > tracked*85) gBadFrames++; else gBadFrames = 0;
		if(gBadFrames >= 20){
			gBroken = true;
			for(auto &kv : gMap){ kv.second.hidden = false; kv.second.zeroStreak = 0; }
			if(!gLog) gLog = fopen("dualpass_occlusion.log", "w");
			if(gLog){ fprintf(gLog, "frame %u: %u of %u tracked objects hidden for 20 frames in a row -> culling DISABLED (query test is not working)\n", gFrame, hid, tracked); fflush(gLog); }
		}
	}
	issueQueries();

	// atomics not seen for a while: streamed out, free them
	if(gFrame % 60 == 0){
		for(auto it = gMap.begin(); it != gMap.end(); ){
			if(gFrame - it->second.lastSeen > 180){ releaseEntry(it->second); it = gMap.erase(it); }
			else ++it;
		}
	}
	if(cfg->log && (gFrame % 300 == 0 || (gFrame < 600 && gFrame % 60 == 0))){
		if(!gLog) gLog = fopen("dualpass_occlusion.log", "w");
		if(gLog){
			unsigned hid = 0; for(auto &kv : gMap) if(kv.second.hidden) hid++;
			fprintf(gLog, "frame %u: tracked %u, hidden %u, candidates %u, queries last 300f %u, skipped draws last 300f %u\n",
			        gFrame, (unsigned)gMap.size(), hid, (unsigned)gCand.size(), gStatTested, gStatSkipped);
			fflush(gLog); gStatTested = gStatSkipped = 0;
		}
	}
	gCand.clear();
	gFrame++;
}
