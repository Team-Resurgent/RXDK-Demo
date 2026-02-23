// PostFXScene.cpp — Black Hole Singularity
// RXDK / Original Xbox — DX8 fixed-function pipeline
// 22 second scene, no shaders, no render targets.
//
// Effects:
//   - 350 static background stars + 200 orbital stars with gravitational
//     infall physics, heat tinting, motion streaks, and lens warp
//   - Lensing streaks on inner stars approximating Einstein arc smear
//   - 64x48 warp grid: 7 additive TEXTUREFACTOR bloom passes (atmosphere,
//     wide scatter, mid halo, tight core, chromatic aberration R/G/B)
//   - Photon ring with per-vertex flicker; expands to fill screen at t=16-22s
//   - Singularity core (source-over black disk, white-hot on flare)
//   - Shockwave: randomised annulus bursts every 4.7-6.3s, ~4 per scene
//   - Vignette crushes to tunnel vision over the final 6 seconds
//
// All bloom passes share one VB — one CPU lock per frame, zero allocations.

#include "PostFXScene.h"
#include <xtl.h>
#include <xgraphics.h>
#include <math.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// ─────────────────────────────────────────────────────────────────────────────
//  Screen / grid constants
// ─────────────────────────────────────────────────────────────────────────────
static const float SW = 640.0f;
static const float SH = 480.0f;
static const float CX = SW * 0.5f;
static const float CY = SH * 0.5f;

static const int GRID_X = 64;
static const int GRID_Y = 48;
static const int NVX = GRID_X + 1;
static const int NVY = GRID_Y + 1;

// ─────────────────────────────────────────────────────────────────────────────
//  Shared vertex type
// ─────────────────────────────────────────────────────────────────────────────
struct Vert { float x, y, z, rhw; DWORD color; };
#define FVF_VERT (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// ─────────────────────────────────────────────────────────────────────────────
//  Warp grid GPU buffers
// ─────────────────────────────────────────────────────────────────────────────
static LPDIRECT3DVERTEXBUFFER8 s_gridVB = nullptr;
static LPDIRECT3DINDEXBUFFER8  s_gridIB = nullptr;
static int                     s_numVerts = 0;
static int                     s_numIndices = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  Star field  — lifted directly from Credits.cpp
// ─────────────────────────────────────────────────────────────────────────────
static const int STAR_COUNT = 200;

struct Star
{
    float x, y;        // warped screen position (updated each frame)
    float spawnDist;
    float angle;       // orbital angle (radians)
    float dist;        // radial distance from CX,CY (pixels)
    float z;           // depth 0=far, 1=near
    BYTE  brightness;
    BYTE  colorType;
    float phase;       // twinkle phase
    float speed;       // pixels/sec infall this frame — drives motion streak
};

static Star  s_stars[STAR_COUNT];
static bool  s_starsInit = false;

static unsigned s_starSeed = 0x1234ABCD;
static unsigned StarRand()
{
    s_starSeed = s_starSeed * 1664525u + 1013904223u;
    return s_starSeed;
}

static DWORD GetStarColor(BYTE colorType, BYTE brightness, float time)
{
    float pulse = 0.85f + 0.15f * sinf(time * 0.5f + (float)colorType * 0.7f);
    unsigned b = (unsigned)((float)brightness * pulse);
    if (b > 255u) b = 255u;
    BYTE br = (BYTE)b;

    switch (colorType)
    {
    case 0:  return D3DCOLOR_ARGB(br, br, br, 255);
    case 1:  return D3DCOLOR_ARGB(br, (BYTE)(br >> 1), br, 255);
    case 2:  return D3DCOLOR_ARGB(br, 255, (BYTE)(br >> 1), 255);
    case 3:  return D3DCOLOR_ARGB(br, 255, 255, (BYTE)(br >> 1));
    case 4:  return D3DCOLOR_ARGB(br, 255, (BYTE)(br >> 1) + 80, (BYTE)(br >> 2));
    case 5:  return D3DCOLOR_ARGB(br, 200, 100, 255);
    case 6:  return D3DCOLOR_ARGB(br, (BYTE)(br >> 1), 255, (BYTE)(br >> 1));
    default: return D3DCOLOR_ARGB(br, 255, 255, 255);
    }
}

static void InitStarfield()
{
    if (s_starsInit) return;
    s_starSeed ^= GetTickCount();

    for (int i = 0; i < STAR_COUNT; ++i)
    {
        Star& s = s_stars[i];

        unsigned r = StarRand();
        float angle = (float)(r & 0xFFFF) * (6.2832f / 65535.0f);

        r = StarRand();
        float dist = 80.0f + (float)(r % 221u);

        s.angle = angle;
        s.dist = dist;
        s.spawnDist = dist;
        s.x = CX + cosf(angle) * dist;
        s.y = CY + sinf(angle) * dist;
        s.speed = 0.f;

        r = StarRand();
        s.z = (float)(r & 1023u) * (1.0f / 1023.0f);

        float fBright = 80.f + s.z * 175.0f;
        if (fBright > 255.f) fBright = 255.f;
        s.brightness = (BYTE)(unsigned)fBright;

        r = StarRand();
        s.colorType = (BYTE)(r & 7u);

        r = StarRand();
        s.phase = (float)(r & 0xFFFF) * (6.2832f / 65535.0f);
    }
    s_starsInit = true;
}

static void RespawnStar(Star& s)
{
    unsigned r = StarRand();
    s.angle = (float)(r & 0xFFFF) * (6.2832f / 65535.0f);

    r = StarRand();
    s.dist = 200.0f + (float)(r % 101u);
    s.spawnDist = s.dist;
    s.speed = 0.f;

    r = StarRand();
    s.z = (float)(r & 1023u) * (1.0f / 1023.0f);

    float fBright = 80.f + s.z * 175.0f;
    if (fBright > 255.f) fBright = 255.f;
    s.brightness = (BYTE)(unsigned)fBright;

    r = StarRand();
    s.colorType = (BYTE)(r & 7u);

    r = StarRand();
    s.phase = (float)(r & 0xFFFF) * (6.2832f / 65535.0f);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Background starfield — static, dense, dim
// ─────────────────────────────────────────────────────────────────────────────
static const int BG_STAR_COUNT = 350;

struct BgStar
{
    float x, y;
    float size;
    BYTE  brightness;
    BYTE  colorType;
    float phase;
};

static BgStar   s_bgStars[BG_STAR_COUNT];
static bool     s_bgStarsInit = false;
static unsigned s_bgSeed = 0xDEADBEEF;

static unsigned BgRand()
{
    s_bgSeed = s_bgSeed * 1664525u + 1013904223u;
    return s_bgSeed;
}

static void InitBgStarfield()
{
    if (s_bgStarsInit) return;
    s_bgSeed ^= (GetTickCount() >> 3) ^ 0xC0FFEE;

    for (int i = 0; i < BG_STAR_COUNT; ++i)
    {
        BgStar& s = s_bgStars[i];

        unsigned r = BgRand();
        s.x = (float)(r % 640u);

        r = BgRand();
        s.y = (float)(r % 480u);

        r = BgRand();
        s.size = ((r & 0xF) < 2) ? 1.5f : 1.0f;

        r = BgRand();
        s.brightness = (BYTE)(30u + (r & 0x3Fu));

        r = BgRand();
        s.colorType = (BYTE)(r & 7u);

        r = BgRand();
        s.phase = (float)(r & 0xFFFF) * (6.2832f / 65535.0f);
    }
    s_bgStarsInit = true;
}

static void RenderBgStars(float t)
{
    g_pDevice->SetVertexShader(FVF_VERT);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    for (int i = 0; i < BG_STAR_COUNT; ++i)
    {
        const BgStar& s = s_bgStars[i];

        float twinkle = 0.88f + 0.12f * sinf(t * 0.6f + s.phase);
        DWORD col = GetStarColor(s.colorType, s.brightness, t * 0.3f + s.phase);

        // Modulate alpha by twinkle
        unsigned alpha = (col >> 24) & 0xFF;
        float    fa = (float)alpha * twinkle;
        BYTE     a = (fa > 255.f) ? 255 : (BYTE)(unsigned)fa;
        col = (col & 0x00FFFFFF) | ((DWORD)a << 24);

        float sz = s.size;
        Vert quad[4] =
        {
            { s.x,      s.y,      0.f, 1.f, col },
            { s.x + sz, s.y,      0.f, 1.f, col },
            { s.x,      s.y + sz, 0.f, 1.f, col },
            { s.x + sz, s.y + sz, 0.f, 1.f, col },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Math helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline float Clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

// Xbox-safe float->BYTE: keep entirely in float, truncate only at the final
// unsigned cast which the compiler maps to a plain integer move.
static inline BYTE F2B(float f)
{
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    float v = f * 255.f;
    return (v >= 255.f) ? (BYTE)255u : (BYTE)(unsigned)v;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Warp kernel
// ─────────────────────────────────────────────────────────────────────────────
static void WarpPoint(
    float dx, float dy,
    float swirl, float lens,
    float t,
    float fx, float fy,
    float& outX, float& outY)
{
    float r2 = dx * dx + dy * dy;
    float r = (r2 > 1e-6f) ? sqrtf(r2) : 0.f;
    float ang = swirl * (1.f - r) + 0.35f * sinf(t * 0.75f);
    float cs = cosf(ang), sn = sinf(ang);
    float sx = dx * cs - dy * sn;
    float sy = dx * sn + dy * cs;
    float pull = 1.f - lens * (1.f - 1.f / (1.f + 6.f * r2));
    sx *= pull; sy *= pull;
    float shim = 0.2f + 0.8f * (1.f - r);
    outX = CX + sx * (SW * 0.5f) + 3.f * sinf(t * 1.2f + fx * 9.f + fy * 5.f) * shim;
    outY = CY + sy * (SH * 0.5f) + 3.f * cosf(t * 1.0f + fy * 11.f + fx * 4.f) * shim;
}

static inline float GlowAt(float r, float ringR, float ringW)
{
    float ring = Clamp01(1.f - fabsf(r * (SW * 0.5f) - ringR) / ringW);
    float core = Clamp01(1.f - r * 1.15f);
    return Clamp01(0.20f * core + 1.15f * ring);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Render state helpers
// ─────────────────────────────────────────────────────────────────────────────
static void StateBase()
{
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetTexture(0, NULL);
}

static void StateTFactor()
{
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

static void StateDiffuse()
{
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

static void BlendAdditive()
{
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
}

static void BlendNone()
{
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void BlendAlpha()
{
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Grid buffer creation
// ─────────────────────────────────────────────────────────────────────────────
static void CreateGridBuffers()
{
    s_numVerts = NVX * NVY;
    s_numIndices = GRID_X * GRID_Y * 6;

    g_pDevice->CreateVertexBuffer(
        s_numVerts * sizeof(Vert), 0, FVF_VERT, D3DPOOL_MANAGED, &s_gridVB);
    g_pDevice->CreateIndexBuffer(
        s_numIndices * sizeof(WORD), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &s_gridIB);

    WORD* ib = nullptr;
    s_gridIB->Lock(0, 0, (BYTE**)&ib, 0);
    int ii = 0;
    for (int y = 0; y < GRID_Y; ++y)
        for (int x = 0; x < GRID_X; ++x)
        {
            WORD i0 = (WORD)(y * NVX + x);
            WORD i1 = i0 + 1, i2 = i0 + NVX, i3 = i2 + 1;
            ib[ii++] = i0; ib[ii++] = i2; ib[ii++] = i3;
            ib[ii++] = i0; ib[ii++] = i3; ib[ii++] = i1;
        }
    s_gridIB->Unlock();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Per-frame updates
// ─────────────────────────────────────────────────────────────────────────────
static void UpdateGrid(float t)
{
    const float swirl = 1.05f + 0.25f * sinf(t * 0.28f);
    const float lens = 0.75f + 0.12f * cosf(t * 0.22f);
    const float ringR = 150.f + 18.f * sinf(t * 0.55f);
    const float ringW = 26.f;

    Vert* v = nullptr;
    s_gridVB->Lock(0, 0, (BYTE**)&v, 0);
    int idx = 0;
    for (int y = 0; y <= GRID_Y; ++y)
    {
        float fy = (float)y / GRID_Y;
        float py = fy * (SH - 1.f) - 0.5f;
        for (int x = 0; x <= GRID_X; ++x)
        {
            float fx = (float)x / GRID_X;
            float px = fx * (SW - 1.f) - 0.5f;
            float dx = (px - CX) / (SW * 0.5f);
            float dy = (py - CY) / (SH * 0.5f);
            float r2 = dx * dx + dy * dy;
            float r = sqrtf(r2 > 1e-6f ? r2 : 1e-6f);
            float wx, wy;
            WarpPoint(dx, dy, swirl, lens, t, fx, fy, wx, wy);
            BYTE g8 = F2B(GlowAt(r, ringR, ringW));
            v[idx++] = { wx, wy, 0.f, 1.f, D3DCOLOR_XRGB(g8,g8,g8) };
        }
    }
    s_gridVB->Unlock();
}

// Gravitational spiral — infall accelerates as dist shrinks.
// infall = depthFactor * GRAVITY / dist so stars loiter far out but
// plunge rapidly inside 120px.  No trig, no allocs — just arithmetic.
static void UpdateStars(float t, float dt)
{
    const float EVENT_HORIZON = 36.f;
    const float GRAVITY = 800.f;  // px^2/s — tuning knob
    const float swirl = 1.05f + 0.25f * sinf(t * 0.28f);
    const float lens = 0.75f + 0.12f * cosf(t * 0.22f);

    for (int i = 0; i < STAR_COUNT; ++i)
    {
        Star& s = s_stars[i];

        float depthFactor = 0.4f + 0.6f * s.z;

        // 5x multiplier inside 120px so the final plunge reads as a snap
        float distFactor = (s.dist < 120.f) ? 5.0f : 1.0f;
        float infall = depthFactor * GRAVITY * distFactor / (s.dist + 8.f) * dt;
        s.dist -= infall;
        s.speed = infall / dt;  // px/s — used for motion streak length

        float angularV = depthFactor * (0.20f + 80.f / (s.dist + 8.f)) * dt;
        s.angle += angularV;

        if (s.dist < EVENT_HORIZON)
        {
            RespawnStar(s);
        }

        float rawX = CX + cosf(s.angle) * s.dist;
        float rawY = CY + sinf(s.angle) * s.dist;

        float dx = (rawX - CX) / (SW * 0.5f);
        float dy = (rawY - CY) / (SH * 0.5f);
        float wx, wy;
        WarpPoint(dx, dy, swirl, lens, t, 0.f, 0.f, wx, wy);

        s.x = wx;
        s.y = wy;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Star rendering — motion-stretched quads
//  Fast inner stars get a tapered inward trail; slow outer stars stay as dots.
// ─────────────────────────────────────────────────────────────────────────────
static void RenderStars(float t)
{
    g_pDevice->SetVertexShader(FVF_VERT);
    BlendAdditive();
    StateDiffuse();

    for (int i = 0; i < STAR_COUNT; ++i)
    {
        const Star& s = s_stars[i];

        if (s.x < -4.f || s.x > SW + 4.f || s.y < -4.f || s.y > SH + 4.f)
            continue;

        float heatFactor = Clamp01(1.f - (s.dist - 36.f) / 200.f);
        float twinkle = 0.9f + 0.2f * sinf(t * 2.0f + s.phase);

        DWORD baseCol = GetStarColor(s.colorType, s.brightness, t + (float)i * 0.1f);
        BYTE  bR = (BYTE)((baseCol >> 16) & 0xFF);
        BYTE  bG = (BYTE)((baseCol >> 8) & 0xFF);
        BYTE  bB = (BYTE)(baseCol & 0xFF);
        BYTE  bA = (BYTE)((baseCol >> 24) & 0xFF);

        // Heat tint toward orange-white — pure float arithmetic, no int cast
        float fR = (float)bR + (255.f - (float)bR) * heatFactor * 0.7f;
        float fG = (float)bG + (180.f - (float)bG) * heatFactor * 0.5f;
        float fB = (float)bB * (1.f - heatFactor * 0.5f);
        bR = (fR >= 255.f) ? 255 : (BYTE)(unsigned)fR;
        bG = (fG >= 255.f) ? 255 : (BYTE)(unsigned)fG;
        bB = (fB >= 255.f) ? 255 : (BYTE)(unsigned)fB;

        DWORD col = D3DCOLOR_ARGB(bA, bR, bG, bB);

        float size = (1.0f + s.z * 1.5f) * twinkle;

        // Motion streak: elongate toward singularity scaled by infall speed
        float streakLen = Clamp01(s.speed * 0.004f) * 22.f;

        float toDx = CX - s.x;
        float toDy = CY - s.y;
        float toLen = sqrtf(toDx * toDx + toDy * toDy);
        if (toLen < 1.f) toLen = 1.f;
        float nDx = toDx / toLen;
        float nDy = toDy / toLen;

        float pDx = -nDy;
        float pDy = nDx;
        float hw = size * 0.5f;

        DWORD colTail = D3DCOLOR_ARGB(0, bR, bG, bB);
        float hx = s.x + nDx * streakLen;
        float hy = s.y + nDy * streakLen;

        Vert quad[4] =
        {
            { s.x + pDx * hw, s.y + pDy * hw, 0.f, 1.f, col     },
            { s.x - pDx * hw, s.y - pDy * hw, 0.f, 1.f, col     },
            { hx + pDx * hw, hy + pDy * hw, 0.f, 1.f, colTail },
            { hx - pDx * hw, hy - pDy * hw, 0.f, 1.f, colTail },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Gravitational lensing streaks
// ─────────────────────────────────────────────────────────────────────────────
static void DrawLensingStreaks(float t)
{
    BlendAdditive();
    StateDiffuse();
    g_pDevice->SetVertexShader(FVF_VERT);

    for (int i = 0; i < STAR_COUNT; ++i)
    {
        const Star& s = s_stars[i];
        if (s.dist > 185.f || s.z < 0.45f) continue;

        float proximity = Clamp01(1.f - (s.dist - 36.f) / 149.f);
        float streakLen = 6.f + proximity * 34.f;
        float streakW = 0.8f + s.z * 0.9f;

        float trailDx = sinf(s.angle);
        float trailDy = -cosf(s.angle);
        float tipX = s.x + trailDx * streakLen;
        float tipY = s.y + trailDy * streakLen;
        float perpX = -trailDy;
        float perpY = trailDx;

        float heatFactor = Clamp01(1.f - (s.dist - 36.f) / 200.f);
        DWORD baseCol = GetStarColor(s.colorType, s.brightness, t + (float)i * 0.1f);
        BYTE  bR = (BYTE)((baseCol >> 16) & 0xFF);
        BYTE  bG = (BYTE)((baseCol >> 8) & 0xFF);
        BYTE  bB = (BYTE)(baseCol & 0xFF);

        float fR = (float)bR + (255.f - (float)bR) * heatFactor * 0.7f;
        float fG = (float)bG + (180.f - (float)bG) * heatFactor * 0.5f;
        float fB = (float)bB * (1.f - heatFactor * 0.5f);
        bR = (fR >= 255.f) ? 255 : (BYTE)(unsigned)fR;
        bG = (fG >= 255.f) ? 255 : (BYTE)(unsigned)fG;
        bB = (fB >= 255.f) ? 255 : (BYTE)(unsigned)fB;

        BYTE  baseAlpha = F2B(proximity * s.z * 0.75f);
        DWORD colBase = D3DCOLOR_ARGB(baseAlpha, bR, bG, bB);
        DWORD colTip = D3DCOLOR_ARGB(0, bR, bG, bB);

        Vert quad[4] =
        {
            { s.x + perpX * streakW, s.y + perpY * streakW, 0.f, 1.f, colBase },
            { s.x - perpX * streakW, s.y - perpY * streakW, 0.f, 1.f, colBase },
            { tipX + perpX * 0.2f,    tipY + perpY * 0.2f,    0.f, 1.f, colTip  },
            { tipX - perpX * 0.2f,    tipY - perpY * 0.2f,    0.f, 1.f, colTip  },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vert));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Photon ring
// ─────────────────────────────────────────────────────────────────────────────
static void DrawPhotonRing(float t)
{
    const int   SEGS = 64;
    const float CORER = 28.f + 4.f * sinf(t * 1.3f);
    const float PR = CORER + 5.f;
    const float PW = 7.f;

    Vert fan[SEGS + 2];
    fan[0] = { CX, CY, 0.f, 1.f, D3DCOLOR_XRGB(0,0,0) };
    for (int i = 0; i <= SEGS; ++i)
    {
        float a = (float)i / SEGS * 6.2832f;
        float rVar = PR + PW * sinf((float)i * 0.4f + t * 3.f) * 0.25f;
        float flicker = 0.55f + 0.45f * sinf((float)i * 0.9f + t * 5.7f);
        float bluF = 0.78f + 0.22f * cosf((float)i * 0.5f + t * 2.f);
        BYTE  lum = F2B(flicker);
        BYTE  blu = F2B(bluF);
        float lumF = (float)lum * 0.75f;
        BYTE  lumMid = (lumF >= 255.f) ? 255 : (BYTE)(unsigned)lumF;
        fan[i + 1] = { CX + cosf(a) * rVar, CY + sinf(a) * rVar, 0.f, 1.f,
                     D3DCOLOR_XRGB(lum, lumMid, blu) };
    }
    BlendAdditive();
    StateDiffuse();
    g_pDevice->SetVertexShader(FVF_VERT);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEGS, fan, sizeof(Vert));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Singularity core
// ─────────────────────────────────────────────────────────────────────────────
static void DrawSingularity(float t, float flareBurst)
{
    const int SEGS = 32;
    float breathe = 28.f + 4.f * sinf(t * 1.3f);
    float CORER = breathe + flareBurst * 22.f;

    BYTE  lum = F2B(flareBurst);
    DWORD coreCol = D3DCOLOR_XRGB(lum, lum, lum);

    Vert fan[SEGS + 2];
    fan[0] = { CX, CY, 0.f, 1.f, coreCol };
    for (int i = 0; i <= SEGS; ++i)
    {
        float a = (float)i / SEGS * 6.2832f;
        fan[i + 1] = { CX + cosf(a) * CORER, CY + sinf(a) * CORER, 0.f, 1.f, coreCol };
    }
    BlendNone();
    StateDiffuse();
    g_pDevice->SetVertexShader(FVF_VERT);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEGS, fan, sizeof(Vert));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Shockwave — randomised burst intervals (3-8s)
// ─────────────────────────────────────────────────────────────────────────────
static const float SHOCK_DURATION = 1.8f;
static const float SHOCK_MAX_R = 280.f;

static float    s_shockNextFire = 2.0f;
static float    s_shockFiredAt = -999.f;
static unsigned s_shockSeed = 0xFACEB00C;

static unsigned ShockRand()
{
    s_shockSeed = s_shockSeed * 1664525u + 1013904223u;
    return s_shockSeed;
}

static float ShockPhase(float t)
{
    if (t >= s_shockNextFire && s_shockFiredAt < s_shockNextFire)
    {
        s_shockFiredAt = t;
        // Jitter ±0.8s around 5.5s base — guarantees ~4 bursts in 22s
        float jitter = (float)(ShockRand() & 0xFF) / 255.f;
        s_shockNextFire = t + 4.7f + jitter * 1.6f;
    }
    if (s_shockFiredAt < 0.f) return 1.f;
    float phase = (t - s_shockFiredAt) / SHOCK_DURATION;
    return (phase > 1.f) ? 1.f : phase;
}

static void DrawShockwave(float t)
{
    float phase = ShockPhase(t);
    if (phase >= 1.f) return;

    float eased = 1.f - (1.f - phase) * (1.f - phase);
    float radius = eased * SHOCK_MAX_R;
    float alpha = Clamp01(1.f - phase * phase * 1.4f);

    BYTE  cr = F2B((1.f - phase * 0.6f) * alpha);
    BYTE  cg = F2B((0.7f + phase * 0.1f) * alpha * 0.7f);
    BYTE  cb = F2B((0.8f + phase * 0.2f) * alpha);

    const int   SEGS = 64;
    const float RIM_W = 4.f + (1.f - phase) * 8.f;

    for (int pass = 0; pass < 2; ++pass)
    {
        float innerR = radius - (pass == 0 ? RIM_W * 3.f : RIM_W * 0.5f);
        float outerR = radius + (pass == 0 ? RIM_W * 3.f : RIM_W * 0.5f);
        float pAlpha = (pass == 0) ? alpha * 0.25f : alpha * 0.85f;
        if (innerR < 0.f) innerR = 0.f;

        BYTE  pa = F2B(pAlpha);
        DWORD colInner = D3DCOLOR_ARGB(pa, cr, cg, cb);
        DWORD colOuter = D3DCOLOR_ARGB(0, cr, cg, cb);

        Vert strip[(SEGS + 1) * 2];
        for (int i = 0; i <= SEGS; ++i)
        {
            float a = (float)i / SEGS * 6.2832f;
            float cs = cosf(a), sn = sinf(a);
            strip[i * 2 + 0] = { CX + cs * innerR, CY + sn * innerR, 0.f, 1.f, colInner };
            strip[i * 2 + 1] = { CX + cs * outerR, CY + sn * outerR, 0.f, 1.f, colOuter };
        }
        BlendAdditive();
        StateDiffuse();
        g_pDevice->SetVertexShader(FVF_VERT);
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, SEGS * 2, strip, sizeof(Vert));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Vignette
// ─────────────────────────────────────────────────────────────────────────────
static void DrawVignette()
{
    const int VSEGS = 12;
    Vert fan[VSEGS + 2];
    fan[0] = { CX, CY, 0.f, 1.f, D3DCOLOR_ARGB(0,0,0,0) };
    for (int i = 0; i <= VSEGS; ++i)
    {
        float a = (float)i / VSEGS * 6.2832f;
        fan[i + 1] = { CX + cosf(a) * (SW * 0.82f), CY + sinf(a) * (SH * 0.82f),
                     0.f, 1.f, D3DCOLOR_ARGB(200,0,0,0) };
    }
    BlendAlpha();
    g_pDevice->SetVertexShader(FVF_VERT);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, VSEGS, fan, sizeof(Vert));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Grid draw
// ─────────────────────────────────────────────────────────────────────────────
static void DrawGrid()
{
    g_pDevice->SetVertexShader(FVF_VERT);
    g_pDevice->SetStreamSource(0, s_gridVB, sizeof(Vert));
    g_pDevice->SetIndices(s_gridIB, 0);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
        0, s_numVerts,
        0, s_numIndices / 3);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public interface
// ─────────────────────────────────────────────────────────────────────────────
void PostFXScene_Init()
{
    if (!g_pDevice) return;

    if (s_gridVB) { s_gridVB->Release(); s_gridVB = nullptr; }
    if (s_gridIB) { s_gridIB->Release(); s_gridIB = nullptr; }

    CreateGridBuffers();
    InitStarfield();
    InitBgStarfield();

    s_shockFiredAt = -999.f;
    s_shockNextFire = 2.0f;
}

void PostFXScene_Shutdown()
{
    if (s_gridVB) { s_gridVB->Release(); s_gridVB = nullptr; }
    if (s_gridIB) { s_gridIB->Release(); s_gridIB = nullptr; }
    s_starsInit = false;
    s_bgStarsInit = false;
}

static float s_prevTime = 0.f;

void PostFXScene_Render(float demoTime)
{
    if (!g_pDevice || !s_gridVB || !s_gridIB) return;

    const float t = demoTime;
    const float dt = (t > s_prevTime) ? (t - s_prevTime) : 0.016f;
    s_prevTime = t;

    const float masterPulse = 0.80f + 0.20f * sinf(t * 0.42f);

    float shockPhase = ShockPhase(t);
    float flareBurst = (shockPhase < 1.f)
        ? Clamp01(1.f - shockPhase * 3.5f)
        : 0.f;
    flareBurst = flareBurst * flareBurst;

    UpdateGrid(t);
    UpdateStars(t, dt);

    StateBase();

    RenderBgStars(t);
    RenderStars(t);
    DrawLensingStreaks(t);

    StateTFactor();

    BlendNone();
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(30, 35, 58));
    DrawGrid();

    BlendAdditive();

    // Bloom pass 0 — atmosphere
    {
        float base = 0.08f + 0.03f * sinf(t * 0.19f);
        float a = Lerp(base, 1.0f, flareBurst);
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,
            D3DCOLOR_XRGB(F2B(a * 0.55f), F2B(a * 0.62f), F2B(a * 0.80f)));
        DrawGrid();
    }
    // Bloom pass A — wide purple scatter
    {
        float base = masterPulse * (0.28f + 0.08f * sinf(t * 0.31f));
        float a = Lerp(base, 1.0f, flareBurst);
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,
            D3DCOLOR_XRGB(F2B(a * 0.55f), F2B(a * 0.28f), F2B(a * 0.92f)));
        DrawGrid();
    }
    // Bloom pass B — mid cyan-purple halo
    {
        float base = masterPulse * (0.45f + 0.12f * cosf(t * 0.55f));
        float a = Lerp(base, 1.0f, flareBurst);
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,
            D3DCOLOR_XRGB(F2B(a * (0.48f + 0.25f * sinf(t * 0.70f))),
                F2B(a * (0.55f + 0.10f * cosf(t * 0.44f))),
                F2B(a * (0.95f + 0.05f * sinf(t * 0.63f)))));
        DrawGrid();
    }
    // Bloom pass C — tight near-white core
    {
        float base = masterPulse * (0.65f + 0.15f * sinf(t * 0.88f + 1.1f));
        float a = Lerp(base, 1.0f, flareBurst);
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,
            D3DCOLOR_XRGB(F2B(a * 0.90f), F2B(a * 0.82f), F2B(a * 1.00f)));
        DrawGrid();
    }
    // Chromatic aberration — R, G, B
    {
        BYTE r = F2B(masterPulse * (0.70f + 0.30f * sinf(t * 1.10f)));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(r, 18, 18));
        DrawGrid();
    }
    {
        BYTE g = F2B(masterPulse * (0.65f + 0.30f * sinf(t * 0.97f + 2.1f)));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(18, g, 18));
        DrawGrid();
    }
    {
        BYTE b = F2B(masterPulse * (0.72f + 0.28f * sinf(t * 0.83f + 4.2f)));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(18, 18, b));
        DrawGrid();
    }

    DrawPhotonRing(t);
    DrawSingularity(t, flareBurst);
    DrawShockwave(t);
    DrawVignette();

    BlendNone();
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}