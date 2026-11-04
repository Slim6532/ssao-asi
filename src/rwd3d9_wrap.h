#pragma once
// Shared RenderWare D3D9 wrappers (GTA SA 1.0 US addresses, taken from SkyGfx gta.cpp) and the
// few RW D3D9 pipeline structs (unused by occlusion; kept for future pipeline work).
//
// !! The three structs below are written from the RW 3.6/3.7 D3D9 SDK layout (what SkyGfx gets from
// !! the real RWSDK headers). If your header set already has RxD3D9ResEntryHeader/RxD3D9InstanceData
// !! with different field order, fix them here - a wrong layout crashes the game on the first building.
#include <windows.h>
#include <d3d9.h>
#include <rwcore.h>
#include <rpworld.h>
#include "MemoryMgr.h"

struct DPVertexStream { void *vertexBuffer; unsigned offset, stride; unsigned short geometryFlags; unsigned char managed, dynamicLock; };
struct DPResEntryHeader {
	unsigned serialNumber, numMeshes;
	void *indexBuffer;
	unsigned primType;
	DPVertexStream vertexStream[2];
	int useOffsets;
	void *vertexDeclaration;
	unsigned totalNumIndex, totalNumVertex;
};
struct DPInstanceData {
	unsigned numIndices, minVert;
	int numVertices;
	RpMaterial *material;
	void *vertexShader;
	unsigned baseIndex, startIndex, numPrimitives;
	int vertexAlpha;
};
static_assert(sizeof(DPVertexStream) == 16, "vertex stream layout");
static_assert(sizeof(DPInstanceData) == 36, "instance data layout");

typedef int (*DPRenderCB)(void*, void*, unsigned char, unsigned);
typedef int (*DPInstanceCB)(void*, DPResEntryHeader*, int);
typedef int (*DPReinstanceCB)(void*, RwResEntry*, DPInstanceCB);

#define DPW static WRAPPER
DPW void dpSetVSConst(unsigned reg, const void *data, unsigned n) { EAXJMP(0x7FACA0); }
DPW void dpSetPSConst(unsigned reg, const void *data, unsigned n) { EAXJMP(0x7FAD00); }
DPW void dpSetVS(void *s) { EAXJMP(0x7F9FB0); }
DPW void dpSetPS(void *s) { EAXJMP(0x7F9FF0); }
DPW void dpSetIndices(void *ib) { EAXJMP(0x7FA1C0); }
DPW void dpSetStreams(const void *streams, int useOffsets) { EAXJMP(0x7FA090); }
DPW void dpSetDecl(void *d) { EAXJMP(0x7F9F70); }
DPW void dpDrawIndexed(unsigned prim, int baseVertex, unsigned minIndex, unsigned numVerts, unsigned startIndex, unsigned primCount) { EAXJMP(0x7FA320); }
DPW void dpDraw(unsigned prim, unsigned start, unsigned primCount) { EAXJMP(0x7FA360); }
DPW void dpDrawIndexedUP(unsigned prim, unsigned minIndex, unsigned numVerts, unsigned primCount, const void *idx, const void *verts, unsigned stride) { EAXJMP(0x7FA1F0); }
DPW int  dpSetTexture(void *tex, unsigned stage) { EAXJMP(0x7FDE70); }
DPW void dpSetTSS(unsigned stage, unsigned type, unsigned value) { EAXJMP(0x7FC340); }
DPW void dpSetRS(unsigned state, unsigned value) { EAXJMP(0x7FC2D0); }
DPW void dpGetRS(unsigned state, void *value) { EAXJMP(0x7FC320); }
DPW int  dpCreateVS(const void *fn, void **sh) { EAXJMP(0x7FAC60); }
DPW int  dpCreatePS(const void *fn, void **sh) { EAXJMP(0x7FACC0); }
DPW int  dpCreateDecl(const void *elems, void **decl) { EAXJMP(0x7FAA30); }
DPW int  dpCreateVB(unsigned stride, unsigned size, void **vb, unsigned *offset) { EAXJMP(0x7F5500); }
DPW void dpDestroyVB(unsigned stride, unsigned size, void *vb, unsigned offset) { EAXJMP(0x7F56A0); }
DPW unsigned dpInstV3d(unsigned type, unsigned char *mem, const void *src, int n, unsigned stride) { EAXJMP(0x752AD0); }
DPW unsigned dpInstV2d(unsigned type, unsigned char *mem, const void *src, int n, unsigned stride) { EAXJMP(0x7544E0); }
DPW int  dpInstColor(unsigned char *mem, const void *rgba, int n, unsigned stride) { EAXJMP(0x754AE0); }
DPW void dpEnableClipping(void *object, unsigned type) { EAXJMP(0x756D90); }
DPW RwMatrix *dpFrameLTM(RwFrame *f) { EAXJMP(0x7F0990); }
DPW RwMatrix *dpMatInvert(RwMatrix *dst, const RwMatrix *src) { EAXJMP(0x7F2070); }
DPW RwMatrix *dpMatMultiply(RwMatrix *dst, const RwMatrix *a, const RwMatrix *b) { EAXJMP(0x7F18B0); }
DPW RwMatrix *dpMatOrthoNormalize(RwMatrix *dst, const RwMatrix *src) { EAXJMP(0x7F1920); }
DPW RwV3d *dpTransformPoint(RwV3d *dst, const RwV3d *src, const RwMatrix *m) { EAXJMP(0x7EDD60); }
DPW unsigned dpMatFXGetEffects(const RpMaterial *m) { EAXJMP(0x812140); }
DPW const void *dpMatFXGetUV(const RpMaterial *m, RwMatrix **a, RwMatrix **b) { EAXJMP(0x812A50); }
DPW void *dpPipeCreate(void) { EAXJMP(0x8057B0); }
DPW void *dpPipeLock(void *p) { EAXJMP(0x806990); }
DPW void *dpPipeAddFragment(void *locked, unsigned *first, void *nodeDef, ...) { EAXJMP(0x806BE0); }
DPW void *dpPipeUnlock(void *locked) { EAXJMP(0x805F40); }
DPW void dpPipeDestroy(void *p) { EAXJMP(0x805820); }
DPW void *dpAllInOneDef(void) { EAXJMP(0x7582E0); }
DPW void *dpPipeFindNode(void *p, const char *name, void *start, int *idx) { EAXJMP(0x806B30); }
DPW void dpSetInstanceCB(void *node, DPInstanceCB cb) { EAXJMP(0x757380); }
DPW void dpSetReinstanceCB(void *node, DPReinstanceCB cb) { EAXJMP(0x7573A0); }
DPW void dpSetRenderCB(void *node, DPRenderCB cb) { EAXJMP(0x7573E0); }

// view / projection as kept by the RW D3D9 driver
static D3DMATRIX &dpViewTf = *(D3DMATRIX*)0xC9BC80;
static D3DMATRIX &dpProjTf = *(D3DMATRIX*)0x8E2458;
static RwTexture *&dpWhiteTex = *(RwTexture**)0xB4E3EC;

static inline void dpMul4(float *c, const float *a, const float *b)
{
	for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++){
		float s = 0.0f;
		for(int k = 0; k < 4; k++) s += a[i*4+k]*b[k*4+j];
		c[i*4+j] = s;
	}
}
static inline void dpTranspose(float *d, const float *s)
{
	for(int i = 0; i < 4; i++) for(int j = 0; j < 4; j++) d[i*4+j] = s[j*4+i];
}
// proj * (view * world), each in column-vector form; world == NULL means identity. Same math as SkyGfx's pipeGetComposedTransformMatrix.
static inline void dpComposed(const RwMatrix *world, float *out)
{
	float w[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
	if(world){
		w[0]=world->right.x; w[1]=world->up.x; w[2]=world->at.x; w[3]=world->pos.x;
		w[4]=world->right.y; w[5]=world->up.y; w[6]=world->at.y; w[7]=world->pos.y;
		w[8]=world->right.z; w[9]=world->up.z; w[10]=world->at.z; w[11]=world->pos.z;
	}
	float v[16], p[16], vw[16];
	dpTranspose(v, (const float*)&dpViewTf);
	dpTranspose(p, (const float*)&dpProjTf);
	dpMul4(vw, v, w);
	dpMul4(out, p, vw);
}
