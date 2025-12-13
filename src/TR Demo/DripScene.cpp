// ============================================================================
// DripScene.cpp - Water Ripple Effect with Interactive Controls
// ============================================================================
// 
// Controls:
//   Y Button - Toggle rain effect on/off
//
// Features:
//   - Real-time water ripple simulation with damping
//   - Multi-layer water shading:
//     * Depth-based color gradient (darker in deep areas)
//     * Specular highlights on wave peaks
//     * White foam on sharp crests
//     * Animated caustics simulation
//     * Height-based lighting
//   - Rain mode for continuous droplet generation
//   - Random ambient droplets
//   - Splash highlights at impact points
//
// ============================================================================

#include "DripScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <string.h>

#include "input.h"

extern IDirect3DDevice8* g_pd3dDevice;

// Xbox D3D8 doesn't define these, so we define them manually
#ifndef D3DFILLMODE_SOLID
#define D3DFILLMODE_SOLID 3
#endif

#ifndef D3DFILLMODE_WIREFRAME
#define D3DFILLMODE_WIREFRAME 2
#endif

namespace
{
    // -------------------------------------------------------------------------
    // Grid / screen
    // -------------------------------------------------------------------------
    static const int GRID_W = 192;
    static const int GRID_H = 144;
    static const int SCREEN_W = 640;
    static const int SCREEN_H = 480;

    // -------------------------------------------------------------------------
    // Ripple solver (integer)
    // -------------------------------------------------------------------------
    static const int DAMP = 247;
    static const int STEPS_PER_FRAME = 2;

    // Visual lift
    static const int HEIGHT_SCALE = 6;
    static const int SPLASH_SCALE = 3;

    // -------------------------------------------------------------------------
    // Wind waves
    // -------------------------------------------------------------------------
    static int g_windPhase = 0;
    static const int WIND_SPEED = 1;

    // -------------------------------------------------------------------------
    // Visual effects
    // -------------------------------------------------------------------------
    static int g_renderMode = 0; // 0=normal, 1=wireframe, 2=both, 3=height-based colors
    static bool g_rainEnabled = false;
    static int g_rainCounter = 0;

    // -------------------------------------------------------------------------
    // Simulation buffers
    // -------------------------------------------------------------------------
    static SHORT g_bufA[GRID_W * GRID_H];
    static SHORT g_bufB[GRID_W * GRID_H];
    static SHORT g_splash[GRID_W * GRID_H];
    static int   g_ping = 0;

    __forceinline int IDX(int x, int y) { return y * GRID_W + x; }

    // -------------------------------------------------------------------------
    // RNG
    // -------------------------------------------------------------------------
    static DWORD g_rng = 0x12345678;
    __forceinline DWORD LcgNext()
    {
        g_rng = g_rng * 1664525u + 1013904223u;
        return g_rng;
    }

    // -------------------------------------------------------------------------
    // Vertex (screen-space)
    // -------------------------------------------------------------------------
    struct Vtx
    {
        float x, y, z, rhw;
        DWORD diffuse;
    };

    static const DWORD FVF_VTX = D3DFVF_XYZRHW | D3DFVF_DIFFUSE;

    static IDirect3DVertexBuffer8* g_vb = NULL;
    static IDirect3DIndexBuffer8* g_ibTri = NULL;
    static IDirect3DIndexBuffer8* g_ibLine = NULL;
    static int g_triCount = 0;
    static int g_lineCount = 0;

    // -------------------------------------------------------------------------
    // Input
    // -------------------------------------------------------------------------
    static WORD g_lastButtons = 0;

    // -------------------------------------------------------------------------
    // Helpers
    // -------------------------------------------------------------------------
    static void ClearSim()
    {
        memset(g_bufA, 0, sizeof(g_bufA));
        memset(g_bufB, 0, sizeof(g_bufB));
        memset(g_splash, 0, sizeof(g_splash));
        g_ping = 0;
        g_windPhase = 0;
        g_rainCounter = 0;
    }

    static void AddDrop(int cx, int cy, int radius, int strength)
    {
        int r2 = radius * radius;
        SHORT* cur = (g_ping == 0) ? g_bufA : g_bufB;

        for (int y = cy - radius; y <= cy + radius; ++y)
        {
            if ((unsigned)y >= (unsigned)GRID_H) continue;
            int dy = y - cy;
            int dy2 = dy * dy;

            for (int x = cx - radius; x <= cx + radius; ++x)
            {
                if ((unsigned)x >= (unsigned)GRID_W) continue;
                int dx = x - cx;
                int d2 = dx * dx + dy2;
                if (d2 > r2) continue;

                int fall = (r2 - d2);
                int impulse = (strength * fall) / (r2 ? r2 : 1);

                int i = IDX(x, y);
                cur[i] = (SHORT)((int)cur[i] + impulse);

                if (dx == 0 && dy == 0)
                    g_splash[i] = 2400;
            }
        }
    }

    static void StepSimOnce()
    {
        SHORT* cur = (g_ping == 0) ? g_bufA : g_bufB;
        SHORT* prev = (g_ping == 0) ? g_bufB : g_bufA;

        for (int y = 1; y < GRID_H - 1; ++y)
        {
            int row = y * GRID_W;
            for (int x = 1; x < GRID_W - 1; ++x)
            {
                int i = row + x;
                int n =
                    cur[i - 1] +
                    cur[i + 1] +
                    cur[i - GRID_W] +
                    cur[i + GRID_W];

                int next = (n >> 1) - prev[i];
                prev[i] = (SHORT)((next * DAMP) >> 8);
            }
        }

        for (int i = 0; i < GRID_W * GRID_H; ++i)
            if (g_splash[i] > 0)
                g_splash[i] -= (g_splash[i] >> 2) + 1;

        g_ping ^= 1;
    }

    // Polished water color with depth, foam, highlights, and caustics
    static DWORD WaterColorFromSlope(int slope, int height, int x, int y)
    {
        // Calculate slope magnitude and direction
        int s = slope < 0 ? -slope : slope;

        // Base depth color - darker blue in deeper areas
        int depthFactor = 255 - ((y * 180) / GRID_H); // Darker as we go down
        int baseR = (depthFactor * 40) / 255;
        int baseG = (depthFactor * 80) / 255;
        int baseB = (depthFactor * 140) / 255;

        // Specular highlights on steep slopes (wave peaks)
        int specular = 0;
        if (s > 400) {
            specular = ((s - 400) >> 1);
            if (specular > 180) specular = 180;
        }

        // Foam on very sharp peaks
        int foam = 0;
        if (s > 800) {
            foam = ((s - 800) >> 2);
            if (foam > 120) foam = 120;
        }

        // Caustics simulation - subtle light patterns
        int causticPhase = (x * 7 + y * 11 + g_windPhase) & 255;
        int caustic = (causticPhase > 128) ? ((causticPhase - 128) >> 3) : 0;

        // Height-based highlights (crests catch light)
        int heightLight = 0;
        if (height > 8) {
            heightLight = ((height - 8) * 3);
            if (heightLight > 100) heightLight = 100;
        }

        // Combine all layers
        int r = baseR + (specular >> 1) + foam;
        int g = baseG + specular + foam + caustic;
        int b = baseB + specular + foam + heightLight + (caustic >> 1);

        // Clamp values
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;

        return D3DCOLOR_XRGB(r, g, b);
    }

    // height-based color scheme
    static DWORD WaterColorFromHeight(int height)
    {
        int h = height + 128;
        if (h < 0) h = 0;
        if (h > 255) h = 255;

        int r = 0;
        int g = h;
        int b = 255 - h;

        return D3DCOLOR_XRGB(r, g, b);
    }
}

// ============================================================================
// Scene API
// ============================================================================
void DripScene_Init()
{
    ClearSim();

    const int cx = GRID_W - 1;
    const int cy = GRID_H - 1;

    g_triCount = cx * cy * 2;
    g_lineCount = cx * cy * 4;

    g_pd3dDevice->CreateIndexBuffer(
        g_triCount * 3 * sizeof(WORD),
        0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ibTri);

    WORD* tri;
    g_ibTri->Lock(0, 0, (BYTE**)&tri, 0);

    int k = 0;
    for (int y = 0; y < cy; ++y)
        for (int x = 0; x < cx; ++x)
        {
            WORD i0 = IDX(x, y);
            WORD i1 = IDX(x + 1, y);
            WORD i2 = IDX(x, y + 1);
            WORD i3 = IDX(x + 1, y + 1);

            tri[k++] = i0; tri[k++] = i2; tri[k++] = i1;
            tri[k++] = i1; tri[k++] = i2; tri[k++] = i3;
        }

    g_ibTri->Unlock();

    g_pd3dDevice->CreateIndexBuffer(
        g_lineCount * 2 * sizeof(WORD),
        0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &g_ibLine);

    WORD* line;
    g_ibLine->Lock(0, 0, (BYTE**)&line, 0);

    k = 0;
    for (int y = 0; y < cy; ++y)
        for (int x = 0; x < cx; ++x)
        {
            WORD i0 = IDX(x, y);
            WORD i1 = IDX(x + 1, y);
            WORD i2 = IDX(x, y + 1);
            WORD i3 = IDX(x + 1, y + 1);

            line[k++] = i0; line[k++] = i1;
            line[k++] = i1; line[k++] = i3;
            line[k++] = i3; line[k++] = i2;
            line[k++] = i2; line[k++] = i0;
        }

    g_ibLine->Unlock();

    // Main vertex buffer for solid rendering
    g_pd3dDevice->CreateVertexBuffer(
        GRID_W * GRID_H * sizeof(Vtx),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        FVF_VTX, D3DPOOL_DEFAULT, &g_vb);

    AddDrop(GRID_W / 2, GRID_H / 2, 7, -3600);
}

void DripScene_Shutdown()
{
    if (g_vb) g_vb->Release();
    if (g_ibTri) g_ibTri->Release();
    if (g_ibLine) g_ibLine->Release();
    g_vb = NULL;
    g_ibTri = NULL;
    g_ibLine = NULL;
}

void DripScene_Update()
{
    WORD buttons = GetButtons();

    // Y button - toggle rain
    if ((buttons & BTN_Y) && !(g_lastButtons & BTN_Y))
        g_rainEnabled = !g_rainEnabled;

    g_lastButtons = buttons;

    // Rain effect
    if (g_rainEnabled)
    {
        g_rainCounter++;
        if (g_rainCounter % 3 == 0)
        {
            DWORD r = LcgNext();
            AddDrop(r % GRID_W, (r >> 8) % GRID_H, 2, -1200);
        }
    }

    // Random drops
    DWORD r = LcgNext();
    if ((r & 31) == 0)
        AddDrop(r % GRID_W, (r >> 8) % GRID_H, 4, -2400);

    if ((r & 255) == 0)
        AddDrop(r % GRID_W, (r >> 16) % GRID_H, 7, -4200);

    for (int i = 0; i < STEPS_PER_FRAME; ++i)
        StepSimOnce();

    g_windPhase += WIND_SPEED;
}

void DripScene_Render()
{
    g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pd3dDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    const SHORT* h = (g_ping == 0) ? g_bufA : g_bufB;

    Vtx* v;
    g_vb->Lock(0, 0, (BYTE**)&v, 0);

    const int horizon = 0; // Start at top of screen instead of 1/5 down
    const int cx = SCREEN_W / 2;

    for (int y = 0; y < GRID_H; ++y)
    {
        int depth = y + 32;
        int scale = (256 * (GRID_H + 32)) / depth;
        int sy = horizon + ((y * SCREEN_H) / (GRID_H - 1)); // Use full screen height

        for (int x = 0; x < GRID_W; ++x)
        {
            int i = IDX(x, y);
            int lx = (x * SCREEN_W) / (GRID_W - 1);
            int sx = cx + (((lx - cx) * scale) >> 8);

            int height =
                (h[i] >> HEIGHT_SCALE) +
                (g_splash[i] >> SPLASH_SCALE);

            SHORT hL = (x > 0) ? h[i - 1] : h[i];
            SHORT hR = (x < GRID_W - 1) ? h[i + 1] : h[i];
            SHORT hU = (y > 0) ? h[i - GRID_W] : h[i];
            SHORT hD = (y < GRID_H - 1) ? h[i + GRID_W] : h[i];

            int slope = (hR - hL) + (hD - hU);

            v[i].x = (float)sx;
            v[i].y = (float)(sy - height);
            v[i].z = 0.0f;
            v[i].rhw = 1.0f;
            v[i].diffuse = WaterColorFromSlope(slope, height, x, y);
        }
    }

    g_vb->Unlock();

    g_pd3dDevice->SetVertexShader(FVF_VTX);
    g_pd3dDevice->SetStreamSource(0, g_vb, sizeof(Vtx));
    g_pd3dDevice->SetIndices(g_ibTri, 0);
    g_pd3dDevice->DrawIndexedPrimitive(
        D3DPT_TRIANGLELIST, 0, GRID_W * GRID_H, 0, g_triCount);
}