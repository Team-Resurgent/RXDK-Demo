// PlasmaScene.cpp - Fullscreen vertex-colored plasma (DX8 / NV2A)
// Swirly plasma field with camera drift (zoom + rotation).
// This version precomputes deformed vertices to avoid strip seams.

#include "PlasmaScene.h"

#include <xtl.h>
#include <math.h>

// Device provided by main.cpp (same as IntroScene)
extern LPDIRECT3DDEVICE8 g_pDevice;

// Match your main resolution
static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

// -----------------------------------------------------------------------------
// Vertex type
// -----------------------------------------------------------------------------

struct PlasmaVertex
{
    float x, y, z, rhw;
    DWORD color;
};

#define PLASMA_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// -----------------------------------------------------------------------------
// Grid setup
// -----------------------------------------------------------------------------

// Tweak these for more/less detail.
static const int GRID_X = 48;
static const int GRID_Y = 36;

// Base (unwarped) grid
static PlasmaVertex s_grid[GRID_Y][GRID_X];

// Deformed grid (wobble + camera applied once per vertex)
static PlasmaVertex s_deformed[GRID_Y][GRID_X];

// Strip buffer for one row pair
static PlasmaVertex s_strip[GRID_X * 2];

static bool s_plasmaActive = false;
static int  s_frameCount = 0;

// -----------------------------------------------------------------------------
// Palettes
// -----------------------------------------------------------------------------

static const DWORD s_paletteBlue[5] =
{
    D3DCOLOR_XRGB(0,   0,   20),
    D3DCOLOR_XRGB(10,  40,  100),
    D3DCOLOR_XRGB(30,  140, 220),
    D3DCOLOR_XRGB(120, 230, 255),
    D3DCOLOR_XRGB(255, 255, 255)
};

static const DWORD s_paletteMagenta[5] =
{
    D3DCOLOR_XRGB(10,   0,  20),
    D3DCOLOR_XRGB(80,   0,  80),
    D3DCOLOR_XRGB(200, 40, 160),
    D3DCOLOR_XRGB(255,120, 80),
    D3DCOLOR_XRGB(255,255,180)
};

static const DWORD s_paletteGreen[5] =
{
    D3DCOLOR_XRGB(0,   10,  0),
    D3DCOLOR_XRGB(0,   40,  30),
    D3DCOLOR_XRGB(40, 180, 80),
    D3DCOLOR_XRGB(180,255,120),
    D3DCOLOR_XRGB(255,255,255)
};

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void InitGridPositions()
{
    float dx = SCREEN_W / (float)(GRID_X - 1);
    float dy = SCREEN_H / (float)(GRID_Y - 1);

    for (int j = 0; j < GRID_Y; ++j)
    {
        float y = dy * (float)j;

        for (int i = 0; i < GRID_X; ++i)
        {
            float x = dx * (float)i;

            PlasmaVertex& v = s_grid[j][i];
            v.x = x;
            v.y = y;
            v.z = 0.0f;
            v.rhw = 1.0f;
            v.color = 0xFF000000;
        }
    }
}

// Update colors based on time & position.
static void UpdatePlasmaColors(float t, int palettePhase)
{
    const DWORD* pal;
    switch (palettePhase)
    {
    default:
    case 0: pal = s_paletteBlue;    break;
    case 1: pal = s_paletteMagenta; break;
    case 2: pal = s_paletteGreen;   break;
    }

    const float sx = 4.0f / (float)(GRID_X - 1);
    const float sy = 4.0f / (float)(GRID_Y - 1);

    for (int j = 0; j < GRID_Y; ++j)
    {
        float ny = (float)j * sy - 2.0f;

        for (int i = 0; i < GRID_X; ++i)
        {
            float nx = (float)i * sx - 2.0f;

            // Dense, chaotic demo-scene style plasma
            // Lots of high-frequency sine waves creating tight ripples
            float v =
                sinf(nx * 5.0f + t * 1.2f) +
                cosf(ny * 5.0f - t * 1.5f) +
                sinf((nx + ny) * 4.0f + t * 0.8f) +
                cosf((nx - ny) * 4.5f - t * 1.0f) +
                sinf(nx * 6.5f + ny * 3.5f + t * 1.3f) +
                cosf(nx * 3.0f - ny * 6.0f - t * 0.9f) +
                sinf(sqrtf(nx * nx + ny * ny) * 7.0f + t * 1.1f) +
                cosf(sqrtf((nx - 0.5f) * (nx - 0.5f) + (ny + 0.3f) * (ny + 0.3f)) * 6.0f - t * 1.4f) +
                sinf(sqrtf((nx + 0.7f) * (nx + 0.7f) + (ny - 0.6f) * (ny - 0.6f)) * 5.5f + t * 0.7f);

            // Add rotating wave patterns
            float angle = t * 0.5f;
            float rx1 = nx * cosf(angle) - ny * sinf(angle);
            float ry1 = nx * sinf(angle) + ny * cosf(angle);
            v += cosf(rx1 * 4.5f + ry1 * 3.5f + t * 0.6f);

            float angle2 = t * -0.7f + 1.5f;
            float rx2 = nx * cosf(angle2) - ny * sinf(angle2);
            float ry2 = nx * sinf(angle2) + ny * cosf(angle2);
            v += sinf(rx2 * 5.5f - ry2 * 4.0f - t * 0.8f);

            // Interference patterns
            v += sinf(nx * ny * 3.0f + t);
            v += cosf((nx + sinf(t * 0.3f)) * 7.0f);
            v += sinf((ny + cosf(t * 0.4f)) * 7.0f);
            v += cosf((nx * 3.0f + ny * 2.0f) * sinf(t * 0.2f) + t * 1.5f);

            // Smoother color bands (16 bands instead of 5 for less chunky look)
            int band;
            if (v > 2.625f)      band = 15;
            else if (v > 2.25f)  band = 14;
            else if (v > 1.875f) band = 13;
            else if (v > 1.5f)   band = 12;
            else if (v > 1.125f) band = 11;
            else if (v > 0.75f)  band = 10;
            else if (v > 0.375f) band = 9;
            else if (v > 0.0f)   band = 8;
            else if (v > -0.375f) band = 7;
            else if (v > -0.75f)  band = 6;
            else if (v > -1.125f) band = 5;
            else if (v > -1.5f)   band = 4;
            else if (v > -1.875f) band = 3;
            else if (v > -2.25f)  band = 2;
            else if (v > -2.625f) band = 1;
            else                  band = 0;

            // Map 16 bands to 5 palette colors with interpolation
            int palidx = band >> 2; // band / 4 = 0..3
            int subband = band & 3; // band % 4 = 0..3

            if (palidx > 3) palidx = 3;
            int palidx1 = palidx + 1;
            if (palidx1 > 4) palidx1 = 4;

            // Interpolate between palette colors
            DWORD c0 = pal[palidx];
            DWORD c1 = pal[palidx1];

            int red0 = (c0 >> 16) & 0xFF;
            int red1 = (c1 >> 16) & 0xFF;
            int grn0 = (c0 >> 8) & 0xFF;
            int grn1 = (c1 >> 8) & 0xFF;
            int blu0 = c0 & 0xFF;
            int blu1 = c1 & 0xFF;

            // subband is 0..3, convert to 0, 64, 128, 192 for blending
            int blend256 = subband << 6;

            int red = red0 + (((red1 - red0) * blend256) >> 8);
            int grn = grn0 + (((grn1 - grn0) * blend256) >> 8);
            int blu = blu0 + (((blu1 - blu0) * blend256) >> 8);

            s_grid[j][i].color = 0xFF000000 | (red << 16) | (grn << 8) | blu;
        }
    }
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void PlasmaScene_Init()
{
    if (!g_pDevice)
        return;

    s_plasmaActive = true;
    s_frameCount = 0;

    InitGridPositions();
}

void PlasmaScene_Shutdown()
{
    s_plasmaActive = false;
}

// This scene assumes main.cpp owns Clear / BeginScene / EndScene / Present.
void PlasmaScene_Render(float demoTime)
{
    (void)demoTime;

    if (!s_plasmaActive || !g_pDevice)
        return;

    s_frameCount++;

    float t = (float)s_frameCount * 0.06f;
    int palettePhase = (s_frameCount / 120) % 3;

    UpdatePlasmaColors(t, palettePhase);

    // Camera motion
    float zoom = 1.0f + 0.06f * sinf(t * 0.25f);
    float angle = 0.06f * sinf(t * 0.18f);
    float ca = cosf(angle);
    float sa = sinf(angle);

    const float cx = SCREEN_W * 0.5f;
    const float cy = SCREEN_H * 0.5f;

    // -------------------------------------------------------------------------
    // 1) Compute deformed vertices ONCE into s_deformed
    // -------------------------------------------------------------------------
    for (int j = 0; j < GRID_Y; ++j)
    {
        float ny = ((float)j / (float)(GRID_Y - 1)) * 2.0f - 1.0f;

        for (int i = 0; i < GRID_X; ++i)
        {
            const PlasmaVertex& src = s_grid[j][i];
            PlasmaVertex        v = src;

            float nx = ((float)i / (float)(GRID_X - 1)) * 2.0f - 1.0f;

            float phaseX = nx * 3.1f + sinf(t * 0.5f);
            float phaseY = ny * 2.7f + cosf(t * 0.37f);

            float wobbleY = sinf(phaseX + phaseY) * 4.0f;
            float wobbleX = cosf(phaseX - phaseY) * 3.0f;

            v.y += wobbleY;
            v.x += wobbleX;

            // Camera transform
            float tx = v.x - cx;
            float ty = v.y - cy;

            tx *= zoom;
            ty *= zoom;

            float rx = tx * ca - ty * sa;
            float ry = tx * sa + ty * ca;

            v.x = rx + cx;
            v.y = ry + cy;

            s_deformed[j][i] = v;
        }
    }

    // -------------------------------------------------------------------------
    // 2) Render using triangle strips built from s_deformed
    // -------------------------------------------------------------------------
    g_pDevice->SetVertexShader(PLASMA_FVF);
    g_pDevice->SetTexture(0, NULL);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    for (int j = 0; j < GRID_Y - 1; ++j)
    {
        int idx = 0;

        for (int i = 0; i < GRID_X; ++i)
        {
            s_strip[idx++] = s_deformed[j][i];
            s_strip[idx++] = s_deformed[j + 1][i];
        }

        g_pDevice->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            (GRID_X * 2) - 2,
            s_strip,
            sizeof(PlasmaVertex)
        );
    }
}