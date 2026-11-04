#define _CRT_SECURE_NO_WARNINGS
#include <unordered_map>
#include <vector>
#include <math.h>
#include <stdio.h>
#include "rwd3d9_wrap.h"
#include "occlusion.h"
#include "occl_vs.h"
#include "occl_ps.h"

// Lightweight hardware occlusion culling. Hot path (Occlusion_Cull, once per building atomic per frame) does one
// hash lookup and a handful of float ops; per-frame work is limited to pending queries + this frame's candidates.

static const OcclusionConfig *cfg;
static IDirect3DDevice9 *&gDev = *(IDirect3DDevice9**)0xC97C28;

struct Entry {
	IDirect3DQuery9 *q;
	bool pending, hidden;
	int zeroStreak;
	unsigned lastSeen, lastIssue;
	float cx, cy, cz, r;
};
static std::unordered_map<RpAtomic*, Entry> gMap;      // node-based: Entry pointers stay valid until erased
static std::vector<Entry*> gCand, gPending;
static unsigned gFrame = 1;
static bool gOn = true, gKeyDown, gBroken, gHaveCam;
static int gBadFrames;
static float gCamX, gCamY, gCamZ;
static void *gVS, *gPS, *gDecl;
static unsigned gStatTested, gStatSkipped;
static FILE *gLog;

static const float kCube[8][3] = { {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},{-1,-1,1},{1,-1,1},{1,1,1},{-1,1,1} };
static const unsigned short kIdx[36] = { 0,2,1, 0,3,2, 4,5,6, 4,6,7, 0,1,5, 0,5,4, 2,3,7, 2,7,6, 1,2,6, 1,6,5, 3,0,4, 3,4,7 };

void Occlusion_Init(const OcclusionConfig *c) { cfg = c; gOn = c->enable != 0; }

static void releaseEntry(Entry &e) { if(e.q){ e.q->Release(); e.q = NULL; } e.pending = false; }
static void releaseAll(void)
{
	for(auto &kv : gMap) releaseEntry(kv.second);
	gMap.clear(); gCand.clear(); gPending.clear();
}
void Occlusion_OnReset(void) { releaseAll(); gHaveCam = false; }
static void unhideAll(void) { for(auto &kv : gMap){ kv.second.hidden = false; kv.second.zeroStreak = 0; } }

bool Occlusion_Cull(RpAtomic *atomic)
{
	if(!cfg || !gOn || gBroken || !gHaveCam) return false;
	if(*(void**)(*(DWORD*)0xC97B24) != *(void**)0xC1703C) return false;     // main camera only
	RpGeometry *g = atomic->geometry;
	if(!g || !g->morphTarget) return false;
	const RwSphere &bs = g->morphTarget->boundingSphere;
	if(bs.radius < 0.5f) return false;
	RwMatrix *ltm = dpFrameLTM((RwFrame*)atomic->object.object.parent);
	// uniform scale is the norm for map objects: take the right axis length
	float s2 = ltm->right.x*ltm->right.x + ltm->right.y*ltm->right.y + ltm->right.z*ltm->right.z;
	float r = bs.radius * (s2 > 0.9999f && s2 < 1.0001f ? 1.0f : sqrtf(s2));
	if(r < cfg->minRadius*0.1f || r > (float)cfg->maxRadius) return false;
	RwV3d c;
	dpTransformPoint(&c, &bs.center, ltm);
	float dx = c.x-gCamX, dy = c.y-gCamY, dz = c.z-gCamZ;
	float lim = (float)cfg->minDistance + r;
	if(dx*dx + dy*dy + dz*dz < lim*lim) return false;

	Entry &e = gMap[atomic];
	if(e.lastSeen != 0 && (fabsf(e.cx-c.x) > 1.0f || fabsf(e.cy-c.y) > 1.0f || fabsf(e.cz-c.z) > 1.0f)){
		releaseEntry(e);                // atomic pointer reused by another object
		e.hidden = false; e.zeroStreak = 0;
	}
	e.cx = c.x; e.cy = c.y; e.cz = c.z; e.r = r;
	if(e.lastSeen != gFrame){ e.lastSeen = gFrame; gCand.push_back(&e); }
	if(e.hidden) gStatSkipped++;
	return e.hidden;
}

static void pollResults(void)
{
	size_t w = 0;
	for(size_t i = 0; i < gPending.size(); i++){
		Entry *e = gPending[i];
		if(!e->pending || !e->q) continue;                              // released since
		DWORD pixels = 0;
		HRESULT hr = e->q->GetData(&pixels, sizeof(pixels), 0);         // never flush, never stall
		if(hr == S_FALSE){ gPending[w++] = e; continue; }               // not ready yet
		e->pending = false;
		if(hr == S_OK){
			if(pixels == 0){ if(++e->zeroStreak >= cfg->hiddenFrames) e->hidden = true; }
			else           { e->zeroStreak = 0; e->hidden = false; }
		}else releaseEntry(*e);
	}
	gPending.resize(w);
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
	enum { N = 8 };
	static const unsigned st[N] = { D3DRS_ZENABLE, D3DRS_ZFUNC, D3DRS_ZWRITEENABLE, D3DRS_COLORWRITEENABLE,
	                                D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_STENCILENABLE };
	DWORD saved[N];
	bool setup = false;
	int budget = cfg->maxQueries;
	// two passes: hidden objects first (the only way they ever come back), then the rest
	for(int pass = 0; pass < 2 && budget > 0; pass++){
		for(Entry *e : gCand){
			if(budget <= 0) break;
			if(e->hidden != (pass == 0) || e->pending || gFrame - e->lastIssue < (unsigned)cfg->interval) continue;
			if(!e->q && FAILED(gDev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &e->q))){ e->q = NULL; continue; }
			if(!setup){
				setup = true;
				for(int i = 0; i < N; i++) dpGetRS(st[i], &saved[i]);
				dpSetRS(D3DRS_ZENABLE, TRUE); dpSetRS(D3DRS_ZFUNC, D3DCMP_LESSEQUAL); dpSetRS(D3DRS_ZWRITEENABLE, FALSE);
				dpSetRS(D3DRS_COLORWRITEENABLE, 0); dpSetRS(D3DRS_CULLMODE, D3DCULL_NONE);
				dpSetRS(D3DRS_ALPHATESTENABLE, FALSE); dpSetRS(D3DRS_ALPHABLENDENABLE, FALSE); dpSetRS(D3DRS_STENCILENABLE, FALSE);
				dpSetVS(gVS); dpSetPS(gPS); dpSetDecl(gDecl);
				float comb[16];
				dpComposed(NULL, comb);
				dpSetVSConst(0, comb, 4);
			}
			float info[4] = { e->cx, e->cy, e->cz, e->r*cfg->margin*0.01f };
			dpSetVSConst(4, info, 1);
			e->q->Issue(D3DISSUE_BEGIN);
			dpDrawIndexedUP(D3DPT_TRIANGLELIST, 0, 8, 12, kIdx, kCube, 12);
			e->q->Issue(D3DISSUE_END);
			e->pending = true; e->lastIssue = gFrame;
			gPending.push_back(e);
			budget--; gStatTested++;
		}
	}
	if(setup) for(int i = 0; i < N; i++) dpSetRS(st[i], saved[i]);
}

void Occlusion_EndFrame(void)
{
	if(!cfg) return;
	if(cfg->toggleKey){
		bool down = (GetAsyncKeyState(cfg->toggleKey) & 0x8000) != 0;
		if(down && !gKeyDown){ gOn = !gOn; if(!gOn) unhideAll(); }
		gKeyDown = down;
	}
	if(!gOn || gBroken){ gCand.clear(); gFrame++; return; }
	if(gDev && (gFrame & 15) == 0 && gDev->TestCooperativeLevel() != D3D_OK){ Occlusion_OnReset(); gFrame++; return; }

	RwCamera *cam = *(RwCamera**)0xC1703C;
	if(cam){
		RwMatrix *m = dpFrameLTM((RwFrame*)cam->object.object.parent);
		if(gHaveCam){
			float dx = m->pos.x-gCamX, dy = m->pos.y-gCamY, dz = m->pos.z-gCamZ;
			if(dx*dx + dy*dy + dz*dz > 40.0f*40.0f) unhideAll();           // teleport / camera cut: nothing may pop
		}
		gCamX = m->pos.x; gCamY = m->pos.y; gCamZ = m->pos.z; gHaveCam = true;
	}

	pollResults();

	// sanity guard (candidates only): if nearly everything is "hidden" the test itself is wrong, not the world
	{
		unsigned hid = 0, n = (unsigned)gCand.size();
		for(Entry *e : gCand) if(e->hidden) hid++;
		if(n >= 30 && hid*100 > n*85) gBadFrames++; else gBadFrames = 0;
		if(gBadFrames >= 20){
			gBroken = true; unhideAll();
			if(cfg->log && (gLog = fopen("dualpass_occlusion.log", "a"))){
				fprintf(gLog, "frame %u: %u of %u candidates hidden for 20 frames -> culling DISABLED\n", gFrame, hid, n);
				fclose(gLog); gLog = NULL;
			}
		}
	}
	issueQueries();

	// atomics not seen for 3 s or so were streamed out: free them (release first, compact the pending list, then erase)
	if((gFrame & 63) == 0){
		for(auto &kv : gMap) if(gFrame - kv.second.lastSeen > 180) releaseEntry(kv.second);
		size_t w = 0;
		for(Entry *e : gPending) if(e->pending) gPending[w++] = e;
		gPending.resize(w);
		for(auto it = gMap.begin(); it != gMap.end(); ){
			if(gFrame - it->second.lastSeen > 180) it = gMap.erase(it); else ++it;
		}
	}
	if(cfg->log && (gFrame % 600) == 0 && (gLog = fopen("dualpass_occlusion.log", "a"))){
		unsigned hid = 0; for(auto &kv : gMap) if(kv.second.hidden) hid++;
		fprintf(gLog, "frame %u: tracked %u, hidden %u, queries/600f %u, skipped draws/600f %u\n",
		        gFrame, (unsigned)gMap.size(), hid, gStatTested, gStatSkipped);
		fclose(gLog); gLog = NULL; gStatTested = gStatSkipped = 0;
	}
	gCand.clear();
	gFrame++;
}
