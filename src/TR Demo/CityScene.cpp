// CityScene.cpp - Synthwave poster scene (2D compositor, RXDK-safe)
// ---------------------------------------------------------------
// Reference goals (poster look):
//  - Center horizon + centered vanishing point grid
//  - Skyline silhouette with magenta tops + subtle cyan side accents
//  - Big striped sun behind skyline + sun reflection below horizon
//  - Mirror-like water reflection of skyline (faded)
//  - Sparse stars
//  - Rooftop blinking red beacons (aviation lights)
//  - Gentle camera sweep + parallax (no float->int casts)
//
// RXDK-safe constraints:
//  - No per-frame allocations
//  - No RNG in Render
//  - Avoid float->int casts (prevents __ftol2_sse)
//  - No textures required
//  - Z disabled (EnableAutoDepthStencil = FALSE)

#include "CityScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <stdlib.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// ------------------------------------------------------------
// Scene control
// ------------------------------------------------------------

static bool  s_active = false;
static DWORD s_startTicks = 0;
static const DWORD SCENE_DURATION_MS = 24000;

// ------------------------------------------------------------
// DDS Logo texture (ADDED)
// ------------------------------------------------------------

static LPDIRECT3DTEXTURE8 s_logoTex = NULL;
static int s_logoW = 0;
static int s_logoH = 0;

// ------------------------------------------------------------
// DDS header structures (ADDED - from IntroScene.cpp)
// ------------------------------------------------------------

#pragma pack(push, 1)
struct DDS_PIXELFORMAT
{
    DWORD size;
    DWORD flags;
    DWORD fourCC;
    DWORD rgbBitCount;
    DWORD rMask;
    DWORD gMask;
    DWORD bMask;
    DWORD aMask;
};

struct DDS_HEADER
{
    DWORD           size;
    DWORD           flags;
    DWORD           height;
    DWORD           width;
    DWORD           pitchOrLinearSize;
    DWORD           depth;
    DWORD           mipMapCount;
    DWORD           reserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD           caps;
    DWORD           caps2;
    DWORD           caps3;
    DWORD           caps4;
    DWORD           reserved2;
};
#pragma pack(pop)

// ------------------------------------------------------------
// LUT
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
// DDS Loader (ADDED - from IntroScene.cpp)
// ------------------------------------------------------------

static LPDIRECT3DTEXTURE8 LoadTextureFromDDS(const char* path, int& outW, int& outH)
{
    outW = 0;
    outH = 0;

    if (!g_pDevice || !path)
        return NULL;

    HANDLE hFile = CreateFileA(
        path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
        return NULL;

    DWORD bytesRead = 0;

    DWORD magic = 0;
    if (!ReadFile(hFile, &magic, sizeof(DWORD), &bytesRead, NULL) ||
        bytesRead != sizeof(DWORD) ||
        magic != 0x20534444)
    {
        CloseHandle(hFile);
        return NULL;
    }

    DDS_HEADER hdr;
    if (!ReadFile(hFile, &hdr, sizeof(DDS_HEADER), &bytesRead, NULL) ||
        bytesRead != sizeof(DDS_HEADER))
    {
        CloseHandle(hFile);
        return NULL;
    }

    if (hdr.size != 124 || hdr.ddspf.size != 32)
    {
        CloseHandle(hFile);
        return NULL;
    }

    const DWORD DDPF_FOURCC = 0x4;
    const DWORD DDPF_RGB = 0x40;
    const DWORD DDPF_ALPHAPIXELS = 0x1;

    if (hdr.ddspf.flags & DDPF_FOURCC)
    {
        CloseHandle(hFile);
        return NULL;
    }

    if (hdr.ddspf.rgbBitCount != 32 ||
        (hdr.ddspf.flags & (DDPF_RGB | DDPF_ALPHAPIXELS)) != (DDPF_RGB | DDPF_ALPHAPIXELS) ||
        hdr.ddspf.rMask != 0x00FF0000 ||
        hdr.ddspf.gMask != 0x0000FF00 ||
        hdr.ddspf.bMask != 0x000000FF ||
        hdr.ddspf.aMask != 0xFF000000)
    {
        CloseHandle(hFile);
        return NULL;
    }

    int w = (int)hdr.width;
    int h = (int)hdr.height;

    if (w <= 0 || h <= 0 || w != h)
    {
        CloseHandle(hFile);
        return NULL;
    }

    if ((w & (w - 1)) != 0)
    {
        CloseHandle(hFile);
        return NULL;
    }

    DWORD pixelBytes = (DWORD)(w * h * 4);

    BYTE* pixels = (BYTE*)malloc(pixelBytes);
    if (!pixels)
    {
        CloseHandle(hFile);
        return NULL;
    }

    if (!ReadFile(hFile, pixels, pixelBytes, &bytesRead, NULL) ||
        bytesRead != pixelBytes)
    {
        free(pixels);
        CloseHandle(hFile);
        return NULL;
    }

    CloseHandle(hFile);

    LPDIRECT3DTEXTURE8 tex = NULL;
    if (FAILED(g_pDevice->CreateTexture(
        (UINT)w,
        (UINT)h,
        1,
        0,
        D3DFMT_A8R8G8B8,
        0,
        &tex)))
    {
        free(pixels);
        return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release();
        free(pixels);
        return NULL;
    }

    XGSwizzleRect(
        pixels,
        w * 4,
        NULL,
        lr.pBits,
        w,
        h,
        NULL,
        4
    );

    tex->UnlockRect(0);
    free(pixels);

    outW = w;
    outH = h;
    return tex;
}

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static inline DWORD ARGB(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return D3DCOLOR_ARGB(a, r, g, b);
}

static inline float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Triangle wave 0..511 from phase 0..1023 (integer-only)
static inline unsigned TriWave511(unsigned phase1024)
{
    phase1024 &= 1023u;
    return (phase1024 < 512u) ? phase1024 : (1023u - phase1024);
}

// ------------------------------------------------------------
// Screen constants
// ------------------------------------------------------------

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

// Poster-like horizon (concept shows buildings lower on screen)
static const float HORIZON_Y = 330.0f;        // much lower - buildings at bottom third
static const float WATER_BOTTOM_Y = 470.0f;   // near bottom

// ------------------------------------------------------------
// 2D vertex
// ------------------------------------------------------------

struct Vtx2D
{
    float x, y, z, rhw;
    DWORD c;
};

#define FVF_2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// ADDED: Textured 2D vertex for logo
struct Vtx2DT
{
    float x, y, z, rhw;
    DWORD c;
    float u, v;
};

#define FVF_2DT (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

// ------------------------------------------------------------
// Render state helpers
// ------------------------------------------------------------

static void Begin2D(bool additive)
{
    g_pDevice->SetVertexShader(FVF_2D);
    g_pDevice->SetTexture(0, NULL);

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);

    g_pDevice->SetRenderState(D3DRS_DESTBLEND, additive ? D3DBLEND_ONE : D3DBLEND_INVSRCALPHA);
}

static void End2D()
{
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// ------------------------------------------------------------
// Sky gradient + horizon glow (NO float->int casts)
// ------------------------------------------------------------

static void DrawSky(DWORD tMs)
{
    if (!g_pDevice) return;

    // Clean gradient - no banding
    Vtx2D q[4];
    q[0] = { 0.0f,     0.0f,      0.0f, 1.0f, ARGB(255,  12,  8,  50) };
    q[1] = { SCREEN_W, 0.0f,      0.0f, 1.0f, ARGB(255,  12,  8,  50) };
    q[2] = { 0.0f,     SCREEN_H,  0.0f, 1.0f, ARGB(255,  95,  8,  70) };
    q[3] = { SCREEN_W, SCREEN_H,  0.0f, 1.0f, ARGB(255,  95,  8,  70) };

    g_pDevice->SetVertexShader(FVF_2D);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx2D));
}

// ------------------------------------------------------------
// Stars (fixed pattern, no RNG)
// ------------------------------------------------------------

static void DrawStars()
{
    if (!g_pDevice) return;

    // Tiny 2x2 quads
    static const float s_xy[][2] =
    {
        {  42,  34 }, { 120,  58 }, { 188,  26 }, { 260,  72 }, { 332,  44 },
        { 418,  30 }, { 512,  66 }, { 586,  40 }, { 610,  84 }, {  80,  96 },
        { 156, 110 }, { 230,  98 }, { 392, 112 }, { 468,  92 }, { 546, 116 },
    };

    Begin2D(false);

    Vtx2D q[4];
    for (int i = 0; i < (int)(sizeof(s_xy) / sizeof(s_xy[0])); ++i)
    {
        float x = s_xy[i][0];
        float y = s_xy[i][1];

        q[0] = { x,     y,     0, 1, ARGB(200, 220, 220, 240) };
        q[1] = { x + 2, y,     0, 1, ARGB(200, 220, 220, 240) };
        q[2] = { x,     y + 2, 0, 1, ARGB(0,   220, 220, 240) };
        q[3] = { x + 2, y + 2, 0, 1, ARGB(0,   220, 220, 240) };

        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx2D));
    }

    End2D();
}

// ------------------------------------------------------------
// Sun (poster) + reflection: triangle fan, no alloc
// ------------------------------------------------------------

static const int SUN_SEGS = 48;
static bool  s_sunReady = false;
static float s_sunUx[SUN_SEGS + 1];
static float s_sunUy[SUN_SEGS + 1];

static void BuildSunCircle()
{
    if (s_sunReady) return;

    for (int i = 0; i <= SUN_SEGS; ++i)
    {
        float a = (float)i * (2.0f * 3.14159265358979323846f) / (float)SUN_SEGS;
        s_sunUx[i] = cosf(a);
        s_sunUy[i] = sinf(a);
    }
    s_sunReady = true;
}

static void DrawSunFan(float cx, float cy, float r, DWORD col, bool additive)
{
    static Vtx2D fan[SUN_SEGS + 2];

    fan[0] = { cx, cy, 0.0f, 1.0f, col };
    for (int i = 0; i <= SUN_SEGS; ++i)
        fan[i + 1] = { cx + s_sunUx[i] * r, cy + s_sunUy[i] * r, 0.0f, 1.0f, col };

    Begin2D(additive);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SUN_SEGS, fan, sizeof(Vtx2D));
    End2D();
}

static void DrawSunStripes(float cx, float cy, float r, DWORD tMs, bool isReflection)
{
    // Horizontal stripe bars - darker and more defined for contrast
    static Vtx2D stripe[4];

    const int stripes = 11;

    // Tiny wobble (sine) is fine; no float->int use
    int idx = (int)((tMs / 26u) & 1023u);
    float wob = 0.5f + 0.5f * s_sin[idx]; // 0..1

    for (int i = 0; i < stripes; ++i)
    {
        float tt = (float)i / (float)(stripes - 1);

        float yy = cy - r * 0.78f + tt * (r * 1.22f);
        yy += (wob - 0.5f) * (isReflection ? 0.8f : 1.4f);

        float hh = r * 0.065f;  // slightly thicker stripes for more definition
        float halfW = r * 0.96f;

        // Darker, more contrasty stripes
        DWORD cTop = isReflection ? ARGB(65, 4, 6, 18) : ARGB(85, 4, 6, 18);
        DWORD cBot = ARGB(0, 4, 6, 18);

        stripe[0] = { cx - halfW, yy,      0.0f, 1.0f, cTop };
        stripe[1] = { cx + halfW, yy,      0.0f, 1.0f, cTop };
        stripe[2] = { cx - halfW, yy + hh, 0.0f, 1.0f, cBot };
        stripe[3] = { cx + halfW, yy + hh, 0.0f, 1.0f, cBot };

        Begin2D(false);
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, stripe, sizeof(Vtx2D));
        End2D();
    }
}

static void DrawSunAndReflection(float sunX, float sunY, float sunR, DWORD tMs)
{
    // Soft neon glow sun - NO STRIPES, just smooth gradient
    // Outer glow layers for soft effect
    DrawSunFan(sunX, sunY, sunR * 1.50f, ARGB(60, 80, 180, 255), true);   // outermost glow
    DrawSunFan(sunX, sunY, sunR * 1.30f, ARGB(90, 90, 190, 255), true);   // mid glow
    DrawSunFan(sunX, sunY, sunR * 1.10f, ARGB(130, 100, 200, 255), true); // inner glow
    DrawSunFan(sunX, sunY, sunR, ARGB(245, 85, 210, 255), false);         // bright core

    // Sun reflection: mirrored - also soft glow
    float ry = (HORIZON_Y * 2.0f) - sunY;

    DrawSunFan(sunX, ry, sunR * 1.50f, ARGB(40, 80, 180, 255), true);
    DrawSunFan(sunX, ry, sunR * 1.30f, ARGB(60, 90, 190, 255), true);
    DrawSunFan(sunX, ry, sunR * 1.10f, ARGB(80, 100, 200, 255), true);
    DrawSunFan(sunX, ry, sunR, ARGB(180, 85, 210, 255), false);
}

// ------------------------------------------------------------
// ADDED: DDS Logo overlay on sun
// ------------------------------------------------------------

static void DrawLogoOnSun(float cx, float cy, float scale, DWORD tMs)
{
    if (!s_logoTex || s_logoW <= 0 || s_logoH <= 0)
        return;

    // Subtle pulse animation
    int idx = (int)((tMs / 28u) & 1023u);
    float p = 0.90f + 0.10f * (0.5f + 0.5f * s_sin[idx]);
    float s = scale * p;

    float w = (float)s_logoW * s;
    float h = (float)s_logoH * s;

    float left = cx - w * 0.5f;
    float right = cx + w * 0.5f;
    float top = cy - h * 0.5f;
    float bottom = cy + h * 0.5f;

    // Slightly transparent with subtle pulse
    BYTE alpha = (BYTE)(200.0f + 40.0f * (0.5f + 0.5f * s_sin[idx]));
    DWORD logoColor = ARGB(alpha, 255, 255, 255);

    Vtx2DT vLogo[4];
    vLogo[0].x = left;  vLogo[0].y = top;    vLogo[0].z = 0.0f; vLogo[0].rhw = 1.0f;
    vLogo[0].c = logoColor; vLogo[0].u = 0.0f; vLogo[0].v = 0.0f;

    vLogo[1].x = right; vLogo[1].y = top;    vLogo[1].z = 0.0f; vLogo[1].rhw = 1.0f;
    vLogo[1].c = logoColor; vLogo[1].u = 1.0f; vLogo[1].v = 0.0f;

    vLogo[2].x = left;  vLogo[2].y = bottom; vLogo[2].z = 0.0f; vLogo[2].rhw = 1.0f;
    vLogo[2].c = logoColor; vLogo[2].u = 0.0f; vLogo[2].v = 1.0f;

    vLogo[3].x = right; vLogo[3].y = bottom; vLogo[3].z = 0.0f; vLogo[3].rhw = 1.0f;
    vLogo[3].c = logoColor; vLogo[3].u = 1.0f; vLogo[3].v = 1.0f;

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    g_pDevice->SetTexture(0, s_logoTex);
    g_pDevice->SetVertexShader(FVF_2DT);

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, vLogo, sizeof(Vtx2DT));

    g_pDevice->SetTexture(0, NULL);
}

// ------------------------------------------------------------
// Procedural "TR" mark centered in sun (no font, no textures)
// ------------------------------------------------------------

static void DrawQuad(float x0, float y0, float x1, float y1, DWORD c0, DWORD c1)
{
    Vtx2D q[4];
    q[0] = { x0, y0, 0, 1, c0 };
    q[1] = { x1, y0, 0, 1, c0 };
    q[2] = { x0, y1, 0, 1, c1 };
    q[3] = { x1, y1, 0, 1, c1 };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx2D));
}

static void DrawTRMark(float cx, float cy, float scale, DWORD tMs)
{
    if (!g_pDevice) return;

    // Subtle pulse (no float->int)
    int idx = (int)((tMs / 28u) & 1023u);
    float p = 0.90f + 0.10f * (0.5f + 0.5f * s_sin[idx]);
    float s = scale * p;

    // Colors: bright cyan core with magenta glow (like synthwave)
    DWORD glow = ARGB(90, 255, 40, 200);
    DWORD core = ARGB(220, 210, 255, 255);

    // Geometry (simple block letters)
    // Total width ~ 92*s, height ~ 52*s
    float w = 92.0f * s;
    float h = 52.0f * s;

    float x0 = cx - w * 0.5f;
    float y0 = cy - h * 0.5f;

    // Stroke thickness
    float t = 7.0f * s;

    // Glow pass (additive, offset)
    Begin2D(true);
    {
        // T: top bar
        DrawQuad(x0 + 0.0f, y0 + 0.0f, x0 + 38.0f * s, y0 + t, glow, ARGB(0, 0, 0, 0));
        // T: stem
        DrawQuad(x0 + 16.0f * s, y0 + 0.0f, x0 + 16.0f * s + t, y0 + 52.0f * s, glow, ARGB(0, 0, 0, 0));

        // R: left stem
        DrawQuad(x0 + 50.0f * s, y0 + 0.0f, x0 + 50.0f * s + t, y0 + 52.0f * s, glow, ARGB(0, 0, 0, 0));
        // R: top bar
        DrawQuad(x0 + 50.0f * s, y0 + 0.0f, x0 + 92.0f * s, y0 + t, glow, ARGB(0, 0, 0, 0));
        // R: mid bar
        DrawQuad(x0 + 50.0f * s, y0 + 24.0f * s, x0 + 86.0f * s, y0 + 24.0f * s + t, glow, ARGB(0, 0, 0, 0));
        // R: diagonal leg (approx as 2 quads)
        DrawQuad(x0 + 68.0f * s, y0 + 30.0f * s, x0 + 92.0f * s, y0 + 30.0f * s + t, glow, ARGB(0, 0, 0, 0));
        DrawQuad(x0 + 80.0f * s, y0 + 30.0f * s, x0 + 80.0f * s + t, y0 + 52.0f * s, glow, ARGB(0, 0, 0, 0));
    }
    End2D();

    // Core pass (normal)
    Begin2D(false);
    {
        // T
        DrawQuad(x0 + 0.0f, y0 + 0.0f, x0 + 38.0f * s, y0 + t, core, ARGB(0, 0, 0, 0));
        DrawQuad(x0 + 16.0f * s, y0 + 0.0f, x0 + 16.0f * s + t, y0 + 52.0f * s, core, ARGB(0, 0, 0, 0));

        // R
        DrawQuad(x0 + 50.0f * s, y0 + 0.0f, x0 + 50.0f * s + t, y0 + 52.0f * s, core, ARGB(0, 0, 0, 0));
        DrawQuad(x0 + 50.0f * s, y0 + 0.0f, x0 + 92.0f * s, y0 + t, core, ARGB(0, 0, 0, 0));
        DrawQuad(x0 + 50.0f * s, y0 + 24.0f * s, x0 + 86.0f * s, y0 + 24.0f * s + t, core, ARGB(0, 0, 0, 0));
        DrawQuad(x0 + 68.0f * s, y0 + 30.0f * s, x0 + 92.0f * s, y0 + 30.0f * s + t, core, ARGB(0, 0, 0, 0));
        DrawQuad(x0 + 80.0f * s, y0 + 30.0f * s, x0 + 80.0f * s + t, y0 + 52.0f * s, core, ARGB(0, 0, 0, 0));
    }
    End2D();
}

// ------------------------------------------------------------
// Skyline silhouette + reflection + accents + beacons
// Multi-layered for depth
// ------------------------------------------------------------

struct Bldg
{
    float x0, x1;   // screen space
    float h;        // height above horizon
    BYTE  style;    // 0/1/2 for accent variation
    BYTE  beacon;   // 0/1 if has rooftop light
};

// Background layer - shorter, lighter
static const Bldg s_bldgBack[] =
{
    {  20,  55,  45, 0, 0 },
    {  58,  90,  52, 1, 0 },
    {  93, 125,  48, 0, 0 },
    { 128, 165,  60, 2, 0 },
    { 168, 200,  42, 0, 0 },
    { 203, 240,  55, 1, 0 },
    { 243, 275,  50, 0, 0 },
    { 278, 315,  58, 2, 0 },
    { 318, 350,  46, 0, 0 },
    { 353, 390,  62, 1, 0 },
    { 393, 425,  49, 0, 0 },
    { 428, 465,  54, 2, 0 },
    { 468, 500,  51, 0, 0 },
    { 503, 540,  57, 1, 0 },
    { 543, 580,  48, 0, 0 },
    { 583, 620,  53, 2, 0 },
};

// Mid layer - medium height
static const Bldg s_bldgMid[] =
{
    {  15,  42,  70, 1, 0 },
    {  45,  75,  85, 0, 0 },
    {  78, 108,  78, 2, 1 },
    { 111, 140,  92, 1, 0 },
    { 143, 175,  80, 0, 0 },
    { 178, 210,  98, 2, 1 },
    { 213, 242,  75, 1, 0 },
    { 245, 278,  88, 0, 0 },
    { 281, 315,  105, 2, 1 },
    { 318, 348,  82, 1, 0 },
    { 351, 385,  95, 0, 1 },
    { 388, 420,  88, 2, 0 },
    { 423, 455,  100, 1, 1 },
    { 458, 488,  78, 0, 0 },
    { 491, 525,  92, 2, 1 },
    { 528, 558,  85, 1, 0 },
    { 561, 595,  90, 0, 1 },
    { 598, 630,  80, 2, 0 },
};

// Foreground layer - tallest, darkest
static const Bldg s_bldgFront[] =
{
    {  10,  38,  95, 1, 1 },
    {  70, 105,  115, 2, 1 },
    { 135, 168,  105, 0, 1 },
    { 195, 235,  125, 2, 1 },
    { 265, 298,  98, 1, 0 },
    { 325, 365,  110, 2, 1 },
    { 395, 430,  120, 1, 1 },
    { 460, 500,  108, 0, 1 },
    { 530, 570,  118, 2, 1 },
    { 600, 635,  102, 1, 1 },
};

static void DrawSkylineLayer(const Bldg* buildings, int count, float sweepX, BYTE topA, BYTE sideA, BYTE fillA, BYTE baseR, BYTE baseG, BYTE baseB)
{
    const float baseY = HORIZON_Y;

    Begin2D(false);

    for (int i = 0; i < count; ++i)
    {
        float x0 = buildings[i].x0 + sweepX;
        float x1 = buildings[i].x1 + sweepX;
        float y0 = baseY - buildings[i].h;
        float y1 = baseY;

        // Layered silhouettes with different darkness
        DWORD fill = ARGB(fillA, baseR, baseG, baseB);
        Vtx2D r[4];
        r[0] = { x0, y0, 0, 1, fill };
        r[1] = { x1, y0, 0, 1, fill };
        r[2] = { x0, y1, 0, 1, ARGB(fillA, baseR * 0.5f, baseG * 0.5f, baseB * 0.5f) };
        r[3] = { x1, y1, 0, 1, ARGB(fillA, baseR * 0.5f, baseG * 0.5f, baseB * 0.5f) };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, r, sizeof(Vtx2D));
    }

    End2D();

    // Magenta tops + cyan side accents (additive)
    Begin2D(true);

    for (int i = 0; i < count; ++i)
    {
        float x0 = buildings[i].x0 + sweepX;
        float x1 = buildings[i].x1 + sweepX;
        float yT = baseY - buildings[i].h;

        // top magenta bar
        DWORD topC = ARGB(topA, 255, 40, 200);
        DrawQuad(x0, yT, x1, yT + 2.0f, topC, ARGB(0, 0, 0, 0));

        // subtle cyan side accent on some buildings
        if (buildings[i].style == 2)
        {
            DWORD sideC = ARGB(sideA, 60, 220, 255);
            DrawQuad(x0, yT + 6.0f, x0 + 2.0f, baseY - 4.0f, sideC, ARGB(0, 0, 0, 0));
        }
    }

    End2D();
}

static void DrawMountainRange(float sweep)
{
    const float baseY = HORIZON_Y;

    // Much taller, more prominent mountain peaks
    struct Peak { float x; float h; };
    static const Peak peaks[] = {
        {  50, 85 },
        { 130, 105 },
        { 210, 92 },
        { 290, 110 },
        { 370, 98 },
        { 450, 108 },
        { 530, 90 },
        { 600, 100 },
    };

    Begin2D(false);

    for (int i = 0; i < (int)(sizeof(peaks) / sizeof(peaks[0])); ++i)
    {
        float cx = peaks[i].x + sweep * 5.0f;  // subtle parallax
        float h = peaks[i].h;
        float w = 95.0f;  // even wider mountains

        // Triangle for mountain - much darker for high contrast
        Vtx2D tri[3];
        tri[0].x = cx;       tri[0].y = baseY - h;         tri[0].z = 0; tri[0].rhw = 1; tri[0].c = ARGB(255, 35, 25, 55);  // peak - fully opaque
        tri[1].x = cx - w;   tri[1].y = baseY;             tri[1].z = 0; tri[1].rhw = 1; tri[1].c = ARGB(255, 25, 18, 45);  // left base
        tri[2].x = cx + w;   tri[2].y = baseY;             tri[2].z = 0; tri[2].rhw = 1; tri[2].c = ARGB(255, 25, 18, 45);  // right base

        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(Vtx2D));
    }

    End2D();
}

static void DrawSkylineAndReflection(DWORD tMs, float sweep)
{
    // Parallax: different layers move at different speeds
    float backSweep = sweep * 8.0f;   // background moves slower
    float midSweep = sweep * 14.0f;   // mid layer
    float frontSweep = sweep * 22.0f; // foreground moves fastest

    // Background layer - lighter, shorter buildings
    DrawSkylineLayer(s_bldgBack, sizeof(s_bldgBack) / sizeof(s_bldgBack[0]), backSweep, 80, 50, 200, 12, 10, 25);

    // Mid layer - medium
    DrawSkylineLayer(s_bldgMid, sizeof(s_bldgMid) / sizeof(s_bldgMid[0]), midSweep, 120, 70, 220, 6, 5, 15);

    // Foreground layer - darkest, tallest
    DrawSkylineLayer(s_bldgFront, sizeof(s_bldgFront) / sizeof(s_bldgFront[0]), frontSweep, 150, 90, 240, 2, 2, 8);

    // Mirror reflection: flip around horizon, darker + more transparent
    Begin2D(false);

    // Reflect all layers (simpler/combined reflection)
    const Bldg* allLayers[] = { s_bldgBack, s_bldgMid, s_bldgFront };
    int layerCounts[] = {
        sizeof(s_bldgBack) / sizeof(s_bldgBack[0]),
        sizeof(s_bldgMid) / sizeof(s_bldgMid[0]),
        sizeof(s_bldgFront) / sizeof(s_bldgFront[0])
    };
    float sweeps[] = { backSweep, midSweep, frontSweep };

    for (int layer = 0; layer < 3; ++layer)
    {
        for (int i = 0; i < layerCounts[layer]; ++i)
        {
            float x0 = allLayers[layer][i].x0 + sweeps[layer];
            float x1 = allLayers[layer][i].x1 + sweeps[layer];

            float h = allLayers[layer][i].h;
            float yTop = HORIZON_Y;
            float yBot = HORIZON_Y + h * 0.70f;

            DWORD c0 = ARGB(70, 8, 4, 16);
            DWORD c1 = ARGB(0, 8, 4, 16);

            DrawQuad(x0, yTop, x1, yBot, c0, c1);
        }
    }
    End2D();

    // Reflection tint band (magenta water glow)
    Begin2D(false);
    {
        Vtx2D band[4];
        band[0] = { 0.0f,     HORIZON_Y,       0, 1, ARGB(75, 255, 40, 200) };
        band[1] = { SCREEN_W, HORIZON_Y,       0, 1, ARGB(75, 255, 40, 200) };
        band[2] = { 0.0f,     HORIZON_Y + 120, 0, 1, ARGB(0,  255, 40, 200) };
        band[3] = { SCREEN_W, HORIZON_Y + 120, 0, 1, ARGB(0,  255, 40, 200) };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, band, sizeof(Vtx2D));
    }
    End2D();

    // Rooftop blinking red beacons (only on foreground buildings)
    Begin2D(true);
    {
        unsigned tick = (tMs / 140u);
        for (int i = 0; i < (int)(sizeof(s_bldgFront) / sizeof(s_bldgFront[0])); ++i)
        {
            if (!s_bldgFront[i].beacon) continue;

            unsigned on = (tick + (unsigned)i * 3u) & 1u;
            if (!on) continue;

            float x = (s_bldgFront[i].x0 + s_bldgFront[i].x1) * 0.5f + frontSweep;
            float y = HORIZON_Y - s_bldgFront[i].h - 4.0f;

            Vtx2D q[4];
            q[0] = { x - 1.5f, y - 1.5f, 0, 1, ARGB(220, 255, 40, 40) };
            q[1] = { x + 1.5f, y - 1.5f, 0, 1, ARGB(220, 255, 40, 40) };
            q[2] = { x - 1.5f, y + 1.5f, 0, 1, ARGB(0,   255, 40, 40) };
            q[3] = { x + 1.5f, y + 1.5f, 0, 1, ARGB(0,   255, 40, 40) };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx2D));
        }
    }
    End2D();
}

// ------------------------------------------------------------
// Grid (center VP) + reflection fade (NO float->int casts)
// ------------------------------------------------------------

static void DrawGridAndWater(DWORD tMs, float sweep)
{
    if (!g_pDevice) return;

    // Vanishing point centered like reference, tiny sweep
    float apexX = (SCREEN_W * 0.50f) + sweep * 16.0f;
    float apexY = HORIZON_Y;

    float botY = WATER_BOTTOM_Y;

    // Vertical perspective lines - BRIGHTER to match concept
    Begin2D(true);

    static Vtx2D v[4];
    const int vcount = 19;
    const int mid = (vcount - 1) / 2; // 9

    for (int i = 0; i < vcount; ++i)
    {
        float t = (float)i / (float)(vcount - 1); // 0..1
        float side = (t - 0.5f) * 2.0f;           // -1..+1

        float topX = apexX + side * (SCREEN_W * 0.30f);
        float botX = apexX + side * (SCREEN_W * 1.10f);

        int dist = i - mid; if (dist < 0) dist = -dist;
        int aInt = 110 - dist * 5; if (aInt < 20) aInt = 20;  // brighter grid
        BYTE a = (BYTE)aInt;

        DWORD c = ARGB(a, 255, 50, 210);  // brighter magenta

        v[0] = { topX - 0.8f, apexY, 0, 1, c };  // slightly thicker lines
        v[1] = { topX + 0.8f, apexY, 0, 1, c };
        v[2] = { botX - 2.0f, botY,  0, 1, ARGB(0,0,0,0) };
        v[3] = { botX + 2.0f, botY,  0, 1, ARGB(0,0,0,0) };

        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(Vtx2D));
    }

    // Horizontal grid lines scrolling down (gives motion) - BRIGHTER
    static Vtx2D h[4];
    const int hcount = 18;

    unsigned sc = (tMs / 12u) & 1023u;
    float scrollF = (float)sc * (1.0f / 1023.0f);

    for (int k = 0; k < hcount; ++k)
    {
        float tt = (float)k / (float)(hcount - 1);
        // Nonlinear spacing like reference
        float u = tt * tt;

        float y = apexY + 10.0f + u * 250.0f;
        y += scrollF * 18.0f;

        float halfW = (SCREEN_W * 0.30f) + (SCREEN_W * 1.08f - SCREEN_W * 0.30f) * u;

        int aInt = 100 - k * 4; if (aInt < 15) aInt = 15;  // brighter
        DWORD c = ARGB((BYTE)aInt, 255, 50, 210);  // brighter magenta

        h[0] = { apexX - halfW, y - 0.8f, 0, 1, c };  // slightly thicker
        h[1] = { apexX + halfW, y - 0.8f, 0, 1, c };
        h[2] = { apexX - halfW, y + 0.8f, 0, 1, ARGB(0,0,0,0) };
        h[3] = { apexX + halfW, y + 0.8f, 0, 1, ARGB(0,0,0,0) };

        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, h, sizeof(Vtx2D));
    }

    End2D();

    // Water darkening toward bottom
    Begin2D(false);
    {
        Vtx2D fade[4];
        fade[0] = { 0.0f,     apexY, 0, 1, ARGB(0,   0, 0, 0) };
        fade[1] = { SCREEN_W, apexY, 0, 1, ARGB(0,   0, 0, 0) };
        fade[2] = { 0.0f,     botY,  0, 1, ARGB(200, 0, 0, 0) };
        fade[3] = { SCREEN_W, botY,  0, 1, ARGB(200, 0, 0, 0) };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fade, sizeof(Vtx2D));
    }
    End2D();
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void CityScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();

    BuildLUT();
    BuildSunCircle();

    // ADDED: Load logo texture
    s_logoTex = LoadTextureFromDDS("D:\\tex\\tr.dds", s_logoW, s_logoH);
}

void CityScene_Shutdown()
{
    s_active = false;

    // ADDED: Release logo texture
    if (s_logoTex)
    {
        s_logoTex->Release();
        s_logoTex = NULL;
    }
}

bool CityScene_IsFinished()
{
    if (!s_active) return true;
    return (GetTickCount() - s_startTicks) >= SCENE_DURATION_MS;
}

void CityScene_Render(float)
{
    if (!s_active || !g_pDevice)
        return;

    DWORD tMs = GetTickCount() - s_startTicks;

    // Camera sweep (gentle) + parallax driver
    int idxA = (int)((tMs / 34u) & 1023u);
    float sweep = 0.55f * s_sin[idxA]; // -0.55..+0.55

    // 1) Sky + stars
    DrawSky(tMs);
    DrawStars();

    // 2) Sun behind skyline (MUCH higher and larger - barely touching building tops)
    float sunX = SCREEN_W * 0.50f + sweep * 10.0f;
    float sunY = HORIZON_Y - 150.0f;  // sun center WELL ABOVE horizon
    float sunR = 155.0f;  // larger sun

    DrawSunAndReflection(sunX, sunY, sunR, tMs);

    // ADDED: 2b) DDS logo on sun (if loaded)
    if (s_logoTex)
    {
        DrawLogoOnSun(sunX, sunY, 0.38f, tMs);  // smaller so it's actually visible
    }

    // 3) Mountain range (behind everything)
    DrawMountainRange(sweep);

    // 4) Skyline layers + reflection + beacons
    DrawSkylineAndReflection(tMs, sweep);

    // 5) Grid + water fade (center VP)
    DrawGridAndWater(tMs, sweep);
}