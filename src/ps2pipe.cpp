// PS2 building pipeline, ported from SkyGfx 4.2b (buildingPipe.cpp) by aap.
// Left out on purpose: Xbox/sphere-map variants, detail maps (TexDB), tag fading, SA-MP fog fix.
#define _CRT_SECURE_NO_WARNINGS
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "rwd3d9_wrap.h"
#include "ps2pipe.h"
#include "ps2_bld_vs.h"
#include "ps2_bldfx_vs.h"
#include "ps2_simple_ps.h"
#include "ps2_envfx_ps.h"

static const PS2PipeConfig *cfg;
static volatile long gDisabled;      // set after the first exception inside the pipe: draw nothing instead of crashing

static void plog(const char *fmt, ...)
{
	static FILE *f;
	if(!f) f = fopen("dualpass_ps2.log", "w");
	if(!f) return;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fflush(f);
}
static int logException(EXCEPTION_POINTERS *ep, const char *where)
{
	HMODULE me = NULL;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)&logException, &me);
	DWORD addr = (DWORD)ep->ExceptionRecord->ExceptionAddress;
	plog("EXCEPTION in %s: code %08X at %08X (dualpass.asi base %08X, offset %08X; gta_sa.exe addresses are absolute)",
	     where, ep->ExceptionRecord->ExceptionCode, addr, (DWORD)me, addr - (DWORD)me);
	if(ep->ExceptionRecord->NumberParameters >= 2)
		plog("  access type %u, address %08X", (unsigned)ep->ExceptionRecord->ExceptionInformation[0], (unsigned)ep->ExceptionRecord->ExceptionInformation[1]);
	InterlockedExchange(&gDisabled, 1);
	plog("PS2 building pipe disabled for the rest of the session (buildings will not draw)");
	return EXCEPTION_EXECUTE_HANDLER;
}

// ---- game variables ----
static int &gtaPipeOffset            = *(int*)0x8D6080;
static int &extraVertColourOffset    = *(int*)0x8D12BC;   // CCustomBuildingDNPipeline::ms_extraVertColourPluginOffset
static int &envMapMatOffset          = *(int*)0x8D12C4;   // CCustomCarEnvMapPipeline::ms_envMapPluginOffset
static float &DNBalance              = *(float*)0x8D12C0;
static float &WetRoads               = *(float*)0xC81308;
static float &LightsMult             = *(float*)0x8D4B5C;
static RxPipeline *&bldgPipe         = *(RxPipeline**)0xC02C68;
static RxPipeline *&bldgDNPipe       = *(RxPipeline**)0xC02C1C;
WRAPPER static double tcAmbR(void) { EAXJMP(0x560330); }
WRAPPER static double tcAmbG(void) { EAXJMP(0x560340); }
WRAPPER static double tcAmbB(void) { EAXJMP(0x560350); }

#define PLUGIN(type, obj, off) ((type*)((char*)(obj) + (off)))
// PC pipeline ids the game stores in the atomic's pipeline plugin (rpPDS_MAKEPIPEID(VEND_ROCKSTAR, id))
#define PIPEID(x) (((0x0253F2u & 0xFFFFu) << 16) | ((x) & 0xFFFFu))   // rpPDS_MAKEPIPEID
static const unsigned ID_BUILDING    = PIPEID(0x9C);
static const unsigned ID_BUILDING_DN = PIPEID(0x98);

struct CustomEnvData {      // CustomEnvMapPipeMaterialData
	signed char scaleX, scaleY, transScaleX, transScaleY, shininess; unsigned char pad; unsigned short frame;
	RwTexture *texture;
};

// ---- shaders ----
static void *vsBuilding, *vsBuildingFx, *psSimple, *psEnvFx;
static void createShaders(void)
{
	if(!vsBuilding)   dpCreateVS(g_vsPS2Building, &vsBuilding);
	if(!vsBuildingFx) dpCreateVS(g_vsPS2BuildingFx, &vsBuildingFx);
	if(!psSimple)     dpCreatePS(g_psPS2Simple, &psSimple);
	if(!psEnvFx)      dpCreatePS(g_psPS2EnvFx, &psEnvFx);
}

// ---- render state helpers (RW device table) ----
typedef int (*StateFn)(int, void*);
static void rsSet(int s, int v) { ((StateFn)*(DWORD*)(*(DWORD*)0xC97B24 + 0x20))(s, (void*)v); }
static void rsGet(int s, void *v) { ((StateFn)*(DWORD*)(*(DWORD*)0xC97B24 + 0x24))(s, v); }
#define ATEST_FUNC ((int)29)
#define ATEST_REF  ((int)30)
#define ATEST_LESS 2
#define ATEST_GEQ  7
#define ATEST_ALWAYS 1

static void drawMesh(DPResEntryHeader *h, DPInstanceData *d)
{
	if(h->indexBuffer) dpDrawIndexed(h->primType, d->baseIndex, 0, d->numVertices, d->startIndex, d->numPrimitives);
	else               dpDraw(h->primType, d->baseIndex, d->numPrimitives);
}
// per-mesh dual pass: solid part with z-write, then the faded part without
static void drawMeshDual(DPResEntryHeader *h, DPInstanceData *d)
{
	int blend = 0, zwrite = 0, func = 0, ref = 0;
	dpGetRS(D3DRS_ALPHABLENDENABLE, &blend);
	rsGet(rwRENDERSTATEZWRITEENABLE, &zwrite);
	rsGet(ATEST_FUNC, &func);
	if(cfg->dualPass && blend && zwrite){
		rsGet(ATEST_REF, &ref);
		rsSet(ATEST_REF, cfg->zwriteThreshold);
		rsSet(ATEST_FUNC, ATEST_GEQ);
		rsSet(rwRENDERSTATEZWRITEENABLE, TRUE);
		drawMesh(h, d);
		rsSet(ATEST_FUNC, ATEST_LESS);
		rsSet(rwRENDERSTATEZWRITEENABLE, FALSE);
		drawMesh(h, d);
		rsSet(rwRENDERSTATEZWRITEENABLE, zwrite);
		rsSet(ATEST_REF, ref);
		rsSet(ATEST_FUNC, func);
	}else if(!zwrite){
		rsSet(ATEST_FUNC, ATEST_ALWAYS);
		drawMesh(h, d);
		rsSet(ATEST_FUNC, func);
	}else
		drawMesh(h, d);
}

// ---- constants ----
enum { REG_transform = 0, REG_ambient = 4, REG_matCol = 19, REG_surfProps = 20, REG_shaderParams = 29,
       REG_dayparam = 30, REG_nightparam = 31, REG_texmat = 32, REG_fxParams = 36, REG_envXform = 37, REG_envmat = 38 };

static unsigned atomicPipeID(RpAtomic *a) { return *PLUGIN(unsigned, a, gtaPipeOffset); }

static void setDnParams(RpAtomic *atomic)
{
	float b = DNBalance; if(b < 0.0f) b = 0.0f; if(b > 1.0f) b = 1.0f;
	float day[4] = { 1.0f-b, 1.0f-b, 1.0f-b, WetRoads };
	float night[4] = { b, b, b, 1.0f-WetRoads };
	// only one colour set (non-DN pipe): force the one we have (night, unintuitively)
	if(atomic->pipeline->pluginData == ID_BUILDING){
		day[0] = day[1] = day[2] = day[3] = 0.0f;
		night[0] = night[1] = night[2] = night[3] = 1.0f;
	}
	dpSetVSConst(REG_dayparam, day, 1);
	dpSetVSConst(REG_nightparam, night, 1);
}

// env matrix: object -> camera space, orthonormalised
static void envMatrix(RpAtomic *atomic, float *out12)
{
	RwCamera *cam = *(RwCamera**)0xC1703C;
	RwFrame *camFrame = (RwFrame*)cam->object.object.parent;
	RpClump *clump = atomic->clump;
	RwFrame *frame = clump ? (RwFrame*)clump->object.parent : (RwFrame*)atomic->object.object.parent;
	RwMatrix inv, m;
	dpMatInvert(&inv, dpFrameLTM(camFrame));
	dpMatMultiply(&m, dpFrameLTM(frame), &inv);
	if((m.flags & rwMATRIXTYPEMASK) != rwMATRIXTYPEORTHONORMAL) dpMatOrthoNormalize(&m, &m);
	memcpy(out12, &m, 48);
}

static void ps2BuildingRenderImpl(RwResEntry *repEntry, void *object, unsigned char type, unsigned flags)
{
	RpAtomic *atomic = (RpAtomic*)object;
	DPResEntryHeader *h = (DPResEntryHeader*)(repEntry + 1);
	DPInstanceData *d = (DPInstanceData*)(h + 1);
	static bool first = true;
	if(first){
		first = false;
		plog("first render: numMeshes %u indexBuffer %p primType %u stream0 vb %p stride %u useOffsets %d decl %p totalVerts %u | mesh0 numIndices %u minVert %u numVerts %d material %p baseIndex %u startIndex %u prims %u",
		     h->numMeshes, h->indexBuffer, h->primType, h->vertexStream[0].vertexBuffer, h->vertexStream[0].stride, h->useOffsets,
		     h->vertexDeclaration, h->totalNumVertex, d->numIndices, d->minVert, d->numVertices, d->material, d->baseIndex, d->startIndex, d->numPrimitives);
	}
	if(h->numMeshes == 0 || h->numMeshes > 2000 || !h->vertexStream[0].vertexBuffer || !h->vertexDeclaration) return;   // sanity: wrong struct layout shows up here

	dpEnableClipping(object, type);
	float transform[16];
	dpComposed(dpFrameLTM((RwFrame*)atomic->object.object.parent), transform);
	dpSetVSConst(REG_transform, transform, 4);

	if(h->indexBuffer) dpSetIndices(h->indexBuffer);
	dpSetStreams(h->vertexStream, h->useOffsets);
	dpSetDecl(h->vertexDeclaration);
	setDnParams(atomic);

	float envmat[12];
	envMatrix(atomic, envmat);
	dpSetVSConst(REG_envmat, envmat, 3);

	// ambient from the live timecycle (not the pAmbient light, SkyGfx explains why)
	float amb[4] = { (float)(tcAmbR()*LightsMult), (float)(tcAmbG()*LightsMult), (float)(tcAmbB()*LightsMult), 0.0f };

	int alphafunc, alpharef, src, dst, fog, zwrite;
	rsGet(ATEST_REF, &alpharef); rsGet(ATEST_FUNC, &alphafunc);
	rsGet(rwRENDERSTATESRCBLEND, &src); rsGet(rwRENDERSTATEDESTBLEND, &dst);
	rsGet(rwRENDERSTATEFOGCOLOR, &fog); rsGet(rwRENDERSTATEZWRITEENABLE, &zwrite);

	static const float identTex[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
	for(unsigned n = h->numMeshes; n--; d++){
		RpMaterial *mat = d->material;
		float scale[4] = { 1.0f, 0, 0, 0 };
		if(mat->texture && cfg->modulate) scale[0] = 255.0f/128.0f;
		dpSetTexture(mat->texture ? mat->texture : dpWhiteTex, 0);
		dpSetPSConst(0, scale, 1);
		dpSetVSConst(REG_shaderParams, scale, 1);

		RwMatrix *m1 = NULL, *m2 = NULL;
		if(dpMatFXGetEffects(mat) == 5 /* rpMATFXEFFECTUVTRANSFORM */){
			dpMatFXGetUV(mat, &m1, &m2);
			if(m1) dpSetVSConst(REG_texmat, m1, 4); else dpSetVSConst(REG_texmat, identTex, 4);
		}else
			dpSetVSConst(REG_texmat, identTex, 4);

		rsSet(rwRENDERSTATEVERTEXALPHAENABLE, d->vertexAlpha || mat->color.alpha != 255);

		float col[4] = { 1, 1, 1, 1 };
		if(flags & rpGEOMETRYMODULATEMATERIALCOLOR){
			col[0] = mat->color.red/255.0f; col[1] = mat->color.green/255.0f;
			col[2] = mat->color.blue/255.0f; col[3] = mat->color.alpha/255.0f;
		}
		dpSetVSConst(REG_matCol, col, 1);
		dpSetVSConst(REG_ambient, amb, 1);
		float surf[4] = { mat->surfaceProps.ambient, mat->surfaceProps.specular, mat->surfaceProps.diffuse, 0.0f };
		dpSetVSConst(REG_surfProps, surf, 1);

		dpSetVS(vsBuilding);
		dpSetPS(psSimple);
		drawMeshDual(h, d);

		// reflection (env map) pass
		if(*(int*)&mat->surfaceProps.specular & 1){
			CustomEnvData *env = *PLUGIN(CustomEnvData*, mat, envMapMatOffset);
			if(env && env->texture){
				rsSet(rwRENDERSTATETEXTUREADDRESS, rwTEXTUREADDRESSWRAP);
				dpSetTexture(env->texture, 1);
				float fx[4] = { env->shininess/255.0f, 1.0f, 0, 0 };
				float xf[4] = { 0.0f, 0.0f, env->scaleX/8.0f, env->scaleY/8.0f };
				dpSetVSConst(REG_envXform, xf, 1);
				dpSetVSConst(REG_fxParams, fx, 1);
				dpSetVS(vsBuildingFx);
				dpSetPS(psEnvFx);
				rsSet(ATEST_FUNC, ATEST_ALWAYS);
				rsSet(rwRENDERSTATEZWRITEENABLE, FALSE);
				rsSet(rwRENDERSTATEVERTEXALPHAENABLE, TRUE);
				rsSet(rwRENDERSTATESRCBLEND, rwBLENDONE);
				rsSet(rwRENDERSTATEDESTBLEND, rwBLENDONE);
				rsSet(rwRENDERSTATEFOGCOLOR, 0);
				drawMesh(h, d);
				rsSet(rwRENDERSTATEFOGCOLOR, fog);
				rsSet(rwRENDERSTATEZWRITEENABLE, zwrite);
				rsSet(rwRENDERSTATESRCBLEND, src);
				rsSet(rwRENDERSTATEDESTBLEND, dst);
				rsSet(ATEST_FUNC, alphafunc);
			}
		}
	}
	rsSet(ATEST_REF, alpharef);
	rsSet(ATEST_FUNC, alphafunc);
	dpSetTexture(NULL, 1);
	dpSetTSS(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dpSetTSS(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
	dpSetTSS(1, D3DTSS_TEXCOORDINDEX, 1);
	dpSetTSS(1, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
}

static void ps2BuildingRender(RwResEntry *repEntry, void *object, unsigned char type, unsigned flags)
{
	if(gDisabled) return;
	__try { ps2BuildingRenderImpl(repEntry, object, type, flags); }
	__except(logException(GetExceptionInformation(), "render callback")) { }
}

// ---- instancing (SkyGfx DNInstance_PS2): pos, uv, normal, night colour (COLOR0), day colour (COLOR1) ----
static RwRGBA *extraColours(RpGeometry *g) { return *PLUGIN(RwRGBA*, g, extraVertColourOffset); }
static RwRGBA *dayColours(RpGeometry *g)
{
	RwRGBA *day = *PLUGIN(RwRGBA*, g, extraVertColourOffset + 4);
	return day ? day : g->preLitLum;
}
static bool allZero(RwRGBA *c, int n) { unsigned *u = (unsigned*)c; while(n--) if(*u++) return false; return true; }
static void instWhite(unsigned char *mem, int n, unsigned stride) { while(n--){ *(unsigned*)mem = 0xFFFFFFFF; mem += stride; } }

static int instanceImpl(void *object, DPResEntryHeader *h, int reinstance)
{
	RpAtomic *atomic = (RpAtomic*)object;
	RpGeometry *geo = atomic->geometry;
	bool textured = (geo->flags & (rpGEOMETRYTEXTURED | rpGEOMETRYTEXTURED2)) != 0;
	RpMorphTarget *mt = geo->morphTarget;
	bool prelit = (geo->flags & rpGEOMETRYPRELIT) != 0;
	bool normals = (geo->flags & rpGEOMETRYNORMALS) != 0;
	RwRGBA *day = dayColours(geo), *night = NULL;
	if(atomicPipeID(atomic) == ID_BUILDING_DN){
		night = extraColours(geo);
		if(night && allZero(night, geo->numVertices)) night = NULL;
	}
	D3DVERTEXELEMENT9 dcl[8];
	int i, j;
	if(reinstance){
		UINT n;
		((IDirect3DVertexDeclaration9*)h->vertexDeclaration)->GetDeclaration(dcl, &n);
	}else{
		h->totalNumVertex = geo->numVertices;
		if(h->vertexStream[0].vertexBuffer){
			dpDestroyVB(h->vertexStream[0].stride, geo->numVertices*h->vertexStream[0].stride,
			            h->vertexStream[0].vertexBuffer, h->vertexStream[0].offset);
			h->vertexStream[0].vertexBuffer = NULL;
		}
		h->vertexStream[0].offset = 0; h->vertexStream[0].managed = 0; h->vertexStream[0].dynamicLock = 0;
		unsigned stride = 12; i = 0;
		dcl[i++] = { 0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0 };
		h->vertexStream[0].geometryFlags = rpGEOMETRYLOCKVERTICES;
		if(textured){
			dcl[i++] = { 0, (WORD)stride, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 };
			stride += 8; h->vertexStream[0].geometryFlags |= rpGEOMETRYLOCKTEXCOORDS;
		}
		if(normals){
			dcl[i++] = { 0, (WORD)stride, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0 };
			stride += 12; h->vertexStream[0].geometryFlags |= rpGEOMETRYLOCKNORMALS;
		}
		dcl[i++] = { 0, (WORD)stride, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0 };
		stride += 4;
		if(prelit){
			dcl[i++] = { 0, (WORD)stride, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1 };
			stride += 4;
		}
		h->vertexStream[0].geometryFlags |= rpGEOMETRYLOCKPRELIGHT;   // non-prelit: one white colour set, like the PS2
		h->vertexStream[0].stride = stride;
		dcl[i] = D3DDECL_END();
		if(!dpCreateDecl(dcl, &h->vertexDeclaration)) return 0;
		h->vertexStream[0].managed = 1;
		if(!dpCreateVB(stride, stride*h->totalNumVertex, (void**)h->vertexStream, &h->vertexStream[0].offset)) return 0;
		DPInstanceData *d = (DPInstanceData*)(h + 1);
		for(unsigned n = h->numMeshes; n--; d++)
			d->baseIndex += d->minVert + h->vertexStream[0].offset/h->vertexStream[0].stride;
	}

	IDirect3DVertexBuffer9 *vb = (IDirect3DVertexBuffer9*)h->vertexStream[0].vertexBuffer;
	int locked = reinstance ? geo->lockedSinceLastInst : rpGEOMETRYLOCKALL;
	if(locked == rpGEOMETRYLOCKALL || (h->vertexStream[0].geometryFlags & locked)){
		void *data = NULL;
		vb->Lock(h->vertexStream[0].offset, h->totalNumVertex*h->vertexStream[0].stride, &data, D3DLOCK_NOSYSLOCK);
		if(data){
			unsigned stride = h->vertexStream[0].stride;
			unsigned char *mem = (unsigned char*)data;
			if(locked & rpGEOMETRYLOCKVERTICES){
				for(i = 0; dcl[i].Usage != D3DDECLUSAGE_POSITION || dcl[i].UsageIndex != 0; i++) ;
				dpInstV3d(dcl[i].Type, mem + dcl[i].Offset, mt->verts, h->totalNumVertex, stride);
			}
			if(textured && (locked & rpGEOMETRYLOCKTEXCOORDS)){
				for(i = 0; dcl[i].Usage != D3DDECLUSAGE_TEXCOORD || dcl[i].UsageIndex != 0; i++) ;
				dpInstV2d(dcl[i].Type, mem + dcl[i].Offset, geo->texCoords[0], h->totalNumVertex, stride);
			}
			if(normals && (locked & rpGEOMETRYLOCKNORMALS)){
				for(i = 0; dcl[i].Usage != D3DDECLUSAGE_NORMAL || dcl[i].UsageIndex != 0; i++) ;
				dpInstV3d(dcl[i].Type, mem + dcl[i].Offset, mt->normals, h->totalNumVertex, stride);
			}
			if(prelit && (locked & rpGEOMETRYLOCKPRELIGHT)){
				for(i = 0; dcl[i].Usage != D3DDECLUSAGE_COLOR || dcl[i].UsageIndex != 0; i++) ;
				for(j = 0; dcl[j].Usage != D3DDECLUSAGE_COLOR || dcl[j].UsageIndex != 1; j++) ;
				if(!night) night = day;       // no extra colours: use the regular set for both
				DPInstanceData *d = (DPInstanceData*)(h + 1);
				for(unsigned n = h->numMeshes; n--; d++){
					d->vertexAlpha = dpInstColor(mem + dcl[i].Offset + stride*d->minVert, night + d->minVert, d->numVertices, stride);
					d->vertexAlpha |= dpInstColor(mem + dcl[j].Offset + stride*d->minVert, day + d->minVert, d->numVertices, stride);
				}
			}else if(locked & rpGEOMETRYLOCKPRELIGHT){
				for(i = 0; dcl[i].Usage != D3DDECLUSAGE_COLOR || dcl[i].UsageIndex != 0; i++) ;
				DPInstanceData *d = (DPInstanceData*)(h + 1);
				for(unsigned n = h->numMeshes; n--; d++){
					d->vertexAlpha = 0;
					instWhite(mem + dcl[i].Offset + stride*d->minVert, d->numVertices, stride);
				}
			}
			vb->Unlock();
		}
	}
	return 1;
}

static int instancePS2(void *object, DPResEntryHeader *h, int reinstance)
{
	static bool first = true;
	if(first){ first = false; plog("first instance call (reinstance=%d, header %p, sizeof header %u)", reinstance, h, (unsigned)sizeof(DPResEntryHeader)); }
	if(gDisabled) return 0;
	__try { return instanceImpl(object, h, reinstance); }
	__except(logException(GetExceptionInformation(), "instance callback")) { }
	return 0;
}

static int reinstance(void *object, RwResEntry *res, DPInstanceCB cb)
{
	return cb == NULL || cb(object, (DPResEntryHeader*)(res + 1), 1) != 0;
}

static RxPipeline *createPipe(unsigned id)
{
	plog("creating building pipeline id %08X", id);
	RxPipeline *pipe = (RxPipeline*)dpPipeCreate();
	void *def = dpAllInOneDef();
	if(!pipe) return NULL;
	void *lp = dpPipeLock(pipe);
	if(!lp || !dpPipeAddFragment(lp, NULL, def, NULL) || !dpPipeUnlock(lp)){
		dpPipeDestroy(pipe);
		return NULL;
	}
	void *node = dpPipeFindNode(pipe, *(const char**)def, NULL, NULL);   // RxNodeDefinition::name is the first member
	dpSetInstanceCB(node, instancePS2);
	dpSetReinstanceCB(node, reinstance);
	dpSetRenderCB(node, (DPRenderCB)ps2BuildingRender);
	createShaders();
	pipe->pluginId = id;
	pipe->pluginData = id;
	plog("pipeline %p ready, shaders vs %p vsfx %p ps %p psenv %p", pipe, vsBuilding, vsBuildingFx, psSimple, psEnvFx);
	return pipe;
}
static RxPipeline *createBuildingPipe(void)   { return createPipe(ID_BUILDING); }
static RxPipeline *createBuildingDNPipe(void) { return createPipe(ID_BUILDING_DN); }

void PS2Pipe_Install(const PS2PipeConfig *c)
{
	cfg = c;
	// the two creator hooks only rewrite a rel32, so both addresses must really be CALL instructions
	unsigned char b1 = *(unsigned char*)0x5D7100, b2 = *(unsigned char*)0x5D7D90, b3 = *(unsigned char*)0x5D7200;
	plog("install: bytes at 5D7100 = %02X, 5D7D90 = %02X (expect E8), 5D7200 = %02X", b1, b2, b3);
	if(b1 != 0xE8 || b2 != 0xE8){
		plog("not a call at the hook sites: PS2 building pipe NOT installed (game keeps its own pipelines)");
		return;
	}
	InjectHook(0x5D7100, createBuildingDNPipe);   // CCustomBuildingDNPipeline::CreateCustomObjPipe
	InjectHook(0x5D7D90, createBuildingPipe);     // CCustomBuildingPipeline::CreateCustomObjPipe
	Patch<unsigned char>(0x5D7200, 0xC3);         // the game's CPU colour interpolation is replaced by the shader blend
	plog("hooks installed");
}
