#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include "rwd3d9_wrap.h"
#include "taa.h"
#include "taa_vs.h"
#include "taa_ps.h"
#include "ssao.h"

#define REL(p) do{ if(p){ (p)->Release(); (p) = NULL; } }while(0)
static const TAAConfig *cfg;
static IDirect3DDevice9 *&gDev = *(IDirect3DDevice9**)0xC97C28;
static D3DMATRIX &gProj = *(D3DMATRIX*)0x8E2458;      // projection the RW D3D9 driver is using

static void LOG(const char *fmt, ...)
{
	if(!cfg || !cfg->log) return;
	FILE *f = fopen("dualpass_taa.log", "a"); if(!f) return;
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fclose(f);
}

static IDirect3DTexture9 *gCur, *gHist;
static IDirect3DSurface9 *gCurSurf, *gHistSurf;
static IDirect3DStateBlock9 *gSB;
static IDirect3DVertexShader9 *gVS;
static IDirect3DPixelShader9 *gPS;
static IDirect3DVertexDeclaration9 *gDecl;
static UINT gW, gH;
static D3DFORMAT gFmt;
static bool gFailed, gOff, gKeyDown, gHistValid;
static int gFrame, gCalib;                             // gCalib: frames of jitter gain calibration left
static bool gHavePrev;
static float gPrevR[3], gPrevU[3], gPrevA[3], gPrevP[3], gPrevVW[2], gPrevJit[2];
static float gGain[2];                                  // NDC shift per unit of viewOffset (measured)
static int gAxisX = -1, gAxisY = -1;                    // which proj element carries the offset (0 = [2][x], 1 = [x][2])

void TAA_Init(const TAAConfig *c) { cfg = c; }

static void dropTargets(void) { REL(gSB); REL(gCurSurf); REL(gCur); REL(gHistSurf); REL(gHist); gHistValid = false; gW = gH = 0; gFmt = D3DFMT_UNKNOWN; }
void TAA_OnReset(void) { dropTargets(); gHavePrev = false; }

static float halton(int i, int b){ float f = 1.0f, r = 0.0f; while(i > 0){ f /= b; r += f*(i % b); i /= b; } return r; }

static bool ensure(IDirect3DDevice9 *dev, const D3DSURFACE_DESC &d)
{
	if(!gVS){
		dev->CreateVertexShader((const DWORD*)g_vsTaa, &gVS);
		dev->CreatePixelShader((const DWORD*)g_psTaa, &gPS);
		D3DVERTEXELEMENT9 el[] = { {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END() };
		dev->CreateVertexDeclaration(el, &gDecl);
		if(!gVS || !gPS || !gDecl){ gFailed = true; return false; }
	}
	if(gCur && (gW != d.Width || gH != d.Height || gFmt != d.Format)) dropTargets();
	if(!gCur){
		if(FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &gCur, NULL)) ||
		   FAILED(dev->CreateTexture(d.Width, d.Height, 1, D3DUSAGE_RENDERTARGET, d.Format, D3DPOOL_DEFAULT, &gHist, NULL))){
			REL(gCur); REL(gHist); gFailed = true; LOG("target creation failed"); return false;
		}
		gCur->GetSurfaceLevel(0, &gCurSurf); gHist->GetSurfaceLevel(0, &gHistSurf);
		gW = d.Width; gH = d.Height; gFmt = d.Format; gHistValid = false;
	}
	if(!gSB && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &gSB))) return false;
	return gCurSurf && gHistSurf && gSB;
}

static void setSampler(IDirect3DDevice9 *dev, DWORD s, DWORD f)
{
	dev->SetSamplerState(s, D3DSAMP_MINFILTER, f); dev->SetSamplerState(s, D3DSAMP_MAGFILTER, f);
	dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP); dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void setJitter(RwCamera *cam, float ox, float oy) { cam->viewOffset.x = ox; cam->viewOffset.y = oy; }

bool TAA_Apply(void)
{
	IDirect3DDevice9 *dev = gDev;
	RwCamera *cam = *(RwCamera**)0xC1703C;
	if(!cfg || !cfg->enable || !dev || !cam || gFailed) return false;

	if(cfg->toggleKey){
		bool down = (GetAsyncKeyState(cfg->toggleKey) & 0x8000) != 0;
		if(down && !gKeyDown){
			DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			if(pid == GetCurrentProcessId()){ gOff = !gOff; gHistValid = false; }
		}
		gKeyDown = down;
	}
	IDirect3DTexture9 *depth = SSAO_GetDepthTexture();
	if(gOff || !depth){
		if(cam->viewOffset.x != 0.0f || cam->viewOffset.y != 0.0f) setJitter(cam, 0, 0);
		gHistValid = false; gHavePrev = false;
		static bool warned; if(!depth && !warned){ warned = true; LOG("no INTZ depth (needs [AO] ssao = 1) -> TAA inactive, FXAA/SMAA used"); }
		return false;
	}

	IDirect3DSurface9 *rt = NULL;
	if(FAILED(dev->GetRenderTarget(0, &rt)) || !rt) return false;
	D3DSURFACE_DESC d;
	if(FAILED(rt->GetDesc(&d)) || !d.Width || !d.Height){ REL(rt); return false; }
	if(!ensure(dev, d)){ REL(rt); return false; }

	// ---- measure the jitter that is really in this frame's projection (self-calibrating, no assumption about RW's formula) ----
	float ox = cam->viewOffset.x, oy = cam->viewOffset.y;
	float mx = 0.0f, my = 0.0f;
	if(fabsf(ox) > 1e-7f || fabsf(oy) > 1e-7f){
		if(gAxisX < 0){
			float a = gProj.m[2][0], b = gProj.m[0][2], c = gProj.m[2][1], e = gProj.m[1][2];
			gAxisX = fabsf(a) >= fabsf(b) ? 0 : 1;
			gAxisY = fabsf(c) >= fabsf(e) ? 0 : 1;
		}
		mx = gAxisX == 0 ? gProj.m[2][0] : gProj.m[0][2];
		my = gAxisY == 0 ? gProj.m[2][1] : gProj.m[1][2];
		if(fabsf(ox) > 1e-7f) gGain[0] = mx / ox;
		if(fabsf(oy) > 1e-7f) gGain[1] = my / oy;
	}
	if(gCalib > 0){
		gCalib--;
		if(gCalib == 0 && (fabsf(gGain[0]) < 1e-3f || fabsf(gGain[1]) < 1e-3f)){
			gFailed = true; setJitter(cam, 0, 0);
			LOG("viewOffset has no effect on the projection (gain %.5f %.5f) -> TAA disabled", gGain[0], gGain[1]);
			REL(rt); return false;
		}
	}

	RwMatrix *m = dpFrameLTM((RwFrame*)cam->object.object.parent);
	float vwx = cam->viewWindow.x, vwy = cam->viewWindow.y;
	bool valid = gHistValid && gHavePrev && gCalib <= 0;
	if(valid){
		float dx = m->pos.x-gPrevP[0], dy = m->pos.y-gPrevP[1], dz = m->pos.z-gPrevP[2];
		if(dx*dx + dy*dy + dz*dz > 20.0f*20.0f) valid = false;                       // camera cut
		if(fabsf(vwx - gPrevVW[0]) > 0.15f*vwx) valid = false;                       // zoom / fov change
	}

	// ---- resolve ----
	gSB->Capture();
	IDirect3DSurface9 *ds = NULL;
	dev->GetDepthStencilSurface(&ds);
	dev->StretchRect(rt, NULL, gCurSurf, NULL, D3DTEXF_NONE);
	dev->SetDepthStencilSurface(NULL);                                               // the depth texture is sampled below
	dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE); dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE); dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
	setSampler(dev, 0, D3DTEXF_POINT); setSampler(dev, 1, D3DTEXF_LINEAR); setSampler(dev, 2, D3DTEXF_POINT);
	dev->SetTexture(0, gCur); dev->SetTexture(1, gHist); dev->SetTexture(2, depth);

	float n = cam->nearPlane, f = cam->farPlane;
	float inv[4]  = { 1.0f/gW, 1.0f/gH, cfg->blend*0.01f, cfg->clipGamma*0.01f };
	float vw[4]   = { vwx, vwy, valid ? gPrevVW[0] : vwx, valid ? gPrevVW[1] : vwy };
	float jit[4]  = { mx, my, gPrevJit[0], gPrevJit[1] };
	float zc[4]   = { n*f, f, f-n, valid ? 1.0f : 0.0f };
	float cr[4] = { m->right.x, m->right.y, m->right.z, 0 }, cu[4] = { m->up.x, m->up.y, m->up.z, 0 };
	float ca[4] = { m->at.x, m->at.y, m->at.z, 0 },        cp[4] = { m->pos.x, m->pos.y, m->pos.z, 0 };
	float pr[4] = { gPrevR[0], gPrevR[1], gPrevR[2], 0 }, pu[4] = { gPrevU[0], gPrevU[1], gPrevU[2], 0 };
	float pa[4] = { gPrevA[0], gPrevA[1], gPrevA[2], 0 }, pp[4] = { gPrevP[0], gPrevP[1], gPrevP[2], 0 };
	dev->SetVertexShaderConstantF(8, inv, 1);
	dev->SetPixelShaderConstantF(0, inv, 1);  dev->SetPixelShaderConstantF(1, vw, 1);
	dev->SetPixelShaderConstantF(2, jit, 1);  dev->SetPixelShaderConstantF(3, zc, 1);
	dev->SetPixelShaderConstantF(4, cr, 1);   dev->SetPixelShaderConstantF(5, cu, 1);
	dev->SetPixelShaderConstantF(6, ca, 1);   dev->SetPixelShaderConstantF(7, cp, 1);
	dev->SetPixelShaderConstantF(8, pr, 1);   dev->SetPixelShaderConstantF(9, pu, 1);
	dev->SetPixelShaderConstantF(10, pa, 1);  dev->SetPixelShaderConstantF(11, pp, 1);
	dev->SetVertexDeclaration(gDecl); dev->SetVertexShader(gVS); dev->SetPixelShader(gPS);
	static const float quad[8] = { -1, 1,  1, 1,  -1, -1,  1, -1 };
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, 2*sizeof(float));
	dev->SetTexture(0, NULL); dev->SetTexture(1, NULL); dev->SetTexture(2, NULL);
	dev->SetDepthStencilSurface(ds); REL(ds);

	dev->StretchRect(rt, NULL, gHistSurf, NULL, D3DTEXF_NONE);                       // unsharpened result = next frame's history
	gSB->Apply();
	REL(rt);

	// ---- remember this camera, choose next frame's jitter ----
	gPrevR[0]=m->right.x; gPrevR[1]=m->right.y; gPrevR[2]=m->right.z;
	gPrevU[0]=m->up.x;    gPrevU[1]=m->up.y;    gPrevU[2]=m->up.z;
	gPrevA[0]=m->at.x;    gPrevA[1]=m->at.y;    gPrevA[2]=m->at.z;
	gPrevP[0]=m->pos.x;   gPrevP[1]=m->pos.y;   gPrevP[2]=m->pos.z;
	gPrevVW[0]=vwx; gPrevVW[1]=vwy; gPrevJit[0]=mx; gPrevJit[1]=my;
	gHavePrev = true; gHistValid = true;

	gFrame++;
	if(gFrame == 1){ gCalib = 3; setJitter(cam, 0.0005f, 0.0005f); }                  // tiny probe offset to measure the gain
	else if(gCalib > 0) setJitter(cam, 0.0005f, 0.0005f);
	else{
		int i = (gFrame % 8) + 1;
		float jx = (halton(i, 2) - 0.5f) * cfg->jitter * 0.01f;                        // pixels
		float jy = (halton(i, 3) - 0.5f) * cfg->jitter * 0.01f;
		float gx = gGain[0], gy = gGain[1];
		if(fabsf(gx) > 1e-3f && fabsf(gy) > 1e-3f) setJitter(cam, (jx*2.0f/gW)/gx, (jy*2.0f/gH)/gy);
	}
	if(gFrame == 20) LOG("running: gain %.4f %.4f, axis %d %d, last jitter ndc %.5f %.5f", gGain[0], gGain[1], gAxisX, gAxisY, mx, my);
	return true;
}
