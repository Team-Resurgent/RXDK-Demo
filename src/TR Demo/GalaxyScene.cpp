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

    if ((w & (w - 1)) != 0 || (h & (h - 1)) != 0)
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
        (UINT)w, (UINT)h, 1,
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
        4);

    tex->UnlockRect(0);
    free(pixels);

    return tex;
}

// -----------------------------------------------------------------------------
// Deterministic PRNG (Init-only)
// -----------------------------------------------------------------------------

static unsigned s_rng = 0xC0FFEE11u;

static unsigned RngU32()
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static int RngRangeI(int lo, int hi)
{
    unsigned r = RngU32();
    int span = (hi - lo) + 1;
    if (span <= 1) return lo;
    return lo + (int)(r % (unsigned)span);
}

// -----------------------------------------------------------------------------
// Scene state
// -----------------------------------------------------------------------------

static bool   s_active = false;
static DWORD  s_startTicks = 0;

static LPDIRECT3DTEXTURE8 s_texSprite = NULL;

struct Vtx
{
    float x, y, z, rhw;
    DWORD c;
    float u, v;
};
#define FVF_2D_TEX (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static Vtx* s_batch = NULL;
static int  s_batchCapVerts = 0;

struct Star
{
    int   ang;
    int   rPix;
    float jx, jy;
    int   depth;
    int   tw;
    int   sprRot;
    float ax, ay;
    int   arm;
    int   spinDir;
    int   spinStep;
    DWORD base;
    int   type;  // 0=core, 1=arm, 2=inter-arm, 3=outer
};

static Star* s_small = NULL;
static Star* s_large = NULL;
static Star* s_dust = NULL;
static Star* s_nebula = NULL;
static Star* s_disc = NULL;  // NEW: central disc

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static DWORD TimeMs()
{
    DWORD now = GetTickCount();
    return now - s_startTicks;
}

static void EnsureBatch(int quads)
{
    const int needVerts = quads * 6;
    if (s_batch && s_batchCapVerts >= needVerts)
        return;

    if (s_batch) { free(s_batch); s_batch = NULL; s_batchCapVerts = 0; }

    s_batch = (Vtx*)malloc(sizeof(Vtx) * needVerts);
    if (s_batch)
        s_batchCapVerts = needVerts;
}

static void EmitQuad6(Vtx* dst, float x, float y, float sx, float sy, float rotCs, float rotSn, DWORD c)
{
    float x0 = (-sx * rotCs) - (-sy * rotSn);
    float y0 = (-sx * rotSn) + (-sy * rotCs);

    float x1 = (sx * rotCs) - (-sy * rotSn);
    float y1 = (sx * rotSn) + (-sy * rotCs);

    float x2 = (-sx * rotCs) - (sy * rotSn);
    float y2 = (-sx * rotSn) + (sy * rotCs);

    float x3 = (sx * rotCs) - (sy * rotSn);
    float y3 = (sx * rotSn) + (sy * rotCs);

    dst[0] = { x + x0, y + y0, 0.0f, 1.0f, c, 0.0f, 0.0f };
    dst[1] = { x + x1, y + y1, 0.0f, 1.0f, c, 1.0f, 0.0f };
    dst[2] = { x + x2, y + y2, 0.0f, 1.0f, c, 0.0f, 1.0f };

    dst[3] = { x + x2, y + y2, 0.0f, 1.0f, c, 0.0f, 1.0f };
    dst[4] = { x + x1, y + y1, 0.0f, 1.0f, c, 1.0f, 0.0f };
    dst[5] = { x + x3, y + y3, 0.0f, 1.0f, c, 1.0f, 1.0f };
}

static void SetupSpriteStates(LPDIRECT3DTEXTURE8 tex)
{
    g_pDevice->SetTexture(0, tex);
    g_pDevice->SetVertexShader(FVF_2D_TEX);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

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

        // Tighter, more consistent spread for defined arms
        int spreadRange = SPREAD_MAX + RngRangeI(-5, 5);
        int spread = RngRangeI(-spreadRange, spreadRange);

        int twist = (rPix * TWIST_MAX) / RMAX_PX;
        if (arm & 1) twist = -twist;

        int a = armBase + spread + twist;
        a &= (LUT_N - 1);
        s.ang = a;

        s.arm = arm;

        // Store distance from arm center for color determination
        int armDist = spread;
        if (armDist < 0) armDist = -armDist;

        int jx = RngRangeI(-100, 100);
        int jy = RngRangeI(-100, 100);

        s.jx = (float)jx * 0.10f;
        s.jy = (float)jy * 0.10f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));

        s.spinDir = (RngU32() & 1u) ? 1 : -1;
        s.spinStep = 1 + (int)(RngU32() & 7u);
        if (s.spinStep > 7) s.spinStep = 7;

        int ax = RngRangeI(-35, 35);
        int ay = RngRangeI(-35, 35);
        s.ax = 1.0f + (float)ax * 0.004f;
        s.ay = 1.0f + (float)ay * 0.004f;

        s.base = PickBaseColorInt(rPix, armDist, isLarge);
    }
}

static void InitDust(Star* dst, int count)
{
    // Dark dust lanes
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        int rPix = BiasedRadiusInt(RMAX_PX);
        s.rPix = rPix;

        int arm = (int)(RngU32() % (unsigned)ARMS);
        int armBase = arm * (LUT_N / ARMS);

        int spread = RngRangeI(-SPREAD_MAX, SPREAD_MAX);  // Tighter dust lanes
        int twist = (rPix * (TWIST_MAX + 40)) / RMAX_PX;

        int a = armBase + spread + twist;
        a &= (LUT_N - 1);
        s.ang = a;

        int jx = RngRangeI(-150, 150);
        int jy = RngRangeI(-150, 150);
        s.jx = (float)jx * 0.12f;
        s.jy = (float)jy * 0.12f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.ax = 1.0f;
        s.ay = 1.0f;

        s.spinDir = (RngU32() & 1u) ? 1 : -1;
        s.spinStep = 1 + (int)(RngU32() & 7u);
        if (s.spinStep > 7) s.spinStep = 7;

        // Dark brown/gray dust
        s.base = D3DCOLOR_ARGB(32, 40, 35, 30);
    }
}

static void InitNebula(Star* dst, int count)
{
    // Pink/red nebula regions
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        // Nebulae mostly in arms
        int rPix = RCORE_PX + BiasedRadiusInt(RMAX_PX - RCORE_PX);
        s.rPix = rPix;

        int arm = (int)(RngU32() % (unsigned)ARMS);
        int armBase = arm * (LUT_N / ARMS);

        int spread = RngRangeI(-SPREAD_MAX + 8, SPREAD_MAX - 8);  // Stay in arm centers
        int twist = (rPix * TWIST_MAX) / RMAX_PX;

        int a = armBase + spread + twist;
        a &= (LUT_N - 1);
        s.ang = a;

        int jx = RngRangeI(-80, 80);
        int jy = RngRangeI(-80, 80);
        s.jx = (float)jx * 0.08f;
        s.jy = (float)jy * 0.08f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.ax = 1.0f;
        s.ay = 1.0f;

        s.spinDir = (RngU32() & 1u) ? 1 : -1;
        s.spinStep = 1 + (int)(RngU32() & 7u);

        // Pink/red nebula (brighter and more saturated)
        s.base = D3DCOLOR_ARGB(52, 235, 120, 165);
    }
}

static void InitDisc(Star* dst, int count)
{
    // Central disc - extends from core to mid-radius
    // More concentrated than spiral arms, less than core bulge
    for (int i = 0; i < count; ++i)
    {
        Star& s = dst[i];

        s.depth = (int)(RngU32() & 255u);
        s.tw = (int)(RngU32() & 255u);

        // Radius distribution: avoid mid-range where arms are strongest
        unsigned pick = (RngU32() & 255u);
        int rPix = 0;
        if (pick < 180u)
            rPix = RCORE_PX + BiasedRadiusInt(70);  // Inner disc
        else if (pick < 220u)
            rPix = 220 + BiasedRadiusInt(80);  // Skip mid, go to outer
        else
            rPix = 120 + BiasedRadiusInt(60);  // Fill gaps

        s.rPix = rPix;

        // Even angular distribution (no arm bias)
        int a = (int)(RngU32() & (LUT_N - 1));
        s.ang = a;

        // Moderate jitter - not too much, we want smooth blending
        int jx = RngRangeI(-50, 50);
        int jy = RngRangeI(-50, 50);
        s.jx = (float)jx * 0.10f;
        s.jy = (float)jy * 0.10f;

        s.sprRot = (int)(RngU32() & (SPR_ROT_MAX - 1));
        s.ax = 1.0f;
        s.ay = 1.0f;

        s.spinDir = (RngU32() & 1u) ? 1 : -1;
        s.spinStep = 1 + (int)(RngU32() & 7u);

        // Disc color: blue-white center, dimmer mid for arm contrast
        BYTE a_val = 65;
        BYTE r, g, b;

        if (rPix < 100)
        {
            // Inner disc: cool blue-white (matches core better)
            r = 235; g = 240; b = 250;
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

static void RenderStars(const Star* stars, int count, LPDIRECT3DTEXTURE8 tex, DWORD tMs, int isLarge)
{
    if (!stars || count <= 0 || !tex || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    Cam cam = BuildCamera(tMs, SCENE_DURATION_MS);

    float cr = cosf(cam.roll);
    float sr = sinf(cam.roll);

    int rot = (int)((tMs / 19) & (LUT_N - 1));

    SetupSpriteStates(tex);

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

            if (sx < -32.0f || sx >(SCREEN_W + 32.0f) ||
                sy < -32.0f || sy >(SCREEN_H + 32.0f))
                continue;

            // Vary size by region - smaller core to reduce blowout
            float size;
            if (s.rPix < RCORE_PX)
                size = isLarge ? 3.2f : 2.2f;  // Reduced core size
            else if (s.rPix < 140)
                size = isLarge ? 2.8f : 1.8f;
            else if (s.rPix < 240)
                size = isLarge ? 2.2f : 1.4f;
            else
                size = isLarge ? 1.8f : 1.1f;

            size *= (0.90f + (float)s.depth * (0.18f / 255.0f));

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 16) & 255u)) & 255);
            unsigned coreAdd = 0;

            // Minimal core brightness (prevent white blowout)
            if (s.rPix < RCORE_PX) coreAdd = 15;
            else if (s.rPix < 100) coreAdd = 8;
            else if (s.rPix < 180) coreAdd = 4;

            unsigned add = tw + coreAdd;
            if (add > 255u) add = 255u;

            DWORD col = TwinkleColor(s.base, add);

            int spin = (int)((tMs / (31u + (unsigned)s.spinStep * 9u)) & 1023u);
            if (s.spinDir < 0) spin = -spin;

            int sprIdx = (s.sprRot << 4) + (spin & 1023);
            sprIdx &= (LUT_N - 1);

            float sprCs = s_cos[sprIdx];
            float sprSn = s_sin[sprIdx];

            float sxh = size * s.ax;
            float syh = size * s.ay;

            EmitQuad6(out, sx, sy, sxh, syh, sprCs, sprSn, col);
            out += 6;
            quadsThis++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST,
                (quadsThis * 6) / 3,
                s_batch,
                sizeof(Vtx));
        }
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void RenderDust(const Star* dust, int count, LPDIRECT3DTEXTURE8 tex, DWORD tMs)
{
    if (!dust || count <= 0 || !tex || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    Cam cam = BuildCamera(tMs, SCENE_DURATION_MS);

    float cr = cosf(cam.roll);
    float sr = sinf(cam.roll);

    int rot = (int)((tMs / 31) & (LUT_N - 1));

    SetupSpriteStates(tex);

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

            if (sx < -80.0f || sx >(SCREEN_W + 80.0f) ||
                sy < -80.0f || sy >(SCREEN_H + 80.0f))
                continue;

            float k = (float)(s.depth & 31) * (1.0f / 31.0f);
            float size = DUST_SIZE_MIN + (DUST_SIZE_MAX - DUST_SIZE_MIN) * k;

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 48) & 255u)) & 255);
            unsigned add = (tw >> 3);

            DWORD col = TwinkleColor(s.base, add);

            int sprA = (s.sprRot + ((tMs / 59) & (SPR_ROT_MAX - 1))) & (SPR_ROT_MAX - 1);
            int sprIdx = (sprA << 4) & (LUT_N - 1);

            float sprCs = s_cos[sprIdx];
            float sprSn = s_sin[sprIdx];

            EmitQuad6(out, sx, sy, size, size, sprCs, sprSn, col);
            out += 6;
            quadsThis++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST,
                (quadsThis * 6) / 3,
                s_batch,
                sizeof(Vtx));
        }
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void RenderNebula(const Star* nebula, int count, LPDIRECT3DTEXTURE8 tex, DWORD tMs)
{
    if (!nebula || count <= 0 || !tex || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    Cam cam = BuildCamera(tMs, SCENE_DURATION_MS);

    float cr = cosf(cam.roll);
    float sr = sinf(cam.roll);

    int rot = (int)((tMs / 25) & (LUT_N - 1));

    SetupSpriteStates(tex);

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

            if (sx < -60.0f || sx >(SCREEN_W + 60.0f) ||
                sy < -60.0f || sy >(SCREEN_H + 60.0f))
                continue;

            float k = (float)(s.depth & 31) * (1.0f / 31.0f);
            float size = NEBULA_SIZE_MIN + (NEBULA_SIZE_MAX - NEBULA_SIZE_MIN) * k;

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 35) & 255u)) & 255);
            unsigned add = (tw >> 2);

            DWORD col = TwinkleColor(s.base, add);

            int sprIdx = (s.sprRot << 3) & (LUT_N - 1);

            float sprCs = s_cos[sprIdx];
            float sprSn = s_sin[sprIdx];

            EmitQuad6(out, sx, sy, size, size, sprCs, sprSn, col);
            out += 6;
            quadsThis++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST,
                (quadsThis * 6) / 3,
                s_batch,
                sizeof(Vtx));
        }
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void RenderDisc(const Star* disc, int count, LPDIRECT3DTEXTURE8 tex, DWORD tMs)
{
    if (!disc || count <= 0 || !tex || !s_batch || s_batchCapVerts < (BATCH_QUADS * 6))
        return;

    Cam cam = BuildCamera(tMs, SCENE_DURATION_MS);

    float cr = cosf(cam.roll);
    float sr = sinf(cam.roll);

    int rot = (int)((tMs / 22) & (LUT_N - 1));

    SetupSpriteStates(tex);

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

            if (sx < -40.0f || sx >(SCREEN_W + 40.0f) ||
                sy < -40.0f || sy >(SCREEN_H + 40.0f))
                continue;

            float k = (float)(s.depth & 31) * (1.0f / 31.0f);
            float size = DISC_SIZE_MIN + (DISC_SIZE_MAX - DISC_SIZE_MIN) * k;

            unsigned tw = (unsigned)((s.tw + (int)((tMs / 40) & 255u)) & 255);
            unsigned add = (tw >> 3);  // Very subtle twinkle for smooth disc

            DWORD col = TwinkleColor(s.base, add);

            int sprIdx = (s.sprRot << 3) & (LUT_N - 1);

            float sprCs = s_cos[sprIdx];
            float sprSn = s_sin[sprIdx];

            EmitQuad6(out, sx, sy, size, size, sprCs, sprSn, col);
            out += 6;
            quadsThis++;
        }

        if (quadsThis > 0)
        {
            g_pDevice->DrawPrimitiveUP(
                D3DPT_TRIANGLELIST,
                (quadsThis * 6) / 3,
                s_batch,
                sizeof(Vtx));
        }
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
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

    // Layer order for realistic look:
    // 1) Dark dust lanes (obscures background)
    // 2) Central disc (smooth background glow)
    // 3) Small stars (dense background)
    // 4) Pink nebulae (emission regions)
    // 5) Large stars (bright foreground pops)
    RenderDust(s_dust, DUST_COUNT, s_texSprite, tMs);
    RenderDisc(s_disc, DISC_COUNT, s_texSprite, tMs);
    RenderStars(s_small, STAR_SMALL_COUNT, s_texSprite, tMs, 0);
    RenderNebula(s_nebula, NEBULA_COUNT, s_texSprite, tMs);
    RenderStars(s_large, STAR_LARGE_COUNT, s_texSprite, tMs, 1);

    // Stats overlay
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    char buf[64];

    IntToStr(STAR_SMALL_COUNT + STAR_LARGE_COUNT, buf, sizeof(buf));
    DrawText(10.0f, 10.0f, "STARS: ", 2.0f, D3DCOLOR_XRGB(200, 220, 255));
    DrawText(120.0f, 10.0f, buf, 2.0f, D3DCOLOR_XRGB(200, 220, 255));

    IntToStr(NEBULA_COUNT, buf, sizeof(buf));
    DrawText(10.0f, 30.0f, "NEBULAE: ", 2.0f, D3DCOLOR_XRGB(255, 140, 200));
    DrawText(160.0f, 30.0f, buf, 2.0f, D3DCOLOR_XRGB(255, 140, 200));

    IntToStr(DUST_COUNT, buf, sizeof(buf));
    DrawText(10.0f, 50.0f, "DUST: ", 2.0f, D3DCOLOR_XRGB(180, 170, 160));
    DrawText(110.0f, 50.0f, buf, 2.0f, D3DCOLOR_XRGB(180, 170, 160));

    IntToStr((int)(tMs / 1000), buf, sizeof(buf));
    DrawText(10.0f, 70.0f, "TIME: ", 2.0f, D3DCOLOR_XRGB(140, 210, 255));
    DrawText(110.0f, 70.0f, buf, 2.0f, D3DCOLOR_XRGB(140, 210, 255));
}