// GalaxyScene.cpp - Realistic spiral galaxy (RXDK-safe)
// Enhanced for more authentic galaxy appearance:
// - Bright yellow/white core bulge
// - Blue young star regions in spiral arms
// - Red/yellow older stars in inter-arm regions
// - Pink nebula clouds
// - Dark dust lanes cutting through arms
// - Central disc structure
// - Irregular density and structure
// - No float->int casts in Render

#include "GalaxyScene.h"
#include "font.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// -----------------------------------------------------------------------------
// Tunables
// -----------------------------------------------------------------------------

static const DWORD  SCENE_DURATION_MS = 25000;

static const float  SCREEN_W = 640.0f;
static const float  SCREEN_H = 480.0f;

static const int    LUT_N = 1024;


// -----------------------------------------------------------------------------
// On-screen stats (per-frame) - reflects what actually made it to the batch
// -----------------------------------------------------------------------------
struct LayerStats
{
    int total;
    int culled;
    int drawn;
};

static LayerStats s_statDust;
static LayerStats s_statDisc;
static LayerStats s_statSmall;
static LayerStats s_statNeb;
static LayerStats s_statLarge;

static __forceinline void Stats_Reset(LayerStats& s) { s.total = s.culled = s.drawn = 0; }

// -----------------------------------------------------------------------------
// Step fade-in near edges (NO float->int casts; comparisons only)
// scale256: 256=full, 192=75%, 128=50%, 64=25%, 0=skip
// -----------------------------------------------------------------------------
static __forceinline unsigned EdgeScale256_1D(float p, float minP, float maxP)
{
    const float FADE1 = 16.0f;
    const float FADE2 = 32.0f;
    const float FADE3 = 48.0f;

    if (p < minP) return 0u;
    if (p > maxP) return 0u;

    // left edge
    float dL = p - minP;
    if (dL < FADE3)
    {
        if (dL < FADE1) return 64u;
        if (dL < FADE2) return 128u;
        return 192u;
    }

    // right edge
    float dR = maxP - p;
    if (dR < FADE3)
    {
        if (dR < FADE1) return 64u;
        if (dR < FADE2) return 128u;
        return 192u;
    }

    return 256u;
}

static __forceinline unsigned MinU(unsigned a, unsigned b) { return (a < b) ? a : b; }

static __forceinline DWORD ApplyAlphaScale256(DWORD argb, unsigned scale256)
{
    unsigned a = (unsigned)(argb >> 24) & 255u;
    a = (a * scale256) >> 8;
    return (argb & 0x00FFFFFFu) | (a << 24);
}

// Increased particle counts for better density and arm visibility
static const int    STAR_SMALL_COUNT = 15000;
static const int    STAR_LARGE_COUNT = 1200;
static const int    DUST_COUNT = 675;
static const int    NEBULA_COUNT = 675;  // Increased for more visible emission regions
static const int    DISC_COUNT = 2500;  // NEW: central disc particles (reduced slightly to let arms show)

static const int    BATCH_QUADS = 512;

// Galaxy shape
static const int    ARMS = 4;
static const int    RMAX_PX = 420;
static const int    RCORE_PX = 20;   // Smaller, denser core
static const int    TWIST_MAX = 280;
static const int    SPREAD_MAX = 48;
static const float  ELLIPSE_Y = 0.78f;

// Camera sweep
static const float  SWEEP_X = 140.0f;
static const float  SWEEP_Y = 85.0f;
static const float  ZOOM_MIN = 0.78f;
static const float  ZOOM_MAX = 1.35f;
static const float  ROLL_MAX = 0.18f;

static const int    SPR_ROT_MAX = 64;

static const float  DUST_SIZE_MIN = 12.0f;
static const float  DUST_SIZE_MAX = 28.0f;

// Nebula size range (larger for visibility)
static const float  NEBULA_SIZE_MIN = 10.0f;
static const float  NEBULA_SIZE_MAX = 24.0f;

// NEW: Disc size range (larger for smooth continuous glow and visibility)
static const float  DISC_SIZE_MIN = 5.5f;
static const float  DISC_SIZE_MAX = 10.5f;

// -----------------------------------------------------------------------------
// String helpers
// -----------------------------------------------------------------------------

static void IntToStr(int val, char* buf, int bufSize)
{
    if (bufSize < 2) return;

    if (val == 0)
    {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    bool neg = (val < 0);
    if (neg) val = -val;

    char temp[32];
    int pos = 0;

    while (val > 0 && pos < 31)
    {
        temp[pos++] = (char)('0' + (val % 10));
        val /= 10;
    }

    int writePos = 0;
    if (neg && writePos < bufSize - 1)
        buf[writePos++] = '-';

    for (int i = pos - 1; i >= 0 && writePos < bufSize - 1; --i)
        buf[writePos++] = temp[i];

    buf[writePos] = '\0';
}

// -----------------------------------------------------------------------------
// LUT trig
// -----------------------------------------------------------------------------

static bool  s_tablesReady = false;
static float s_sin[LUT_N];
static float s_cos[LUT_N];

static void BuildTables()
{
    if (s_tablesReady) return;

    for (int i = 0; i < LUT_N; ++i)
    {
        float a = (float)i * (2.0f * 3.14159265358979323846f) / (float)LUT_N;
        s_sin[i] = sinf(a);
        s_cos[i] = cosf(a);
    }

    s_tablesReady = true;
}

// -----------------------------------------------------------------------------
// DDS loader
// -----------------------------------------------------------------------------

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

static LPDIRECT3DTEXTURE8 LoadDDS_A8R8G8B8_Swizzled(const char* path)
{
    if (!g_pDevice || !path)
        return NULL;

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
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

    if (w <= 0 || h <= 0)
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
    if (FAILED(g_pDevice->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, 0, &tex)))
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
        4);

    tex->UnlockRect(0);
    free(pixels);

    return tex;
}

// -----------------------------------------------------------------------------
// RNG
// -----------------------------------------------------------------------------

static unsigned s_rng = 0x12345678u;
static __forceinline unsigned RngU32()
{
    s_rng = (s_rng * 1664525u + 1013904223u);
    return s_rng;
}

static __forceinline int RngRangeI(int lo, int hi)
{
    if (hi <= lo) return lo;
    unsigned span = (unsigned)(hi - lo + 1);
    unsigned r = RngU32();
    return lo + (int)(r % (unsigned)span);
}

// -----------------------------------------------------------------------------
// Scene state + resources
// -----------------------------------------------------------------------------

static bool   s_active = false;
static DWORD  s_startTicks = 0;

static LPDIRECT3DTEXTURE8 s_texSprite = NULL;

// -----------------------------------------------------------------------------
// 2D batch
// -----------------------------------------------------------------------------

struct Vtx
{
    float x, y, z, rhw;
    DWORD c;
    float u, v;
};

#define FVF_2D_TEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static Vtx* s_batch = NULL;
static int  s_batchCapVerts = 0;
static int  s_batchCountVerts = 0;

static void EnsureBatch(int quads)
{
    int wantVerts = quads * 6;
    if (s_batch && s_batchCapVerts >= wantVerts)
        return;

    if (s_batch)
        free(s_batch);

    s_batch = (Vtx*)malloc(sizeof(Vtx) * wantVerts);
    s_batchCapVerts = s_batch ? wantVerts : 0;
    s_batchCountVerts = 0;
}

static void Batch_Begin()
{
    s_batchCountVerts = 0;
}

static void Batch_Flush()
{
    if (!g_pDevice || !s_batch || s_batchCountVerts <= 0)
        return;

    g_pDevice->SetVertexShader(FVF_2D_TEX);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, s_batchCountVerts / 3, s_batch, sizeof(Vtx));
    s_batchCountVerts = 0;
}

static void Batch_End()
{
    Batch_Flush();
}

static __forceinline void EmitQuad6(float cx, float cy, float half, DWORD col)
{
    if (s_batchCountVerts + 6 > s_batchCapVerts)
        Batch_Flush();

    Vtx* v = s_batch + s_batchCountVerts;

    float x0 = cx - half;
    float y0 = cy - half;
    float x1 = cx + half;
    float y1 = cy + half;

    v[0] = { x0, y0, 0.0f, 1.0f, col, 0.0f, 0.0f };
    v[1] = { x1, y0, 0.0f, 1.0f, col, 1.0f, 0.0f };
    v[2] = { x1, y1, 0.0f, 1.0f, col, 1.0f, 1.0f };

    v[3] = { x0, y0, 0.0f, 1.0f, col, 0.0f, 0.0f };
    v[4] = { x1, y1, 0.0f, 1.0f, col, 1.0f, 1.0f };
    v[5] = { x0, y1, 0.0f, 1.0f, col, 0.0f, 1.0f };

    s_batchCountVerts += 6;
}

// -----------------------------------------------------------------------------
// Render states
// -----------------------------------------------------------------------------

static void SetupSpriteStates(LPDIRECT3DTEXTURE8 tex)
{
    g_pDevice->SetTexture(0, tex);
    g_pDevice->SetVertexShader(FVF_2D_TEX);

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);

    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
}

static void DrawBackdrop()
{
    struct BV { float x, y, z, rhw; DWORD c; };
    BV q[4] =
    {
        { 0.0f,     0.0f,     0.0f, 1.0f, D3DCOLOR_XRGB(0,0,2) },
        { SCREEN_W, 0.0f,     0.0f, 1.0f, D3DCOLOR_XRGB(0,0,2) },
        { 0.0f,     SCREEN_H, 0.0f, 1.0f, D3DCOLOR_XRGB(0,0,5) },
        { SCREEN_W, SCREEN_H, 0.0f, 1.0f, D3DCOLOR_XRGB(0,0,5) },
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(BV));
}

static DWORD TwinkleColor(DWORD baseARGB, unsigned add)
{
    BYTE a = (BYTE)(baseARGB >> 24);
    BYTE r = (BYTE)(baseARGB >> 16);
    BYTE g = (BYTE)(baseARGB >> 8);
    BYTE b = (BYTE)(baseARGB);

    unsigned mul = 165u + add;

    unsigned rr = ((unsigned)r * mul) >> 8; if (rr > 255u) rr = 255u;
    unsigned gg = ((unsigned)g * mul) >> 8; if (gg > 255u) gg = 255u;
    unsigned bb = ((unsigned)b * mul) >> 8; if (bb > 255u) bb = 255u;

    return D3DCOLOR_ARGB(a, (BYTE)rr, (BYTE)gg, (BYTE)bb);
}

// -----------------------------------------------------------------------------
// Star data
// -----------------------------------------------------------------------------

struct Star
{
    int   rPix;
    int   ang;        // LUT angle base index
    int   depth;      // 0..255
    int   tw;         // 0..255
    int   armDist;    // distance from arm center (0..)
    int   sprRot;     // 0..SPR_ROT_MAX-1
    int   spinStep;   // 1..8
    float jx, jy;     // jitter in px
    DWORD base;       // base ARGB (integer, init-only)
};

static Star* s_small = NULL;
static Star* s_large = NULL;
static Star* s_dust = NULL;
static Star* s_nebula = NULL;
static Star* s_disc = NULL;

// -----------------------------------------------------------------------------
// Init-only distribution with realistic galaxy colors
// -----------------------------------------------------------------------------

static DWORD PickBaseColorInt(int rPix, int armDist, int isLarge)
{
    BYTE a = isLarge ? 160 : 115;
    BYTE r, g, b;

    // Core bulge: warm yellow-white (avoid white blowout)
    if (rPix < RCORE_PX)
    {
        r = 245; g = 235; b = 200;
        a = isLarge ? 120 : 95;
        return D3DCOLOR_ARGB(a, r, g, b);
    }

    // Spiral arms: blue-white (young hot stars) - BRIGHTER
    if (armDist < 18)
    {
        r = 210; g = 225; b = 255;
        a = isLarge ? 185 : 140;
    }
    // Inter-arm regions: yellow/white (older stars)
    else if (armDist > 30)
    {
        r = 245; g = 235; b = 200;
        a = isLarge ? 140 : 95;
    }
    // Transition zone
    else
    {
        r = 225; g = 230; b = 240;
        a = isLarge ? 155 : 110;
    }

    // Outer regions: dimmer and slightly bluer
    if (rPix > 220)
    {
        a = (BYTE)((unsigned)a * 140u >> 8);
        r = (BYTE)((unsigned)r * 170u >> 8);
        g = (BYTE)((unsigned)g * 175u >> 8);
        b = (BYTE)((unsigned)b * 190u >> 8);
    }

    return D3DCOLOR_ARGB(a, r, g, b);
}

static int BiasedRadiusInt(int maxR)
{
    unsigned u = (RngU32() & 0xFFFFu);
    unsigned long long uu = (unsigned long long)u * (unsigned long long)u;
    const unsigned long long denom = 4294836225ULL;
    unsigned long long scaled = uu * (unsigned long long)maxR;
    int r = (int)(scaled / denom);
    if (r < 0) r = 0;
    if (r > maxR) r = maxR;
    return r;
}

static void InitStars(Star* dst, int count, int isLarge)
{
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        // More even distribution but still concentrated outside core
        unsigned pick = (RngU32() & 255u);
        int rPix = 0;
        if (pick < 100u)
            rPix = BiasedRadiusInt(RCORE_PX);
        else
            rPix = RCORE_PX + BiasedRadiusInt(RMAX_PX - RCORE_PX);

        s.rPix = rPix;

        int arm = (int)(RngU32() % (unsigned)ARMS);
        int armBase = arm * (LUT_N / ARMS);

        int twist = (rPix * TWIST_MAX) / RMAX_PX;
        int spread = (RngRangeI(-SPREAD_MAX, SPREAD_MAX));
        int ang = (armBase + twist + spread) & (LUT_N - 1);
        s.ang = ang;

        // Arm distance metric (abs spread)
        int ad = spread; if (ad < 0) ad = -ad;
        s.armDist = ad;

        // jitter thickness (float only, no int conversion)
        float jx = (float)RngRangeI(-10, 10) * 0.55f;
        float jy = (float)RngRangeI(-10, 10) * 0.55f;
        s.jx = jx;
        s.jy = jy;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.spinStep = 1 + (int)(RngU32() & 7u);

        s.base = PickBaseColorInt(rPix, ad, isLarge);
    }
}

static void InitDust(Star* dst, int count)
{
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        // dust lanes in arms mid radius
        int rPix = RCORE_PX + BiasedRadiusInt(RMAX_PX - RCORE_PX);
        s.rPix = rPix;

        int arm = (int)(RngU32() % (unsigned)ARMS);
        int armBase = arm * (LUT_N / ARMS);

        int twist = (rPix * TWIST_MAX) / RMAX_PX;
        int spread = (RngRangeI(-24, 24));
        int ang = (armBase + twist + spread) & (LUT_N - 1);
        s.ang = ang;

        int ad = spread; if (ad < 0) ad = -ad;
        s.armDist = ad;

        s.jx = (float)RngRangeI(-16, 16) * 0.6f;
        s.jy = (float)RngRangeI(-16, 16) * 0.6f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.spinStep = 1 + (int)(RngU32() & 7u);

        // Very dark purple/blue dust; alpha low
        s.base = D3DCOLOR_ARGB(52, 10, 8, 14);
    }
}

static void InitNebula(Star* dst, int count)
{
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        // nebula in arms and slightly outer
        int rPix = RCORE_PX + BiasedRadiusInt(RMAX_PX - RCORE_PX);
        if (rPix < 120) rPix = 120 + (rPix % 40);
        s.rPix = rPix;

        int arm = (int)(RngU32() % (unsigned)ARMS);
        int armBase = arm * (LUT_N / ARMS);

        int twist = (rPix * TWIST_MAX) / RMAX_PX;
        int spread = (RngRangeI(-30, 30));
        int ang = (armBase + twist + spread) & (LUT_N - 1);
        s.ang = ang;

        int ad = spread; if (ad < 0) ad = -ad;
        s.armDist = ad;

        s.jx = (float)RngRangeI(-22, 22) * 0.7f;
        s.jy = (float)RngRangeI(-22, 22) * 0.7f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.spinStep = 1 + (int)(RngU32() & 7u);

        // Pink/purple emission regions
        BYTE a = 85;
        BYTE r = 255, g = 95, b = 205;

        // slight variation per armDist
        if (ad < 12) { r = 255; g = 120; b = 220; a = 95; }
        else if (ad > 24) { r = 185; g = 60; b = 255; a = 70; }

        s.base = D3DCOLOR_ARGB(a, r, g, b);
    }
}

static void InitDisc(Star* dst, int count)
{
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        // DISC: smooth distribution, more in inner region, not tied to arms
        int rPix = BiasedRadiusInt(RMAX_PX);
        s.rPix = rPix;

        int a = (int)(RngU32() & (LUT_N - 1));
        s.ang = a;

        s.armDist = 0;

        s.jx = (float)RngRangeI(-10, 10) * 0.9f;
        s.jy = (float)RngRangeI(-10, 10) * 0.9f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.spinStep = 1 + (int)(RngU32() & 7u);

        // Disc color: subtle warm white/yellow with radial falloff
        BYTE r = 235, g = 235, b = 220;
        BYTE a_val = 50;

        if (rPix < 30)
        {
            // Central glow: brighter but not blown out
            r = 250; g = 245; b = 230;
            a_val = 85;
        }
        else if (rPix < 180)
        {
            // Mid disc: dimmer to let arms show
            r = 230; g = 230; b = 215;
            a_val = 55;
        }
        else if (rPix < 260)
        {
            // Outer mid disc: warmer yellow-white
            r = 240; g = 230; b = 200;
            a_val = 60;
        }
        else
        {
            // Far outer disc: warm yellow falloff
            r = 235; g = 220; b = 190;
            a_val = 40;
        }

        s.base = D3DCOLOR_ARGB(a_val, r, g, b);
    }
}

// -----------------------------------------------------------------------------
// Camera sweep
// -----------------------------------------------------------------------------

struct Cam
{
    float cx, cy;
    float zoom;
    float roll;
};

static DWORD TimeMs()
{
    DWORD now = GetTickCount();
    return now - s_startTicks;
}

static Cam BuildCamera(DWORD tMs, DWORD durMs)
{
    float t = (durMs > 0) ? ((float)tMs / (float)durMs) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    float e = 0.5f - 0.5f * cosf(t * 3.14159265358979323846f);

    int phA = (int)((tMs / 16) & (LUT_N - 1));
    int phB = (int)(((tMs / 23) + 170) & (LUT_N - 1));

    float sa = s_sin[phA];
    float ca = s_cos[phA];
    float sb = s_sin[phB];

    Cam c;
    c.cx = (SCREEN_W * 0.5f) + (-SWEEP_X + (2.0f * SWEEP_X) * e) + sa * 18.0f;
    c.cy = (SCREEN_H * 0.5f) + (SWEEP_Y - (2.0f * SWEEP_Y) * e) + ca * 12.0f;

    c.zoom = ZOOM_MIN + (ZOOM_MAX - ZOOM_MIN) * e;
    c.roll = sb * ROLL_MAX;
    return c;
}

// -----------------------------------------------------------------------------
// Render star set
// -----------------------------------------------------------------------------

static void RenderStars(const Star* stars, int count, DWORD tMs, int isLarge,
    const Cam& cam, float cr, float sr, int rot,
    LayerStats& st)
{
    if (!stars || count <= 0 || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;
    int i = 0;
    while (i < count)
    {
        int quadsThis = 0;
        Vtx* out = s_batch;

        while (i < count && quadsThis < BATCH_QUADS)
        {
            const Star& s = stars[i++];

            int a = (s.ang + rot) & (LUT_N - 1);

            float cs = s_cos[a];
            float sn = s_sin[a];

            float gx = cs * (float)s.rPix + s.jx;
            float gy = sn * (float)s.rPix * ELLIPSE_Y + s.jy;

            float dz = 0.62f + ((float)s.depth) * (0.70f / 255.0f);
            float scale = cam.zoom * dz;

            float rx = gx * cr - gy * sr;
            float ry = gx * sr + gy * cr;

            float sx = cam.cx + rx * scale;
            float sy = cam.cy + ry * scale;

            st.total++;

            const float CULL_PAD = 32.0f;
            if (sx < -CULL_PAD || sx >(SCREEN_W + CULL_PAD) ||
                sy < -CULL_PAD || sy >(SCREEN_H + CULL_PAD))
            {
                st.culled++;
                continue;
            }

            unsigned sxScale = EdgeScale256_1D(sx, 0.0f, SCREEN_W);
            unsigned syScale = EdgeScale256_1D(sy, 0.0f, SCREEN_H);
            unsigned scale256 = MinU(sxScale, syScale);
            if (scale256 == 0u)
            {
                st.culled++;
                continue;
            }

            float size = isLarge ? 2.6f : 1.2f;

            if (s.rPix < 60) size *= isLarge ? 1.0f : 1.05f;
            else if (s.rPix > 280) size *= isLarge ? 0.82f : 0.90f;

            size *= (0.90f + (float)s.depth * (0.18f / 255.0f));

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 16) & 255u)) & 255);
            unsigned add = (tw >> 2); // 0..63

            DWORD col = TwinkleColor(s.base, add);
            col = ApplyAlphaScale256(col, scale256);
            if (((col >> 24) & 255u) < 6u) { st.culled++; continue; }


            out[0] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[1] = { sx + size, sy - size, 0.0f, 1.0f, col, 1.0f, 0.0f };
            out[2] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };

            out[3] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[4] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };
            out[5] = { sx - size, sy + size, 0.0f, 1.0f, col, 0.0f, 1.0f };

            out += 6;
            quadsThis++;
            st.drawn++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->SetVertexShader(FVF_2D_TEX);
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, quadsThis * 2, s_batch, sizeof(Vtx));
        }
    }
}

static void RenderDust(const Star* dust, int count, DWORD tMs,
    const Cam& cam, float cr, float sr, int rot,
    LayerStats& st)
{
    if (!dust || count <= 0 || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    int i = 0;
    while (i < count)
    {
        int quadsThis = 0;
        Vtx* out = s_batch;

        while (i < count && quadsThis < BATCH_QUADS)
        {
            const Star& s = dust[i++];

            int a = (s.ang + rot) & (LUT_N - 1);
            float cs = s_cos[a];
            float sn = s_sin[a];

            float gx = cs * (float)s.rPix + s.jx;
            float gy = sn * (float)s.rPix * ELLIPSE_Y + s.jy;

            float dz = 0.62f + ((float)s.depth) * (0.70f / 255.0f);
            float scale = cam.zoom * dz;

            float rx = gx * cr - gy * sr;
            float ry = gx * sr + gy * cr;

            float sx = cam.cx + rx * scale;
            float sy = cam.cy + ry * scale;

            st.total++;

            const float CULL_PAD = 80.0f;
            if (sx < -CULL_PAD || sx >(SCREEN_W + CULL_PAD) ||
                sy < -CULL_PAD || sy >(SCREEN_H + CULL_PAD))
            {
                st.culled++;
                continue;
            }

            unsigned sxScale = EdgeScale256_1D(sx, 0.0f, SCREEN_W);
            unsigned syScale = EdgeScale256_1D(sy, 0.0f, SCREEN_H);
            unsigned scale256 = MinU(sxScale, syScale);
            if (scale256 == 0u)
            {
                st.culled++;
                continue;
            }

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 48) & 255u)) & 255);
            unsigned add = (tw >> 3); // 0..31

            DWORD col = TwinkleColor(s.base, add);
            col = ApplyAlphaScale256(col, scale256);

            float k = (float)(s.depth & 31) * (1.0f / 31.0f);
            float size = DUST_SIZE_MIN + (DUST_SIZE_MAX - DUST_SIZE_MIN) * k;

            out[0] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[1] = { sx + size, sy - size, 0.0f, 1.0f, col, 1.0f, 0.0f };
            out[2] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };

            out[3] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[4] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };
            out[5] = { sx - size, sy + size, 0.0f, 1.0f, col, 0.0f, 1.0f };

            out += 6;
            quadsThis++;
            st.drawn++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->SetVertexShader(FVF_2D_TEX);
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, quadsThis * 2, s_batch, sizeof(Vtx));
        }
    }
}

static void RenderNebula(const Star* nebula, int count, DWORD tMs,
    const Cam& cam, float cr, float sr, int rot,
    LayerStats& st)
{
    if (!nebula || count <= 0 || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    int i = 0;
    while (i < count)
    {
        int quadsThis = 0;
        Vtx* out = s_batch;

        while (i < count && quadsThis < BATCH_QUADS)
        {
            const Star& s = nebula[i++];

            int a = (s.ang + rot) & (LUT_N - 1);
            float cs = s_cos[a];
            float sn = s_sin[a];

            float gx = cs * (float)s.rPix + s.jx;
            float gy = sn * (float)s.rPix * ELLIPSE_Y + s.jy;

            float dz = 0.62f + ((float)s.depth) * (0.70f / 255.0f);
            float scale = cam.zoom * dz;

            float rx = gx * cr - gy * sr;
            float ry = gx * sr + gy * cr;

            float sx = cam.cx + rx * scale;
            float sy = cam.cy + ry * scale;

            st.total++;

            const float CULL_PAD = 60.0f;
            if (sx < -CULL_PAD || sx >(SCREEN_W + CULL_PAD) ||
                sy < -CULL_PAD || sy >(SCREEN_H + CULL_PAD))
            {
                st.culled++;
                continue;
            }

            unsigned sxScale = EdgeScale256_1D(sx, 0.0f, SCREEN_W);
            unsigned syScale = EdgeScale256_1D(sy, 0.0f, SCREEN_H);
            unsigned scale256 = MinU(sxScale, syScale);
            if (scale256 == 0u)
            {
                st.culled++;
                continue;
            }

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 35) & 255u)) & 255);
            unsigned add = (tw >> 3); // 0..31

            DWORD col = TwinkleColor(s.base, add);
            col = ApplyAlphaScale256(col, scale256);

            float k = (float)(s.depth & 31) * (1.0f / 31.0f);
            float size = NEBULA_SIZE_MIN + (NEBULA_SIZE_MAX - NEBULA_SIZE_MIN) * k;

            out[0] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[1] = { sx + size, sy - size, 0.0f, 1.0f, col, 1.0f, 0.0f };
            out[2] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };

            out[3] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[4] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };
            out[5] = { sx - size, sy + size, 0.0f, 1.0f, col, 0.0f, 1.0f };

            out += 6;
            quadsThis++;
            st.drawn++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->SetVertexShader(FVF_2D_TEX);
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, quadsThis * 2, s_batch, sizeof(Vtx));
        }
    }
}

static void RenderDisc(const Star* disc, int count, DWORD tMs,
    const Cam& cam, float cr, float sr, int rot,
    LayerStats& st)
{
    if (!disc || count <= 0 || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    int i = 0;
    while (i < count)
    {
        int quadsThis = 0;
        Vtx* out = s_batch;

        while (i < count && quadsThis < BATCH_QUADS)
        {
            const Star& s = disc[i++];

            int a = (s.ang + rot) & (LUT_N - 1);
            float cs = s_cos[a];
            float sn = s_sin[a];

            float gx = cs * (float)s.rPix + s.jx;
            float gy = sn * (float)s.rPix * ELLIPSE_Y + s.jy;

            float dz = 0.62f + ((float)s.depth) * (0.70f / 255.0f);
            float scale = cam.zoom * dz;

            float rx = gx * cr - gy * sr;
            float ry = gx * sr + gy * cr;

            float sx = cam.cx + rx * scale;
            float sy = cam.cy + ry * scale;

            st.total++;

            const float CULL_PAD = 40.0f;
            if (sx < -CULL_PAD || sx >(SCREEN_W + CULL_PAD) ||
                sy < -CULL_PAD || sy >(SCREEN_H + CULL_PAD))
            {
                st.culled++;
                continue;
            }

            unsigned sxScale = EdgeScale256_1D(sx, 0.0f, SCREEN_W);
            unsigned syScale = EdgeScale256_1D(sy, 0.0f, SCREEN_H);
            unsigned scale256 = MinU(sxScale, syScale);
            if (scale256 == 0u)
            {
                st.culled++;
                continue;
            }

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 40) & 255u)) & 255);
            unsigned add = (tw >> 3); // 0..31

            DWORD col = TwinkleColor(s.base, add);
            col = ApplyAlphaScale256(col, scale256);

            float k = (float)(s.depth & 31) * (1.0f / 31.0f);
            float size = DISC_SIZE_MIN + (DISC_SIZE_MAX - DISC_SIZE_MIN) * k;

            out[0] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[1] = { sx + size, sy - size, 0.0f, 1.0f, col, 1.0f, 0.0f };
            out[2] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };

            out[3] = { sx - size, sy - size, 0.0f, 1.0f, col, 0.0f, 0.0f };
            out[4] = { sx + size, sy + size, 0.0f, 1.0f, col, 1.0f, 1.0f };
            out[5] = { sx - size, sy + size, 0.0f, 1.0f, col, 0.0f, 1.0f };

            out += 6;
            quadsThis++;
            st.drawn++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->SetVertexShader(FVF_2D_TEX);
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, quadsThis * 2, s_batch, sizeof(Vtx));
        }
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void GalaxyScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();

    BuildTables();

    if (s_texSprite) { s_texSprite->Release(); s_texSprite = NULL; }
    s_texSprite = LoadDDS_A8R8G8B8_Swizzled("D:\\tex\\cloud_256.dds");

    if (s_small) { free(s_small); s_small = NULL; }
    if (s_large) { free(s_large); s_large = NULL; }
    if (s_dust) { free(s_dust);  s_dust = NULL; }
    if (s_nebula) { free(s_nebula); s_nebula = NULL; }
    if (s_disc) { free(s_disc); s_disc = NULL; }

    s_small = (Star*)malloc(sizeof(Star) * STAR_SMALL_COUNT);
    s_large = (Star*)malloc(sizeof(Star) * STAR_LARGE_COUNT);
    s_dust = (Star*)malloc(sizeof(Star) * DUST_COUNT);
    s_nebula = (Star*)malloc(sizeof(Star) * NEBULA_COUNT);
    s_disc = (Star*)malloc(sizeof(Star) * DISC_COUNT);

    EnsureBatch(BATCH_QUADS);

    s_rng = 0xC0FFEE11u ^ GetTickCount();

    if (s_small) InitStars(s_small, STAR_SMALL_COUNT, 0);
    if (s_large) InitStars(s_large, STAR_LARGE_COUNT, 1);
    if (s_dust)  InitDust(s_dust, DUST_COUNT);
    if (s_nebula) InitNebula(s_nebula, NEBULA_COUNT);
    if (s_disc) InitDisc(s_disc, DISC_COUNT);
}

void GalaxyScene_Shutdown()
{
    s_active = false;

    if (s_texSprite) { s_texSprite->Release(); s_texSprite = NULL; }

    if (s_small) { free(s_small); s_small = NULL; }
    if (s_large) { free(s_large); s_large = NULL; }
    if (s_dust) { free(s_dust);  s_dust = NULL; }
    if (s_nebula) { free(s_nebula); s_nebula = NULL; }
    if (s_disc) { free(s_disc); s_disc = NULL; }

    if (s_batch) { free(s_batch); s_batch = NULL; s_batchCapVerts = 0; }
}

bool GalaxyScene_IsFinished()
{
    if (!s_active) return true;
    return (TimeMs() >= SCENE_DURATION_MS);
}

void GalaxyScene_Render(float)
{
    if (!s_active || !g_pDevice)
        return;

    DWORD tMs = TimeMs();
    if (tMs > SCENE_DURATION_MS) tMs = SCENE_DURATION_MS;

    DrawBackdrop();

    if (!s_texSprite || !s_small || !s_large || !s_dust || !s_nebula || !s_disc || !s_batch)
        return;

    // Per-frame stats
    Stats_Reset(s_statDust);
    Stats_Reset(s_statDisc);
    Stats_Reset(s_statSmall);
    Stats_Reset(s_statNeb);
    Stats_Reset(s_statLarge);

    Cam cam = BuildCamera(tMs, SCENE_DURATION_MS);
    float cr = cosf(cam.roll);
    float sr = sinf(cam.roll);

    int rotStars = (int)((tMs / 19) & (LUT_N - 1));
    int rotDust = (int)((tMs / 31) & (LUT_N - 1));
    int rotNeb = (int)((tMs / 25) & (LUT_N - 1));
    int rotDisc = (int)((tMs / 22) & (LUT_N - 1));

    // One-time state bind
    SetupSpriteStates(s_texSprite);

    // Layer order: dust -> disc -> small stars -> nebula -> large stars
    RenderDust(s_dust, DUST_COUNT, tMs, cam, cr, sr, rotDust, s_statDust);
    RenderDisc(s_disc, DISC_COUNT, tMs, cam, cr, sr, rotDisc, s_statDisc);
    RenderStars(s_small, STAR_SMALL_COUNT, tMs, 0, cam, cr, sr, rotStars, s_statSmall);
    RenderNebula(s_nebula, NEBULA_COUNT, tMs, cam, cr, sr, rotNeb, s_statNeb);
    RenderStars(s_large, STAR_LARGE_COUNT, tMs, 1, cam, cr, sr, rotStars, s_statLarge);

    // Stats overlay (drawn counts reflect on-screen workload)
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    char buf[64];

    IntToStr(s_statSmall.drawn + s_statLarge.drawn, buf, sizeof(buf));
    DrawText(10.0f, 10.0f, "STARS ON-SCREEN: ", 2.0f, D3DCOLOR_XRGB(200, 220, 255));
    DrawText(250.0f, 10.0f, buf, 2.0f, D3DCOLOR_XRGB(200, 220, 255));

    IntToStr(s_statNeb.drawn, buf, sizeof(buf));
    DrawText(10.0f, 30.0f, "NEBULAE ON-SCREEN: ", 2.0f, D3DCOLOR_XRGB(255, 140, 200));
    DrawText(280.0f, 30.0f, buf, 2.0f, D3DCOLOR_XRGB(255, 140, 200));

    IntToStr(s_statDust.drawn, buf, sizeof(buf));
    DrawText(10.0f, 50.0f, "DUST ON-SCREEN: ", 2.0f, D3DCOLOR_XRGB(180, 170, 160));
    DrawText(230.0f, 50.0f, buf, 2.0f, D3DCOLOR_XRGB(180, 170, 160));
}
