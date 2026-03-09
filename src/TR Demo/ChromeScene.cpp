// ChromeScene.cpp — Deep Space Dark Chrome
// RXDK / Original Xbox — DX8 fixed-function, 25 seconds, no shaders.
//
// Assets:
//   D:\tex\chrome.dds      (DXT5) — tangent-space normal map
//   D:\tex\chrome_diff.dds (DXT1) — diffuse albedo
//   D:\tex\chrome_bmp.dds  (DXT1) — bump/specular mask
//
// Techniques: per-vertex tangent-space DOT3 bump lighting with dual light
// bake (warm primary + cold blue-purple fill) into vertex colour; animated
// ripple via D3DTS_TEXTURE0 UV offset + rotation; camera-space reflection
// mapping (D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR); additive specular and
// rim light passes.  Float->int via inline asm fistp, no __ftol2_sse.


#include "ChromeScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

// -----------------------------------------------------------------------------
// RXDK-safe float->int helpers
// -----------------------------------------------------------------------------
static __forceinline int Ftoi(float f)
{
    int i;
    __asm
    {
        fld  f
        fistp i
    }
    return i;
}

static __forceinline BYTE ClampByteFromFloat(float f)
{
    int v = Ftoi(f);
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

static __forceinline DWORD ARGB(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return D3DCOLOR_ARGB(a, r, g, b);
}

// -----------------------------------------------------------------------------
// Textures
// -----------------------------------------------------------------------------
static LPDIRECT3DTEXTURE8 s_texChrome = nullptr;  // env/reflection layer
static LPDIRECT3DTEXTURE8 s_texDiff = nullptr;  // diffuse albedo
static LPDIRECT3DTEXTURE8 s_texMask = nullptr;  // spec/gloss mask

static bool  s_active = false;
static float s_lastT = 0.f;
static bool  s_hasLast = false;

// -----------------------------------------------------------------------------
// Mesh
// -----------------------------------------------------------------------------
struct Vtx
{
    float x, y, z;
    float nx, ny, nz;
    DWORD color;   // tangent-space light vector encoded as RGB, updated per-frame
    float u, v;
};
#define FVF_VTX (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static LPDIRECT3DVERTEXBUFFER8 s_vb = nullptr;
static LPDIRECT3DINDEXBUFFER8  s_ib = nullptr;
static int s_numVerts = 0;
static int s_numIndices = 0;

static void CreateSphere(float r, int segU, int segV)
{
    s_numVerts = (segU + 1) * (segV + 1);
    s_numIndices = segU * segV * 6;

    g_pDevice->CreateVertexBuffer(s_numVerts * sizeof(Vtx), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, FVF_VTX, D3DPOOL_DEFAULT, &s_vb);
    g_pDevice->CreateIndexBuffer(s_numIndices * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &s_ib);

    Vtx* v = nullptr;
    s_vb->Lock(0, 0, (BYTE**)&v, 0);
    int vi = 0;
    for (int y = 0; y <= segV; ++y)
    {
        float fv = (float)y / (float)segV;
        float phi = (fv - 0.5f) * D3DX_PI;
        float cp = cosf(phi), sp = sinf(phi);
        for (int x = 0; x <= segU; ++x)
        {
            float fu = (float)x / (float)segU;
            float th = fu * (D3DX_PI * 2.0f);
            float ct = cosf(th), st = sinf(th);
            float nx = cp * ct, ny = sp, nz = cp * st;
            v[vi].x = nx * r; v[vi].y = ny * r; v[vi].z = nz * r;
            v[vi].nx = nx;   v[vi].ny = ny;   v[vi].nz = nz;
            v[vi].u = fu;
            v[vi].v = 1.0f - fv;
            v[vi].color = 0xFFFFFFFF;  // will be overwritten per-frame
            ++vi;
        }
    }
    s_vb->Unlock();

    WORD* ib = nullptr;
    s_ib->Lock(0, 0, (BYTE**)&ib, 0);
    int ii = 0;
    for (int y = 0; y < segV; ++y)
        for (int x = 0; x < segU; ++x)
        {
            WORD i0 = (WORD)(y * (segU + 1) + x);
            WORD i1 = i0 + 1, i2 = i0 + (segU + 1), i3 = i2 + 1;
            ib[ii++] = i0; ib[ii++] = i2; ib[ii++] = i3;
            ib[ii++] = i0; ib[ii++] = i3; ib[ii++] = i1;
        }
    s_ib->Unlock();
}

static void ReleaseMesh()
{
    if (s_vb) { s_vb->Release(); s_vb = nullptr; }
    if (s_ib) { s_ib->Release(); s_ib = nullptr; }
    s_numVerts = s_numIndices = 0;
}

static void ReleaseTextures()
{
    if (s_texChrome) { s_texChrome->Release(); s_texChrome = nullptr; }
    if (s_texDiff) { s_texDiff->Release();   s_texDiff = nullptr; }
    if (s_texMask) { s_texMask->Release();   s_texMask = nullptr; }
}

// -----------------------------------------------------------------------------
// Background
// -----------------------------------------------------------------------------
struct V2D { float x, y, z, rhw; DWORD color; };
#define FVF_V2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void Begin2D(bool additive)
{
    g_pDevice->SetVertexShader(FVF_V2D);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetTexture(2, NULL);
    g_pDevice->SetTexture(3, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(3, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(3, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    if (additive)
    {
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    else
    {
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
}

static void DrawNebulaGradient(float t)
{
    Begin2D(false);
    BYTE topR = ClampByteFromFloat(8.0f + 5.0f * sinf(t * 0.17f));
    BYTE topG = ClampByteFromFloat(6.0f + 4.0f * sinf(t * 0.19f + 1.2f));
    BYTE topB = ClampByteFromFloat(18.0f + 8.0f * sinf(t * 0.15f + 0.6f));
    BYTE botR = ClampByteFromFloat(18.0f + 8.0f * sinf(t * 0.22f + 0.8f));
    BYTE botG = ClampByteFromFloat(5.0f + 5.0f * sinf(t * 0.21f));
    BYTE botB = ClampByteFromFloat(30.0f + 12.0f * sinf(t * 0.18f + 1.1f));
    DWORD cTop = ARGB(255, topR, topG, topB);
    DWORD cBot = ARGB(255, botR, botG, botB);
    V2D q[4] =
    {
        {0.f,     0.f,     0.f,1.f,cTop},
        {SCREEN_W,0.f,     0.f,1.f,cTop},
        {0.f,     SCREEN_H,0.f,1.f,cBot},
        {SCREEN_W,SCREEN_H,0.f,1.f,cBot},
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
}

static void DrawLensGlow(float t)
{
    Begin2D(true);
    const float cx = SCREEN_W * 0.5f, cy = SCREEN_H * 0.5f;
    float pulse = 0.65f + 0.35f * sinf(t * 0.75f);
    float R0 = 80.0f + 18.0f * pulse, R1 = 190.0f + 30.0f * pulse;
    DWORD c0 = ARGB(0, 0, 0, 0);
    DWORD c1 = ARGB(70, 140, 80, 255);
    DWORD c2 = ARGB(55, 80, 190, 255);
    const int SEG = 40;
    for (int i = 0; i < SEG; ++i)
    {
        float a0 = (float)i * (D3DX_PI * 2.f) / SEG;
        float a1 = (float)(i + 1) * (D3DX_PI * 2.f) / SEG;
        V2D t0[3] = { {cx,cy,0,1,c1}, {cx + cosf(a0) * R1,cy + sinf(a0) * R1,0,1,c0}, {cx + cosf(a1) * R1,cy + sinf(a1) * R1,0,1,c0} };
        V2D t1[3] = { {cx,cy,0,1,c2}, {cx + cosf(a0) * R0,cy + sinf(a0) * R0,0,1,c0}, {cx + cosf(a1) * R0,cy + sinf(a1) * R0,0,1,c0} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t0, sizeof(V2D));
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t1, sizeof(V2D));
    }
}

// -----------------------------------------------------------------------------
// Starfield
// -----------------------------------------------------------------------------
struct Star { float x, y, vy, phase, base; };
static const int STAR_COUNT = 320;
static Star      s_stars[STAR_COUNT];
static bool      s_starInit = false;
static unsigned  s_rng = 0xC0FFEEu;

static __forceinline unsigned RandU32() { s_rng = s_rng * 1664525u + 1013904223u; return s_rng; }
static __forceinline float    Rand01() { return (float)(RandU32() & 0x00FFFFFFu) / 16777215.f; }

static void InitStars()
{
    if (s_starInit) return;
    s_rng ^= (unsigned)GetTickCount();
    for (int i = 0; i < STAR_COUNT; ++i)
    {
        float z = Rand01();
        s_stars[i] = { Rand01() * SCREEN_W, Rand01() * SCREEN_H,
                       8.f + 42.f * z, Rand01() * 6.2831853f, 70.f + 170.f * z };
    }
    s_starInit = true;
}

static void UpdateStars(float dt)
{
    for (int i = 0; i < STAR_COUNT; ++i)
    {
        s_stars[i].y += s_stars[i].vy * dt;
        if (s_stars[i].y > SCREEN_H + 6.f)
        {
            float z = Rand01();
            s_stars[i] = { Rand01() * SCREEN_W, -6.f, 8.f + 42.f * z, s_stars[i].phase, 70.f + 170.f * z };
        }
    }
}

static void DrawStars(float t)
{
    Begin2D(true);
    for (int i = 0; i < STAR_COUNT; ++i)
    {
        float tw = 0.75f + 0.25f * sinf(t * 2.15f + s_stars[i].phase);
        BYTE br = ClampByteFromFloat(s_stars[i].base * tw);
        BYTE b = ClampByteFromFloat((float)br * 1.05f);
        DWORD col = ARGB(br, br, br, b);
        float sz = 1.f + 0.6f * tw;
        V2D q[4] =
        {
            {s_stars[i].x,    s_stars[i].y,    0,1,col},
            {s_stars[i].x + sz, s_stars[i].y,    0,1,col},
            {s_stars[i].x,    s_stars[i].y + sz, 0,1,col},
            {s_stars[i].x + sz, s_stars[i].y + sz, 0,1,col},
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }
}

// -----------------------------------------------------------------------------
// 3D helpers
// -----------------------------------------------------------------------------
static void HardReset3D()
{
    // Full opaque state — wipes any 2D blend leakage before drawing the sphere
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);  // we handle tint via TFACTOR
}

static void SetupCamera(float t)
{
    float camR = 3.1f;
    float camY = 0.55f + 0.15f * sinf(t * 0.35f);
    D3DXVECTOR3 eye(cosf(t * 0.22f) * camR, camY, sinf(t * 0.22f) * camR);
    D3DXVECTOR3 at(0, 0.05f, 0), up(0, 1, 0);
    D3DXMATRIX v, p;
    D3DXMatrixLookAtLH(&v, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&p, D3DX_PI / 3.2f, SCREEN_W / SCREEN_H, 0.1f, 50.f);
    g_pDevice->SetTransform(D3DTS_VIEW, &v);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &p);
}

static void SetLinear(int stage)
{
    g_pDevice->SetTextureStageState(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
}

static void UndoChromeState()
{
    // Full wipe of all 4 texture stages — not just 1 and 2.
    // Previous scenes may leave dirty TEXCOORDINDEX, TEXTURETRANSFORMFLAGS,
    // or stale textures in any slot.  This is the only safe cleanup.
    D3DXMATRIX identTex;
    D3DXMatrixIdentity(&identTex);
    for (int si = 0; si < 4; ++si)
    {
        g_pDevice->SetTexture(si, NULL);
        g_pDevice->SetTextureStageState(si, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_TEXCOORDINDEX, si);
        g_pDevice->SetTextureStageState(si, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pDevice->SetTransform(
            (D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + si), &identTex);
    }
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
}

// -----------------------------------------------------------------------------
// Public interface
// -----------------------------------------------------------------------------
// -----------------------------------------------------------------------------
// Stepped loader — one asset per frame so disk I/O never spikes
// -----------------------------------------------------------------------------
// Load order:
//   Step 0: CreateSphere (CPU only, no disk)
//   Step 1: load chrome.dds      (disk)
//   Step 2: load chrome_diff.dds (disk)
//   Step 3: load chrome_bmp.dds  (disk)
//   Step 4: done — s_active = true
//
// Render draws background + stars every frame regardless of load state.
// Sphere draw is gated on s_active so it only appears once all assets exist.
// Music_Update continues uninterrupted — only one texture load per frame.
// -----------------------------------------------------------------------------
static int s_loadStep = 0;

void ChromeScene_Init()
{
    if (!g_pDevice) return;

    // Wipe all texture and render state inherited from previous scene
    // before releasing or creating any resources.
    UndoChromeState();

    ReleaseMesh();
    ReleaseTextures();
    InitStars();

    s_active = false;  // not ready until all steps complete
    s_loadStep = 0;
    s_hasLast = false;
}

static void ChromeScene_StepLoad()
{
    switch (s_loadStep)
    {
    case 0:
        CreateSphere(1.0f, 64, 48);
        s_loadStep = 1;
        break;
    case 1:
        D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\chrome.dds", &s_texChrome);
        s_loadStep = 2;
        break;
    case 2:
        D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\chrome_diff.dds", &s_texDiff);
        s_loadStep = 3;
        break;
    case 3:
        D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\chrome_bmp.dds", &s_texMask);
        s_loadStep = 4;
        break;
    case 4:
        s_active = true;  // all assets present — enable 3D render
        break;
    default:
        break;
    }
}

void ChromeScene_Shutdown()
{
    s_active = false;
    if (g_pDevice) UndoChromeState();
    ReleaseMesh();
    ReleaseTextures();
}

void ChromeScene_Render(float demoTime)
{
    if (!g_pDevice) return;

    // Tick loader — one asset per frame until s_active becomes true
    if (!s_active) ChromeScene_StepLoad();

    float t = demoTime;
    float dt = 0.016f;
    if (!s_hasLast) { s_hasLast = true; s_lastT = t; }
    else
    {
        dt = t - s_lastT;
        s_lastT = t;
        if (dt < 0.f)   dt = 0.f;
        if (dt > 0.05f) dt = 0.05f;
    }

    // ── Background ───────────────────────────────────────────────────────────
    DrawNebulaGradient(t);
    UpdateStars(dt);
    DrawStars(t);
    DrawLensGlow(t);

    // 3D sphere — only once all assets have loaded
    if (!s_active || !s_vb || !s_ib) return;
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    for (int si = 0; si < 4; ++si)
    {
        g_pDevice->SetTexture(si, NULL);
        g_pDevice->SetTextureStageState(si, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    SetupCamera(t);

    // World matrix
    D3DXMATRIX ry, rx, world;
    D3DXMatrixRotationY(&ry, t * 0.65f);
    D3DXMatrixRotationX(&rx, 0.35f + 0.10f * sinf(t * 0.27f));
    D3DXMatrixMultiply(&world, &rx, &ry);
    g_pDevice->SetTransform(D3DTS_WORLD, &world);

    g_pDevice->SetVertexShader(FVF_VTX);
    g_pDevice->SetStreamSource(0, s_vb, sizeof(Vtx));
    g_pDevice->SetIndices(s_ib, 0);
    SetLinear(0); SetLinear(1); SetLinear(2);

    // ── Per-vertex tangent-space point light ──────────────────────────────────
    // Primary: warm white, orbits slowly in front of camera.
    // Fill: cold blue-purple, opposite hemisphere, 25% intensity.
    // Both baked into vertex colour together — single DOT3 pass reads both.
    float la = t * 0.18f;
    float lb = t * 0.12f;
    float plx = 1.8f * cosf(la);
    float ply = 1.0f + 0.3f * sinf(lb);
    float plz = 1.8f * sinf(la);

    // Fill light: opposite hemisphere + slight downward offset, cold blue tint.
    // Orbits at a different rate so the two highlights never fully cancel.
    float fla = la + D3DX_PI + 0.4f * sinf(t * 0.09f);
    float flx = 2.2f * cosf(fla);
    float fly = -0.6f - 0.2f * sinf(lb * 1.3f);
    float flz = 2.2f * sinf(fla);

    Vtx* verts = nullptr;
    s_vb->Lock(0, 0, (BYTE**)&verts, 0);
    for (int vi = 0; vi < s_numVerts; ++vi)
    {
        Vtx& v = verts[vi];

        // World position of this vertex
        float wx = v.x * world._11 + v.y * world._21 + v.z * world._31 + world._41;
        float wy = v.x * world._12 + v.y * world._22 + v.z * world._32 + world._42;
        float wz = v.x * world._13 + v.y * world._23 + v.z * world._33 + world._43;

        // Primary light direction: vertex -> primary light, world space
        float pdx = plx - wx, pdy = ply - wy, pdz = plz - wz;
        float pdl = sqrtf(pdx * pdx + pdy * pdy + pdz * pdz);
        if (pdl < 0.0001f) pdl = 1.f;
        pdx /= pdl; pdy /= pdl; pdz /= pdl;

        // Fill light direction: vertex -> fill light, world space
        float fdx = flx - wx, fdy = fly - wy, fdz = flz - wz;
        float fdl = sqrtf(fdx * fdx + fdy * fdy + fdz * fdz);
        if (fdl < 0.0001f) fdl = 1.f;
        fdx /= fdl; fdy /= fdl; fdz /= fdl;

        // Both into object space
        float pox = pdx * world._11 + pdy * world._12 + pdz * world._13;
        float poy = pdx * world._21 + pdy * world._22 + pdz * world._23;
        float poz = pdx * world._31 + pdy * world._32 + pdz * world._33;

        float fox = fdx * world._11 + fdy * world._12 + fdz * world._13;
        float foy = fdx * world._21 + fdy * world._22 + fdz * world._23;
        float foz = fdx * world._31 + fdy * world._32 + fdz * world._33;

        // Tangent frame
        float nx = v.nx, ny = v.ny, nz = v.nz;
        float tx = -nz, ty = 0.f, tz = nx;
        float tl = sqrtf(tx * tx + tz * tz);
        if (tl < 0.0001f) { tx = 1.f; tz = 0.f; }
        else { tx /= tl; tz /= tl; }
        float bx = ny * tz - nz * ty;
        float by = nz * tx - nx * tz;
        float bz = nx * ty - ny * tx;

        // Primary in tangent space
        float pt = pox * tx + poy * ty + poz * tz;
        float pb = pox * bx + poy * by + poz * bz;
        float pn = pox * nx + poy * ny + poz * nz;

        // Fill in tangent space — weighted 25%
        const float FILL_W = 0.25f;
        float ft = (fox * tx + foy * ty + foz * tz) * FILL_W;
        float fb = (fox * bx + foy * by + foz * bz) * FILL_W;
        float fn = (fox * nx + foy * ny + foz * nz) * FILL_W;

        // Combine: primary warm white + fill cold blue-purple
        // Primary scale 0.65 — fill already weighted, add into it.
        const float S = 0.65f;
        // T channel: blend primary (white) and fill (blue-purple tinted)
        float combT = pt * S + ft * 0.6f;   // fill blue bias reduces R contribution
        float combB = pb * S + fb * 0.6f;
        float combN = pn * S + fn * 0.9f;   // fill stronger in N channel = blue tint

        BYTE cr = ClampByteFromFloat((combT * 0.5f + 0.5f) * 255.f);
        BYTE cg = ClampByteFromFloat((combB * 0.5f + 0.5f) * 255.f);
        BYTE cb = ClampByteFromFloat((combN * 0.5f + 0.5f) * 255.f);
        v.color = D3DCOLOR_ARGB(255, cr, cg, cb);
    }
    s_vb->Unlock();

    DWORD dotLight = 0;  // unused — DOT3 reads D3DTA_DIFFUSE

    // ── Animated ripple texture matrix ───────────────────────────────────────
    // Slow sinusoidal UV offset + gentle rotation applied to stage 0 (normal map).
    // Makes the chrome surface look liquid — shifting surface detail.
    // Two independent frequencies prevent a metronomic feel.
    float uOff = 0.018f * sinf(t * 0.31f) + 0.008f * sinf(t * 0.73f);
    float vOff = 0.018f * cosf(t * 0.27f) + 0.008f * cosf(t * 0.61f);
    float rot = 0.04f * sinf(t * 0.19f);
    float cr2 = cosf(rot), sr2 = sinf(rot);

    // D3D texture matrix for 2D transform: row-major, applied to (u,v,0,1)
    D3DXMATRIX texMat;
    D3DXMatrixIdentity(&texMat);
    texMat._11 = cr2;  texMat._12 = sr2;
    texMat._21 = -sr2;  texMat._22 = cr2;
    texMat._31 = uOff; texMat._32 = vOff;  // translation in row 3 (w row)

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 1 — DOT3 bump × diffuse  (opaque, writes Z)
    //
    // Stage 0: DOTPRODUCT3(chrome.dds normal map, TFACTOR light)
    //   Greyscale bump intensity, capped at 60% so highlights don't blow out.
    //
    // Stage 1: MODULATE by chrome_diff.dds
    //   Applies surface colour to the bump-lit intensity.
    //
    // Stage 2: ADD a dim TFACTOR ambient
    //   Lifts the shadow side off pure black so surface detail stays visible
    //   in unlit areas.  Ambient is dark blue-grey — matches the nebula bg.
    // ─────────────────────────────────────────────────────────────────────────
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // Apply ripple matrix to stage 0 — animates the normal map sample
    g_pDevice->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0), &texMat);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);

    // Stage 0: DOT3 — normal map dotted against per-vertex tangent-space light
    // D3DTA_DIFFUSE carries the combined primary+fill light direction baked above.
    g_pDevice->SetTexture(0, s_texChrome);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    // Stage 1: modulate by diffuse — dark base keeps deep space feel
    g_pDevice->SetTexture(1, s_texDiff);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s_numVerts, 0, s_numIndices / 3);

    // Restore texture transform after pass 1 so it doesn't bleed into other passes
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    D3DXMATRIX identTex; D3DXMatrixIdentity(&identTex);
    g_pDevice->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0), &identTex);

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 2 — Environment reflection  (additive, no Z write)
    //
    // D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR makes the hardware generate UVs
    // from the per-vertex reflection vector (view-space reflect(N, V)).
    // Binding chrome.dds here makes it read as a genuine reflection map —
    // the texture appears to slide across the surface as the sphere rotates,
    // exactly like real chrome.  This is the single biggest "wow" factor.
    //
    // Intensity modulated by bump map so reflection is stronger on raised
    // features and weaker in recesses — gives depth to the chrome read.
    // ─────────────────────────────────────────────────────────────────────────
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    // Reflection intensity: slow pulse so it feels alive, kept at ~45% max
    float reflPulse = 0.35f + 0.10f * sinf(t * 0.62f);
    BYTE  refI = ClampByteFromFloat(reflPulse * 255.f);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(refI, refI, refI));

    // Stage 0: chrome.dds sampled via hardware reflection vector UVs
    g_pDevice->SetTexture(0, s_texChrome);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
        D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);

    // Stage 1: modulate by bump map — reflection stronger on raised features
    g_pDevice->SetTexture(1, s_texMask);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);  // use mesh UVs for mask

    g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s_numVerts, 0, s_numIndices / 3);

    // Restore TEXCOORDINDEX to default for subsequent passes
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 3 — Specular highlight  (additive, no Z write)
    //
    // Bump map used as a tight specular mask — bright where surface is raised.
    // Dim TFACTOR keeps it from blowing out.  Shadow-side suppression via
    // a second MODULATE by the light elevation scale.
    // ─────────────────────────────────────────────────────────────────────────
    float specCycle = 0.5f + 0.5f * sinf(t * 0.38f);
    BYTE  spR = ClampByteFromFloat((0.30f + 0.12f * specCycle) * 255.f);
    BYTE  spG = ClampByteFromFloat((0.26f + 0.10f * specCycle) * 255.f);
    BYTE  spB = ClampByteFromFloat((0.38f + 0.10f * (1.f - specCycle)) * 255.f);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(spR, spG, spB));

    g_pDevice->SetTexture(0, s_texMask);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    // Side-scale: how much of the sphere faces the light — derived from
    // point light elevation (ply) so specular is stronger when light is high.
    float sideScale = 0.5f + 0.5f * (ply / (fabsf(ply) + 1.0f));
    BYTE sideR = ClampByteFromFloat(sideScale * 0.85f * 255.f);
    BYTE sideG = ClampByteFromFloat(sideScale * 0.85f * 255.f);
    BYTE sideB = ClampByteFromFloat(sideScale * 0.95f * 255.f);
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(sideR, sideG, sideB));
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s_numVerts, 0, s_numIndices / 3);

    // ─────────────────────────────────────────────────────────────────────────
    // PASS 4 — Rim light  (additive, no Z write)
    //
    // A second light from the opposite side of the primary, dim and cool blue.
    // Catches the silhouette edge of the sphere — the classic chrome "halo"
    // that makes a metallic object read as 3D against a dark background.
    // Encoded as a second DOT3 pass against a back-facing light vector.
    // ─────────────────────────────────────────────────────────────────────────
    // Rim light direction: opposite side of sphere from the point light,
    // derived from point light position relative to sphere centre.
    float rimX = -(plx / (fabsf(plx) + fabsf(ply) + fabsf(plz) + 0.001f)) * 0.7f + 0.3f;
    float rimY = -(ply / (fabsf(plx) + fabsf(ply) + fabsf(plz) + 0.001f)) * 0.7f - 0.2f;
    float rimZ = -(plz / (fabsf(plx) + fabsf(ply) + fabsf(plz) + 0.001f)) * 0.7f;
    float rlen = sqrtf(rimX * rimX + rimY * rimY + rimZ * rimZ);
    if (rlen < 0.0001f) rlen = 1.f;
    rimX /= rlen; rimY /= rlen; rimZ /= rlen;

    BYTE rimR = ClampByteFromFloat((rimX * 0.5f + 0.5f) * 255.f);
    BYTE rimG = ClampByteFromFloat((rimY * 0.5f + 0.5f) * 255.f);
    BYTE rimB = ClampByteFromFloat((rimZ * 0.5f + 0.5f) * 255.f);

    // Scale rim down — it should be a subtle cool-blue edge, not a fill light
    float rimScale = 0.28f + 0.08f * sinf(t * 0.44f);
    BYTE  rimTR = ClampByteFromFloat((float)rimR * rimScale * 0.6f);
    BYTE  rimTG = ClampByteFromFloat((float)rimG * rimScale * 0.7f);
    BYTE  rimTB = ClampByteFromFloat((float)rimB * rimScale * 1.0f);  // blue bias

    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rimR, rimG, rimB));

    // Stage 0: DOT3 against rim light
    g_pDevice->SetTexture(0, s_texChrome);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    // Stage 1: tint with cool blue-biased rim colour
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rimTR, rimTG, rimTB));
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s_numVerts, 0, s_numIndices / 3);

    // ── Cleanup ───────────────────────────────────────────────────────────────
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    UndoChromeState();
}