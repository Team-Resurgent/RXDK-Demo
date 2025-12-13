// XScene.cpp - Classic Xbox X + interior ribbon lights + volumetric smoke (RXDK-safe)
//
// Goals:
// - Classic Xbox logo curved blade shape
// - Bright neon green color scheme
// - Smoke fills the X volume (stays inside bounds)
// - Ribbons act as lights that brighten nearby smoke
//
// RXDK-safe constraints:
// - No per-frame allocations
// - No RNG in Render (Init-only)
// - No float->int casts in Render (avoid __ftol2_sse)

#include "XScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>

#include "music.h"

extern LPDIRECT3DDEVICE8 g_pDevice;

// ------------------------------------------------------------
// Scene control
// ------------------------------------------------------------

static bool  s_active = false;
static DWORD s_startTicks = 0;
static const DWORD SCENE_DURATION_MS = 20000;

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static inline float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static DWORD ARGB(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return D3DCOLOR_ARGB(a, r, g, b);
}

// ------------------------------------------------------------
// Vertex formats
// ------------------------------------------------------------

struct Vtx3D
{
    float x, y, z;
    DWORD c;
};
#define FVF_3D (D3DFVF_XYZ | D3DFVF_DIFFUSE)

struct SmokeVtx
{
    float x, y, z;
    DWORD c;
    float u, v;
};
#define FVF_SMOKE (D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1)

// ------------------------------------------------------------
// Trig LUT
// ------------------------------------------------------------

static const int LUT_N = 1024;
static bool  s_lutReady = false;
static float s_sin[LUT_N];
static float s_cos[LUT_N];

static void BuildLUT()
{
    if (s_lutReady) return;

    for (int i = 0; i < LUT_N; ++i)
    {
        float a = (float)i * (2.0f * 3.14159265358979323846f) / (float)LUT_N;
        s_sin[i] = sinf(a);
        s_cos[i] = cosf(a);
    }
    s_lutReady = true;
}

// ------------------------------------------------------------
// Integer-only glow LUT
// ------------------------------------------------------------

static bool s_u8Ready = false;
static BYTE s_u8Glow[LUT_N];

static inline BYTE TriWaveU8(int x)
{
    x &= 1023;
    if (x < 512) return (BYTE)((x * 255) >> 9);
    return (BYTE)(((1023 - x) * 255) >> 9);
}

static void BuildU8()
{
    if (s_u8Ready) return;
    for (int i = 0; i < LUT_N; ++i) s_u8Glow[i] = TriWaveU8(i);
    s_u8Ready = true;
}

// ------------------------------------------------------------
// Deterministic PRNG (Init-only)
// ------------------------------------------------------------

static unsigned s_rng = 0xA51A7EEDu;

static unsigned RngU32()
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static float RngF(float lo, float hi)
{
    unsigned r = RngU32() & 0xFFFFu;
    float t = (float)r * (1.0f / 65535.0f);
    return lo + (hi - lo) * t;
}

// ------------------------------------------------------------
// Classic Xbox blade silhouette (curved swoosh shape)
// ------------------------------------------------------------

struct P2 { float x, y; };

static const int BLADE_PROFILE_N = 10;

// Classic Xbox curved swoosh blade
static const P2 s_blade2D[BLADE_PROFILE_N] =
{
    { -0.35f,  0.00f },   // inner left
    { -0.80f,  0.95f },   // mid curve out
    { -0.50f,  2.20f },   // upper curve in
    {  0.00f,  3.20f },   // sharp tip
    {  0.50f,  2.20f },   // upper curve in (right)
    {  0.80f,  0.95f },   // mid curve out (right)
    {  0.35f,  0.00f },   // inner right
    {  0.25f, -0.35f },   // bottom inner curve
    {  0.00f, -0.20f },   // bottom center
    { -0.25f, -0.35f },   // bottom inner curve (left)
};

static const float X_THICK_Z = 0.55f;

// ------------------------------------------------------------
// Clean outline geometry (ONE blade)
// ------------------------------------------------------------

static const int OUT_LOOP_LINES = BLADE_PROFILE_N;
static const int OUT_LINES_ONEBLADE = (OUT_LOOP_LINES * 2);

static Vtx3D s_outline[OUT_LINES_ONEBLADE * 2];
static int   s_outlineVCount = 0;
static bool  s_outlineBuilt = false;

static void BuildBladeOutline()
{
    if (s_outlineBuilt) return;

    const float zf = +X_THICK_Z * 0.5f;
    const float zb = -X_THICK_Z * 0.5f;

    int v = 0;

    // front loop
    for (int i = 0; i < BLADE_PROFILE_N; ++i)
    {
        int j = i + 1; if (j >= BLADE_PROFILE_N) j = 0;
        s_outline[v++] = { s_blade2D[i].x, s_blade2D[i].y, zf, 0xFFFFFFFF };
        s_outline[v++] = { s_blade2D[j].x, s_blade2D[j].y, zf, 0xFFFFFFFF };
    }

    // back loop
    for (int i = 0; i < BLADE_PROFILE_N; ++i)
    {
        int j = i + 1; if (j >= BLADE_PROFILE_N) j = 0;
        s_outline[v++] = { s_blade2D[i].x, s_blade2D[i].y, zb, 0xFFFFFFFF };
        s_outline[v++] = { s_blade2D[j].x, s_blade2D[j].y, zb, 0xFFFFFFFF };
    }

    s_outlineVCount = v;
    s_outlineBuilt = true;
}

// ------------------------------------------------------------
// Point-in-polygon (2D) + X volume tests
// ------------------------------------------------------------

static bool PointInPoly(const P2* poly, int n, float x, float y)
{
    bool c = false;
    int j = n - 1;
    for (int i = 0; i < n; ++i)
    {
        float yi = poly[i].y;
        float yj = poly[j].y;
        float xi = poly[i].x;
        float xj = poly[j].x;

        bool cond = ((yi > y) != (yj > y));
        if (cond)
        {
            float t = (y - yi) / (yj - yi);
            float xInt = xi + t * (xj - xi);
            if (x < xInt) c = !c;
        }
        j = i;
    }
    return c;
}

static void RotZ_LUT(int idx, float x, float y, float* ox, float* oy)
{
    float ca = s_cos[idx & 1023];
    float sa = s_sin[idx & 1023];
    *ox = x * ca - y * sa;
    *oy = x * sa + y * ca;
}

static bool InsideXVolume2D(float x, float y)
{
    static const int invRotIdx[4] = { 0, 768, 512, 256 };
    for (int k = 0; k < 4; ++k)
    {
        float rx, ry;
        RotZ_LUT(invRotIdx[k], x, y, &rx, &ry);
        if (PointInPoly(s_blade2D, BLADE_PROFILE_N, rx, ry))
            return true;
    }
    return false;
}

static bool InsideXVolume(float x, float y, float z)
{
    const float zf = +X_THICK_Z * 0.5f;
    const float zb = -X_THICK_Z * 0.5f;
    if (z < zb || z > zf) return false;
    return InsideXVolume2D(x, y);
}

// ------------------------------------------------------------
// Clamp step inside X (for ribbons + smoke drift)
// ------------------------------------------------------------

static void ClampStepInside(float* x, float* y, float* z, float* dx, float* dy, float* dz)
{
    float nx = *x + *dx;
    float ny = *y + *dy;
    float nz = *z + *dz;

    if (InsideXVolume(nx, ny, nz))
    {
        *x = nx; *y = ny; *z = nz;
        return;
    }

    float sx = *dx, sy = *dy, sz = *dz;

    for (int i = 0; i < 5; ++i)
    {
        sx *= 0.5f; sy *= 0.5f; sz *= 0.5f;
        nx = *x + sx;
        ny = *y + sy;
        nz = *z + sz;

        if (InsideXVolume(nx, ny, nz))
        {
            *x = nx; *y = ny; *z = nz;
            *dx = sx; *dy = sy; *dz = sz;
            return;
        }
    }

    *dx = 0.0f; *dy = 0.0f; *dz = 0.0f;
}

// ------------------------------------------------------------
// Interior ribbon seeds (precomputed)
// ------------------------------------------------------------

static const int FX_PTS = 1200;

struct FXPoint
{
    float x, y, z;
    int   seed;
    int   band;
};

static FXPoint s_fx[FX_PTS];
static bool    s_fxBuilt = false;

static void BuildFX()
{
    if (s_fxBuilt) return;

    s_rng ^= GetTickCount();

    const float ZW = X_THICK_Z * 0.48f;
    const float BOUNDS = 3.7f;

    int count = 0;
    int guard = 0;

    while (count < FX_PTS && guard < 250000)
    {
        ++guard;

        float x = RngF(-BOUNDS, BOUNDS);
        float y = RngF(-BOUNDS, BOUNDS);
        float z = RngF(-ZW, ZW);

        if (!InsideXVolume(x, y, z))
            continue;

        s_fx[count].x = x;
        s_fx[count].y = y;
        s_fx[count].z = z;
        s_fx[count].seed = (int)(RngU32() & 1023u);
        s_fx[count].band = (int)(RngU32() & 3u);
        ++count;
    }

    s_fxBuilt = true;
}

// ------------------------------------------------------------
// Galaxy smoke texture + particles (billboard quads)
// ------------------------------------------------------------

static LPDIRECT3DTEXTURE8 s_smokeTex = NULL;

static const int SMOKE_PTS = 800;  // Much denser smoke to fill the X
static const int SMOKE_VERTS = SMOKE_PTS * 6;

struct SmokePt
{
    float x, y, z;
    int   seedA;
    int   seedB;
    float r;
    float uo, vo;
};

static SmokePt   s_smoke[SMOKE_PTS];
static SmokeVtx  s_smokeV[SMOKE_VERTS];
static bool      s_smokeBuilt = false;

static void LoadSmokeTexture()
{
    if (s_smokeTex || !g_pDevice) return;

    const char* p0 = "D:\\tex\\cloud_256.dds";
    const char* p1 = "tex\\cloud_256.dds";

    if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, p0, &s_smokeTex)))
        D3DXCreateTextureFromFileA(g_pDevice, p1, &s_smokeTex);
}

static void ReleaseSmokeTexture()
{
    if (s_smokeTex)
    {
        s_smokeTex->Release();
        s_smokeTex = NULL;
    }
}

static void BuildSmoke()
{
    if (s_smokeBuilt) return;

    s_rng ^= (GetTickCount() + 0x6D5A2B1u);

    const float ZW = X_THICK_Z * 0.49f;
    const float BOUNDS = 3.7f;

    int count = 0;
    int guard = 0;

    while (count < SMOKE_PTS && guard < 400000)
    {
        ++guard;

        float x = RngF(-BOUNDS, BOUNDS);
        float y = RngF(-BOUNDS, BOUNDS);
        float z = RngF(-ZW, ZW);

        if (!InsideXVolume(x, y, z))
            continue;

        s_smoke[count].x = x;
        s_smoke[count].y = y;
        s_smoke[count].z = z;
        s_smoke[count].seedA = (int)(RngU32() & 1023u);
        s_smoke[count].seedB = (int)(RngU32() & 1023u);

        s_smoke[count].r = 0.18f + RngF(0.0f, 0.22f);

        s_smoke[count].uo = RngF(0.0f, 0.75f);
        s_smoke[count].vo = RngF(0.0f, 0.75f);

        ++count;
    }

    s_smokeBuilt = true;
}

static void SetupSmokeStates()
{
    g_pDevice->SetVertexShader(FVF_SMOKE);
    g_pDevice->SetTexture(0, s_smokeTex);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);

    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
}

static void EndSmokeStates()
{
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetTexture(0, NULL);
}

// ------------------------------------------------------------
// Ribbon "light probes"
// ------------------------------------------------------------

static float LightProbeAt(float x, float y, float z, DWORD tMs)
{
    int base = (int)((tMs / 7) & 1023);

    const float ZW = X_THICK_Z * 0.45f;
    const float L = 2.9f;

    float light = 0.0f;

    for (int i = 0; i < 4; ++i)
    {
        int ph = (base + i * 257) & 1023;

        float t = s_sin[ph];

        float dirx, diry;
        if (i == 0) { dirx = 0.7071067f; diry = 0.7071067f; }
        else if (i == 1) { dirx = -0.7071067f; diry = 0.7071067f; }
        else if (i == 2) { dirx = 0.7071067f; diry = -0.7071067f; }
        else { dirx = -0.7071067f; diry = -0.7071067f; }

        float lx = dirx * (t * L);
        float ly = diry * (t * L);
        float lz = s_sin[(ph + 333) & 1023] * ZW;

        float dx = x - lx;
        float dy = y - ly;
        float dz = z - lz;

        float d2 = dx * dx + dy * dy + dz * dz;
        float inv = 1.0f / (1.0f + 6.0f * d2);

        light += inv;
    }

    if (light > 1.45f) light = 1.45f;
    return light;
}

// ------------------------------------------------------------
// Smoke rendering (billboard quads)
// ------------------------------------------------------------

static void EmitQuad(int& outV, float x, float y, float z, float r, DWORD col, float u0, float v0, float u1, float v1)
{
    const float rx = r;
    const float uy = r;

    float x0 = x - rx; float x1p = x + rx;
    float y0 = y - uy; float y1p = y + uy;

    s_smokeV[outV++] = { x0,  y0,  z, col, u0, v1 };
    s_smokeV[outV++] = { x1p, y0,  z, col, u1, v1 };
    s_smokeV[outV++] = { x1p, y1p, z, col, u1, v0 };

    s_smokeV[outV++] = { x0,  y0,  z, col, u0, v1 };
    s_smokeV[outV++] = { x1p, y1p, z, col, u1, v0 };
    s_smokeV[outV++] = { x0,  y1p, z, col, u0, v0 };
}

static void RenderSmoke(const D3DXMATRIX& world, DWORD tMs)
{
    if (!s_smokeTex) return;

    g_pDevice->SetTransform(D3DTS_WORLD, &world);

    int base = (int)((tMs / 10) & 1023);

    int v = 0;

    for (int i = 0; i < SMOKE_PTS; ++i)
    {
        SmokePt& p = s_smoke[i];

        int a0 = (p.seedA + base) & 1023;
        int a1 = (p.seedB + (base >> 1)) & 1023;

        float dx = s_cos[a0] * 0.0045f;
        float dy = s_sin[a0] * 0.0045f;
        float dz = s_sin[a1] * 0.0032f;

        float x = p.x, y = p.y, z = p.z;
        ClampStepInside(&x, &y, &z, &dx, &dy, &dz);
        p.x = x; p.y = y; p.z = z;

        BYTE u = s_u8Glow[(a0 + 200) & 1023];
        int baseA = 35 + (u >> 3);  // Denser base smoke

        float L = LightProbeAt(x, y, z, tMs);

        // Xbox green tinted smoke
        int addA = 0;
        int addG = 0;
        int addR = 0;

        if (L > 1.05f) { addA = 125; addG = 80; addR = 25; }
        else if (L > 0.75f) { addA = 85;  addG = 50; addR = 15; }
        else if (L > 0.45f) { addA = 48;  addG = 25; addR = 8; }
        else if (L > 0.22f) { addA = 22;  addG = 12;  addR = 4; }

        int ia = baseA + addA;
        if (ia > 170) ia = 170;

        int ig = 180 + addG; if (ig > 255) ig = 255;  // green dominant
        int ir = 100 + addR; if (ir > 220) ir = 220;
        BYTE ib = 80;  // low blue for green tint

        float breath = 0.88f + 0.12f * s_sin[(a1 + 300) & 1023];
        float rr = p.r * breath;

        float panU = p.uo + 0.07f * s_sin[(a0 + 111) & 1023];
        float panV = p.vo + 0.07f * s_cos[(a1 + 222) & 1023];

        panU = ClampF(panU, 0.0f, 0.90f);
        panV = ClampF(panV, 0.0f, 0.90f);

        float u0 = panU;
        float v0 = panV;
        float u1 = panU + 0.22f;
        float v1 = panV + 0.22f;

        DWORD col = ARGB((BYTE)ia, (BYTE)ir, (BYTE)ig, ib);

        if (v + 6 <= SMOKE_VERTS)
            EmitQuad(v, x, y, z, rr, col, u0, v0, u1, v1);
    }

    if (v <= 0) return;

    SetupSmokeStates();
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, v / 3, s_smokeV, sizeof(SmokeVtx));
    EndSmokeStates();
}

// ------------------------------------------------------------
// Camera + line states
// ------------------------------------------------------------

static void SetupCamera()
{
    D3DXMATRIX view, proj;

    D3DXVECTOR3 eye(0.0f, 0.0f, -8.6f);
    D3DXVECTOR3 at(0.0f, 0.0f, 0.0f);
    D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);

    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, D3DX_PI / 3.0f, 640.0f / 480.0f, 0.1f, 100.0f);

    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);
}

static void SetupAdditiveLines()
{
    g_pDevice->SetVertexShader(FVF_3D);
    g_pDevice->SetTexture(0, NULL);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
}

static void EndAdditive()
{
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ------------------------------------------------------------
// Outline rendering: Xbox green neon
// ------------------------------------------------------------

static void DrawBladeOutline(const D3DXMATRIX& baseWorld, DWORD col, float scaleXY)
{
    static Vtx3D tmp[OUT_LINES_ONEBLADE * 2];

    for (int i = 0; i < s_outlineVCount; ++i)
    {
        tmp[i] = s_outline[i];
        tmp[i].x *= scaleXY;
        tmp[i].y *= scaleXY;
        tmp[i].c = col;
    }

    for (int k = 0; k < 4; ++k)
    {
        D3DXMATRIX rz;
        D3DXMatrixRotationZ(&rz, (D3DX_PI * 0.5f) * (float)k);

        D3DXMATRIX w = rz * baseWorld;
        g_pDevice->SetTransform(D3DTS_WORLD, &w);

        g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, s_outlineVCount / 2, tmp, sizeof(Vtx3D));
    }
}

static void RenderXOutlineNeon(const D3DXMATRIX& baseWorld, DWORD tMs)
{
    int ph = (int)((tMs >> 2) & 1023);
    BYTE u = s_u8Glow[ph];

    // Xbox green colors
    DWORD colCore = ARGB((BYTE)190, (BYTE)(140 + (u >> 3)), (BYTE)(235 + (u >> 4)), (BYTE)60);
    DWORD colH1 = ARGB((BYTE)80, (BYTE)100, (BYTE)200, (BYTE)30);
    DWORD colH2 = ARGB((BYTE)45, (BYTE)70, (BYTE)150, (BYTE)20);

    // Outer soft halo
    DrawBladeOutline(baseWorld, colH2, 1.060f);
    // Mid halo
    DrawBladeOutline(baseWorld, colH1, 1.032f);
    // Crisp core
    DrawBladeOutline(baseWorld, colCore, 1.000f);
}

// ------------------------------------------------------------
// Interior ribbons (clamped)
// ------------------------------------------------------------

static const int MAX_FX_LINES = 900;
static Vtx3D s_fxV[MAX_FX_LINES * 2];

static void RenderInteriorFX(const D3DXMATRIX& world, DWORD tMs)
{
    g_pDevice->SetTransform(D3DTS_WORLD, &world);

    int uv[4] = { 0,0,0,0 };
    Music_GetUVLevels(uv);

    float music = 1.0f + (float)uv[0] * 0.0040f;

    int base = (int)((tMs / 6) & 1023);

    const int RIBBONS = 12;
    const int SEGS = 10;

    int v = 0;

    for (int r = 0; r < RIBBONS && (v + (SEGS * 2)) <= (MAX_FX_LINES * 2); ++r)
    {
        const FXPoint& src = s_fx[(r * (FX_PTS / RIBBONS) + (base & 31)) % FX_PTS];

        float x = src.x;
        float y = src.y;
        float z = src.z;

        int a = (src.seed + base + r * 77) & 1023;

        float dirx, diry;
        if (src.band == 0) { dirx = 0.7071067f;  diry = 0.7071067f; }
        else if (src.band == 1) { dirx = -0.7071067f; diry = 0.7071067f; }
        else if (src.band == 2) { dirx = 0.7071067f;  diry = -0.7071067f; }
        else { dirx = -0.7071067f; diry = -0.7071067f; }

        // Xbox green ribbons
        DWORD col;
        if (src.band == 0) col = ARGB(170, 120, 240, 70);
        else if (src.band == 1) col = ARGB(170, 140, 255, 80);
        else if (src.band == 2) col = ARGB(170, 160, 255, 90);
        else col = ARGB(170, 180, 255, 100);

        float px = x, py = y, pz = z;

        for (int s = 0; s < SEGS; ++s)
        {
            int ph = (a + s * 37) & 1023;

            float curlx = s_cos[(ph + 160) & 1023] * 0.18f;
            float curly = s_sin[(ph + 160) & 1023] * 0.18f;
            float curlz = s_sin[(ph + 420) & 1023] * 0.12f;

            float step = (0.18f + 0.06f * s_sin[(ph + 40) & 1023]) * music;

            float dx = dirx * step + curlx * 0.08f;
            float dy = diry * step + curly * 0.08f;
            float dz = curlz * 0.05f;

            ClampStepInside(&x, &y, &z, &dx, &dy, &dz);
            if (dx == 0.0f && dy == 0.0f && dz == 0.0f)
                break;

            s_fxV[v++] = { px, py, pz, col };
            s_fxV[v++] = { x,  y,  z,  col };

            px = x; py = y; pz = z;
        }
    }

    if (v > 1)
        g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, v / 2, s_fxV, sizeof(Vtx3D));
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void XScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();

    BuildLUT();
    BuildU8();
    BuildBladeOutline();
    BuildFX();
    BuildSmoke();
    LoadSmokeTexture();
}

void XScene_Shutdown()
{
    s_active = false;
    ReleaseSmokeTexture();
}

bool XScene_IsFinished()
{
    if (!s_active) return true;
    return (GetTickCount() - s_startTicks) >= SCENE_DURATION_MS;
}

void XScene_Render(float)
{
    if (!s_active || !g_pDevice)
        return;

    DWORD tMs = GetTickCount() - s_startTicks;

    SetupCamera();

    float rx = (float)tMs * 0.00014f;
    float ry = (float)tMs * 0.00024f;
    float rz = (float)tMs * 0.00008f;

    D3DXMATRIX mx, my, mz, baseWorld;
    D3DXMatrixRotationX(&mx, rx);
    D3DXMatrixRotationY(&my, ry);
    D3DXMatrixRotationZ(&mz, rz);
    baseWorld = mx * my * mz;

    // 1) volumetric smoke first (alpha)
    RenderSmoke(baseWorld, tMs);

    // 2) ribbons (additive) - light sources
    SetupAdditiveLines();
    RenderInteriorFX(baseWorld, tMs);

    // 3) thick neon outline (multi-pass additive)
    RenderXOutlineNeon(baseWorld, tMs);
    EndAdditive();
}