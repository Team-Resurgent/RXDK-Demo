// CubeScene.cpp - Spinning cube with "Matrix rain" RXDK streams on all 6 faces (RXDK-safe)
//
// Changes / polish:
// - Smooth face “culling” via per-face fade (no popping)
// - Fix ClampF undefined (not used)
// - Performance: skip back-facing faces + adaptive glow (head = strong, trail = lighter)
// - Only "RXDK" glyphs trailing, readable, cinematic glow
//
// Constraints:
// - No per-frame allocations
// - No RNG in Render (Init-only)
// - Avoid float->int casts in Render (no (int) from float values)

#include "CubeScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>

#include "font.h"

extern LPDIRECT3DDEVICE8 g_pDevice;

// ------------------------------------------------------------
// Scene control
// ------------------------------------------------------------

static bool  s_active = false;
static DWORD s_startTicks = 0;
static const DWORD SCENE_DURATION_MS = 22000;

// ------------------------------------------------------------
// Trig LUT (int-indexed)
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
// Deterministic PRNG (Init-only)
// ------------------------------------------------------------

static unsigned s_rng = 0xC0B3F00Du;

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
// Glyph setup (ONLY "RXDK")
// ------------------------------------------------------------

static const char* GLYPHS = "RXDK";
static const int   GLYPH_N = 4;

static const int FACE_COLS = 10;
static const int FACE_ROWS = 8;

// ------------------------------------------------------------
// Stream setup (per face/column)
// ------------------------------------------------------------

struct ColStream
{
    unsigned seed;      // stable hash base
    unsigned phase;     // 0..(wrap-1)
    unsigned stepDiv;   // speed: head advances by 1 row every stepDiv steps
    unsigned tailLen;   // visible trail length
    unsigned gapLen;    // dead air after tail
};

static ColStream s_col[6][FACE_COLS];
static bool      s_built = false;

// ------------------------------------------------------------
// Camera / projection helpers
// ------------------------------------------------------------

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

static void SetupCamera(D3DXMATRIX* outView, D3DXMATRIX* outProj)
{
    D3DXVECTOR3 eye(0.0f, 0.0f, -6.2f);
    D3DXVECTOR3 at(0.0f, 0.0f, 0.0f);
    D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);

    D3DXMatrixLookAtLH(outView, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(outProj, D3DX_PI / 3.0f, SCREEN_W / SCREEN_H, 0.1f, 100.0f);

    if (g_pDevice)
    {
        g_pDevice->SetTransform(D3DTS_VIEW, outView);
        g_pDevice->SetTransform(D3DTS_PROJECTION, outProj);
    }
}

static inline bool ProjectToScreen(const D3DXVECTOR3& p, const D3DXMATRIX& mWVP, float* outX, float* outY, float* outZ)
{
    D3DXVECTOR3 q;
    D3DXVec3TransformCoord(&q, &p, &mWVP);

    *outX = (q.x * 0.5f + 0.5f) * SCREEN_W;
    *outY = (-q.y * 0.5f + 0.5f) * SCREEN_H;
    *outZ = q.z;

    if (*outX < -64.0f || *outX >(SCREEN_W + 64.0f) || *outY < -64.0f || *outY >(SCREEN_H + 64.0f))
        return false;

    return true;
}

// ------------------------------------------------------------
// Face mapping
// ------------------------------------------------------------

static inline D3DXVECTOR3 FacePoint(int face, float u, float v, float s)
{
    // 0:+Z, 1:-Z, 2:+X, 3:-X, 4:+Y, 5:-Y
    if (face == 0) return D3DXVECTOR3(u * s, v * s, +s);
    if (face == 1) return D3DXVECTOR3(-u * s, v * s, -s);
    if (face == 2) return D3DXVECTOR3(+s, v * s, -u * s);
    if (face == 3) return D3DXVECTOR3(-s, v * s, +u * s);
    if (face == 4) return D3DXVECTOR3(u * s, +s, -v * s);
    return          D3DXVECTOR3(u * s, -s, +v * s);
}

// Local-space normals for each face (matches FacePoint mapping)
static const D3DXVECTOR3 s_faceN[6] =
{
    D3DXVECTOR3(0,  0,  1), // +Z
    D3DXVECTOR3(0,  0, -1), // -Z
    D3DXVECTOR3(1,  0,  0), // +X
    D3DXVECTOR3(-1,  0,  0), // -X
    D3DXVECTOR3(0,  1,  0), // +Y
    D3DXVECTOR3(0, -1,  0)  // -Y
};

// ------------------------------------------------------------
// Build streams (Init-only)
// ------------------------------------------------------------

static void BuildStreams()
{
    if (s_built) return;

    s_rng ^= GetTickCount();

    for (int f = 0; f < 6; ++f)
    {
        for (int c = 0; c < FACE_COLS; ++c)
        {
            unsigned sd = RngU32() ^ (unsigned)(f * 1337u + c * 97u);

            // speed bucket (smoothed)
            unsigned stepDiv = 2u + (sd & 3u); // 2..5
            sd = (sd * 1664525u + 1013904223u);

            unsigned tailLen = 5u + (sd & 7u); // 5..12
            sd = (sd * 1664525u + 1013904223u);

            unsigned gapLen = 3u + (sd & 7u); // 3..10
            sd = (sd * 1664525u + 1013904223u);

            if (tailLen > (unsigned)(FACE_ROWS + 6)) tailLen = (unsigned)(FACE_ROWS + 6);

            unsigned wrap = (unsigned)FACE_ROWS + tailLen + gapLen;
            unsigned phase = (sd % wrap);

            s_col[f][c].seed = sd ^ 0x9E3779B9u;
            s_col[f][c].phase = phase;
            s_col[f][c].stepDiv = stepDiv;
            s_col[f][c].tailLen = tailLen;
            s_col[f][c].gapLen = gapLen;
        }
    }

    s_built = true;
}

// ------------------------------------------------------------
// Render helpers
// ------------------------------------------------------------

static inline BYTE ClampU8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

// Adaptive glow: choose cheaper draw path when dim / far / fading faces
static void DrawGlowTextAdaptive(float x, float y, const char* s, float scale, DWORD coreCol, DWORD glow1, DWORD glow2, bool strongGlow)
{
    if (strongGlow)
    {
        // outer glow (soft)
        DrawText(x - 1.6f, y, s, scale * 1.10f, glow2);
        DrawText(x + 1.6f, y, s, scale * 1.10f, glow2);
        DrawText(x, y - 1.6f, s, scale * 1.10f, glow2);
        DrawText(x, y + 1.6f, s, scale * 1.10f, glow2);

        // mid glow
        DrawText(x - 1.0f, y - 1.0f, s, scale * 1.06f, glow1);
        DrawText(x + 1.0f, y - 1.0f, s, scale * 1.06f, glow1);
        DrawText(x - 1.0f, y + 1.0f, s, scale * 1.06f, glow1);
        DrawText(x + 1.0f, y + 1.0f, s, scale * 1.06f, glow1);
    }
    else
    {
        // cheaper glow: 4 taps only
        DrawText(x - 1.0f, y, s, scale * 1.05f, glow1);
        DrawText(x + 1.0f, y, s, scale * 1.05f, glow1);
        DrawText(x, y - 1.0f, s, scale * 1.05f, glow1);
        DrawText(x, y + 1.0f, s, scale * 1.05f, glow1);
    }

    // core
    DrawText(x, y, s, scale, coreCol);
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void CubeScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();

    BuildLUT();
    BuildStreams();
}

void CubeScene_Shutdown()
{
    s_active = false;
}

bool CubeScene_IsFinished()
{
    if (!s_active) return true;
    return (GetTickCount() - s_startTicks) >= SCENE_DURATION_MS;
}

void CubeScene_Render(float)
{
    if (!s_active || !g_pDevice)
        return;

    DWORD tMs = GetTickCount() - s_startTicks;

    // camera
    D3DXMATRIX view, proj;
    SetupCamera(&view, &proj);

    // cube motion (float-only)
    float t = (float)tMs * 0.001f;

    int idxA = (int)((tMs / 6) & 1023);
    int idxB = (int)((tMs / 9) & 1023);

    float yaw = t * 0.75f;
    float pitch = t * 0.48f + 0.10f * s_sin[idxA];
    float roll = t * 0.22f + 0.08f * s_sin[idxB];

    D3DXMATRIX world;
    D3DXMatrixRotationYawPitchRoll(&world, yaw, pitch, roll);

    D3DXMATRIX wvp = world * view * proj;

    // text overlay states
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);

    // layout
    const float cubeS = 2.15f;
    const float uStep = 2.0f / (float)(FACE_COLS - 1);
    const float vStep = 2.0f / (float)(FACE_ROWS - 1);

    // movement step (integer-only)
    unsigned frameStep = (unsigned)(tMs / 55);

    char one[2] = { 0, 0 };

    for (int f = 0; f < 6; ++f)
    {
        // ---- Smooth face fade (no popping) + performance skip ----
        D3DXVECTOR3 nW;
        D3DXVec3TransformNormal(&nW, &s_faceN[f], &world);

        // camera looks along +Z with current view setup
        float facing = nW.z;

        // If heavily turned away, skip the whole face (big perf win)
        if (facing < -0.15f)
            continue;

        // Alpha penalty tiers as face turns away
        int facePenalty = 0;
        if (facing < -0.05f) facePenalty = 190;
        else if (facing < 0.05f) facePenalty = 140;
        else if (facing < 0.15f) facePenalty = 95;
        else if (facing < 0.25f) facePenalty = 55;

        // also decide glow strength per face (cheaper when fading)
        bool faceStrongGlow = (facePenalty <= 55);

        for (int c = 0; c < FACE_COLS; ++c)
        {
            const ColStream& cs = s_col[f][c];
            unsigned wrap = (unsigned)FACE_ROWS + cs.tailLen + cs.gapLen;

            // head advances by 1 every stepDiv steps (integer-only)
            unsigned head = (frameStep / cs.stepDiv + cs.phase) % wrap;

            // cinematic sway (face/col based) - integer-driven LUT
            unsigned wobIdx = (frameStep + (unsigned)c * 19u + (unsigned)f * 37u) & 1023u;

            for (int r = 0; r < FACE_ROWS; ++r)
            {
                unsigned vr = (unsigned)r;
                unsigned dist = (head >= vr) ? (head - vr) : ((head + wrap) - vr);

                if (dist > cs.tailLen)
                    continue;

                float uBase = -1.0f + uStep * (float)c;
                float vBase = -1.0f + vStep * (float)r;

                D3DXVECTOR3 p = FacePoint(f, uBase, vBase, cubeS);

                float sx, sy, sz;
                if (!ProjectToScreen(p, wvp, &sx, &sy, &sz))
                    continue;

                // readable scale (no ClampF)
                float zScale = 1.0f - (sz * 0.33f);
                if (zScale < 0.62f) zScale = 0.62f;
                if (zScale > 1.22f) zScale = 1.22f;
                float scale = 0.92f * zScale;

                // choose RXDK letter (integer-only)
                unsigned h = cs.seed ^ (unsigned)(f * 977u + c * 131u + (unsigned)r * 73u);
                unsigned gi = (h + frameStep * (dist == 0u ? 7u : 3u) + dist * 11u) & 3u;
                one[0] = GLYPHS[gi];

                // brightness profile
                int a, rr, gg, bb;

                if (dist == 0u)
                {
                    // head (bright)
                    a = 255;
                    rr = 235;
                    gg = 255;
                    bb = 235;
                }
                else
                {
                    int d = (int)dist;

                    if (d <= 2) { a = 210; rr = 60; gg = 245; bb = 90; }
                    else if (d <= 4) { a = 170; rr = 35; gg = 228; bb = 80; }
                    else if (d <= 7) { a = 130; rr = 22; gg = 212; bb = 70; }
                    else { a = 95; rr = 16; gg = 195; bb = 60; }
                }

                // depth fade (gentle)
                if (sz > 0.90f) a -= 14;
                if (sz > 0.98f) a -= 14;

                // apply face fade penalty (smooth “culling”)
                a -= facePenalty;
                if (a <= 0)
                    continue;

                // sway (cinematic)
                float swayX = 0.9f * s_sin[wobIdx];
                float swayY = 0.6f * s_cos[wobIdx];

                BYTE A = ClampU8(a);
                BYTE RR = ClampU8(rr);
                BYTE GG = ClampU8(gg);
                BYTE BB = ClampU8(bb);

                DWORD core = D3DCOLOR_ARGB(A, RR, GG, BB);

                // glow alpha scaled by visibility + head
                int g1a = (int)A - (dist == 0u ? 35 : 65);
                int g2a = (int)A - (dist == 0u ? 75 : 120);
                if (g1a < 10) g1a = 10;
                if (g2a < 6) g2a = 6;

                DWORD glow1 = D3DCOLOR_ARGB((BYTE)g1a, 10, ClampU8((int)GG + 30), 70);
                DWORD glow2 = D3DCOLOR_ARGB((BYTE)g2a, 6, ClampU8((int)GG + 15), 55);

                // Strong glow only for heads and “front-ish” faces
                bool strongGlow = (dist == 0u) && faceStrongGlow;

                DrawGlowTextAdaptive(sx + swayX, sy + swayY, one, scale, core, glow1, glow2, strongGlow);
            }
        }
    }

    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
}
