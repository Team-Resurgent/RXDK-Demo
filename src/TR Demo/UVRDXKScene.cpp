// UVRXDKScene.cpp - Big wireframe RXDK letters (DX8, RXDK-safe)
// - Thick pseudo-3D wireframe letters filling viewport (isometric)
// - Each letter has a "UV fill" driven by Music_GetUVLevels() that conforms to letter shape
// - 2D (XYZRHW) only for stability
// - No float->int casts

#include "UVRXDKScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <string.h>

#include "music.h"

extern LPDIRECT3DDEVICE8 g_pDevice;

static bool  s_active = false;
static DWORD s_startTicks = 0;

static const DWORD SCENE_DURATION_MS = 22000;

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

struct V2 { float x, y; };

struct Vtx2D
{
    float x, y, z, rhw;
    DWORD c;
};
#define FVF_2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static DWORD TimeMs() { return GetTickCount() - s_startTicks; }

static float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

// Draw a filled rect (solid color)
static void DrawRect(float x0, float y0, float x1, float y1, DWORD c)
{
    if (!g_pDevice) return;

    Vtx2D q[4] =
    {
        { x0, y0, 0.0f, 1.0f, c },
        { x1, y0, 0.0f, 1.0f, c },
        { x0, y1, 0.0f, 1.0f, c },
        { x1, y1, 0.0f, 1.0f, c },
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(FVF_2D);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx2D));
}

// Draw a thick line as a thin quad (screen space)
static void DrawLineThick(float x0, float y0, float x1, float y1, float thickness, DWORD c)
{
    if (!g_pDevice) return;

    float dx = x1 - x0;
    float dy = y1 - y0;

    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.0001f) return;

    float nx = -dy / len;
    float ny = dx / len;

    float t = thickness * 0.5f;
    float ox = nx * t;
    float oy = ny * t;

    Vtx2D q[4] =
    {
        { x0 - ox, y0 - oy, 0.0f, 1.0f, c },
        { x1 - ox, y1 - oy, 0.0f, 1.0f, c },
        { x0 + ox, y0 + oy, 0.0f, 1.0f, c },
        { x1 + ox, y1 + oy, 0.0f, 1.0f, c },
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(FVF_2D);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vtx2D));
}

// Letter defined as a set of line segments in normalized 0..1 box
struct Seg { V2 a, b; };

// Blocky wireframe letters (RXDK)
static const Seg LETTER_R[] =
{
    {{0.12f,0.10f},{0.12f,0.90f}},
    {{0.12f,0.10f},{0.72f,0.10f}},
    {{0.72f,0.10f},{0.72f,0.50f}},
    {{0.72f,0.50f},{0.12f,0.50f}},
    {{0.12f,0.50f},{0.80f,0.90f}},
};
static const Seg LETTER_X[] =
{
    {{0.12f,0.10f},{0.88f,0.90f}},
    {{0.88f,0.10f},{0.12f,0.90f}},
};
static const Seg LETTER_D[] =
{
    {{0.12f,0.10f},{0.12f,0.90f}},
    {{0.12f,0.10f},{0.68f,0.10f}},
    {{0.68f,0.10f},{0.88f,0.30f}},
    {{0.88f,0.30f},{0.88f,0.70f}},
    {{0.88f,0.70f},{0.68f,0.90f}},
    {{0.68f,0.90f},{0.12f,0.90f}},
};
static const Seg LETTER_K[] =
{
    {{0.16f,0.10f},{0.16f,0.90f}},
    {{0.86f,0.10f},{0.16f,0.52f}},
    {{0.86f,0.90f},{0.16f,0.52f}},
};

static DWORD TwinkleLite(DWORD c, int add /*0..255*/)
{
    BYTE a = (BYTE)(c >> 24);
    BYTE r = (BYTE)(c >> 16);
    BYTE g = (BYTE)(c >> 8);
    BYTE b = (BYTE)(c >> 0);

    unsigned mul = 180u + (unsigned)(add & 255);
    unsigned rr = ((unsigned)r * mul) >> 8; if (rr > 255u) rr = 255u;
    unsigned gg = ((unsigned)g * mul) >> 8; if (gg > 255u) gg = 255u;
    unsigned bb = ((unsigned)b * mul) >> 8; if (bb > 255u) bb = 255u;

    return D3DCOLOR_ARGB(a, (BYTE)rr, (BYTE)gg, (BYTE)bb);
}

// Enhanced isometric projection (more pronounced depth)
static void IsoProject(float inX, float inY, float depth, float* outX, float* outY)
{
    const float dx = depth * 1.10f;  // increased from 0.70f
    const float dy = -depth * 0.75f; // increased from 0.45f
    *outX = inX + dx;
    *outY = inY + dy;
}

static void DrawLetterWireIso(const Seg* segs, int segCount,
    float x, float y, float w, float h,
    float thick, float depthPx,
    DWORD colFront, DWORD colBack, DWORD colEdge)
{
    for (int i = 0; i < segCount; ++i)
    {
        float fx0 = x + segs[i].a.x * w;
        float fy0 = y + segs[i].a.y * h;
        float fx1 = x + segs[i].b.x * w;
        float fy1 = y + segs[i].b.y * h;

        float bx0, by0, bx1, by1;
        IsoProject(fx0, fy0, depthPx, &bx0, &by0);
        IsoProject(fx1, fy1, depthPx, &bx1, &by1);

        // back (dim)
        DrawLineThick(bx0, by0, bx1, by1, thick, colBack);
        // front (bright)
        DrawLineThick(fx0, fy0, fx1, fy1, thick, colFront);
        // connectors (edges)
        DrawLineThick(fx0, fy0, bx0, by0, thick, colEdge);
        DrawLineThick(fx1, fy1, bx1, by1, thick, colEdge);
    }
}

// VU fill that conforms to letter shape (scans horizontally at each Y level)
static void DrawLetterFillConforming(const Seg* segs, int segCount,
    float x, float y, float w, float h,
    int level /*0..255*/, DWORD baseCol)
{
    float fillPercent = (float)level * (1.0f / 255.0f);
    fillPercent = ClampF(fillPercent, 0.0f, 1.0f);

    // Scan from bottom to fillPercent height (fixed count, no float->int)
    const int scanLines = 80;

    for (int scan = 0; scan < scanLines; ++scan)
    {
        float scanY = 1.0f - ((float)scan / (float)scanLines); // 1.0 to 0.0 (bottom to top)

        // Only fill up to fillPercent
        if (scanY < (1.0f - fillPercent))
            continue;

        // Find left and right edges at this Y by checking all segments
        float minX = 1.0f;
        float maxX = 0.0f;
        bool foundEdge = false;

        for (int i = 0; i < segCount; ++i)
        {
            float y0 = segs[i].a.y;
            float y1 = segs[i].b.y;
            float x0 = segs[i].a.x;
            float x1 = segs[i].b.x;

            // Check if this segment crosses scanY
            if ((y0 <= scanY && scanY <= y1) || (y1 <= scanY && scanY <= y0))
            {
                // Interpolate X at scanY
                float t = (scanY - y0) / (y1 - y0 + 0.0001f);
                float xInt = x0 + t * (x1 - x0);

                if (xInt < minX) minX = xInt;
                if (xInt > maxX) maxX = xInt;
                foundEdge = true;
            }
        }

        if (foundEdge && maxX > minX)
        {
            float worldY = y + scanY * h;
            float worldX0 = x + minX * w;
            float worldX1 = x + maxX * w;
            float lineHeight = h / (float)scanLines;

            // Brighten toward top (NO float->int cast)
            float brightness = 1.0f + (1.0f - scanY) * 0.6f;
            unsigned brightnessU = (unsigned)(brightness * 80.0f); // intermediate as unsigned
            if (brightnessU > 255u) brightnessU = 255u;
            DWORD scanCol = TwinkleLite(baseCol, (int)brightnessU); // cast unsigned to int (safe)

            DrawRect(worldX0, worldY, worldX1, worldY + lineHeight, scanCol);
        }
    }
}

static void SetupFrameStates()
{
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetTexture(0, NULL);
}

void UVRXDKScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();
}

void UVRXDKScene_Shutdown()
{
    s_active = false;
}

bool UVRXDKScene_IsFinished()
{
    if (!s_active) return true;
    return (TimeMs() >= SCENE_DURATION_MS);
}

void UVRXDKScene_Render(float)
{
    if (!s_active || !g_pDevice)
        return;

    int uv[4] = { 0,0,0,0 };
    Music_GetUVLevels(uv);

    SetupFrameStates();
    DrawRect(0.0f, 0.0f, SCREEN_W, SCREEN_H, D3DCOLOR_XRGB(0, 0, 0));

    // Layout: RXDK only (4 letters), big and centered
    const float marginX = 18.0f;
    const float topY = 38.0f;
    const float letterH = 400.0f;
    const float gap = 10.0f;

    const float totalW = SCREEN_W - marginX * 2.0f;
    const float letterW = (totalW - gap * 3.0f) / 4.0f;

    const float thick = 7.0f;
    const float depth = 32.0f; // increased depth for more pronounced isometric effect

    // Bands -> letters:
    // R=low, X=mid, D=high, K=overall
    const int lvlR = uv[0];
    const int lvlX = uv[1];
    const int lvlD = uv[2];
    const int lvlK = uv[3];

    // Fill colors (ARGB)
    DWORD fillR = D3DCOLOR_ARGB(135, 70, 165, 255);
    DWORD fillX = D3DCOLOR_ARGB(135, 80, 255, 180);
    DWORD fillD = D3DCOLOR_ARGB(135, 255, 140, 90);
    DWORD fillK = D3DCOLOR_ARGB(135, 255, 220, 110);

    DWORD wireFront = D3DCOLOR_ARGB(235, 235, 245, 255);
    DWORD wireBack = D3DCOLOR_ARGB(120, 80, 110, 160);
    DWORD wireEdge = D3DCOLOR_ARGB(200, 140, 190, 255);

    float x = marginX;

    // R - conforming fill
    DrawLetterFillConforming(LETTER_R, (int)(sizeof(LETTER_R) / sizeof(LETTER_R[0])), x, topY, letterW, letterH, lvlR, fillR);
    DrawLetterWireIso(LETTER_R, (int)(sizeof(LETTER_R) / sizeof(LETTER_R[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);
    x += letterW + gap;

    // X - conforming fill
    DrawLetterFillConforming(LETTER_X, (int)(sizeof(LETTER_X) / sizeof(LETTER_X[0])), x, topY, letterW, letterH, lvlX, fillX);
    DrawLetterWireIso(LETTER_X, (int)(sizeof(LETTER_X) / sizeof(LETTER_X[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);
    x += letterW + gap;

    // D - conforming fill
    DrawLetterFillConforming(LETTER_D, (int)(sizeof(LETTER_D) / sizeof(LETTER_D[0])), x, topY, letterW, letterH, lvlD, fillD);
    DrawLetterWireIso(LETTER_D, (int)(sizeof(LETTER_D) / sizeof(LETTER_D[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);
    x += letterW + gap;

    // K - conforming fill
    DrawLetterFillConforming(LETTER_K, (int)(sizeof(LETTER_K) / sizeof(LETTER_K[0])), x, topY, letterW, letterH, lvlK, fillK);
    DrawLetterWireIso(LETTER_K, (int)(sizeof(LETTER_K) / sizeof(LETTER_K[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);

    // Baseline glow strip
    DrawRect(0.0f, SCREEN_H - 18.0f, SCREEN_W, SCREEN_H, D3DCOLOR_ARGB(85, 70, 140, 255));
}