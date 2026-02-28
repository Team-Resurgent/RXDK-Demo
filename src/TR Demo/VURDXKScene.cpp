// VURXDKScene.cpp - Big wireframe RXDK letters (DX8, RXDK-safe)
// - Thick pseudo-3D wireframe letters filling viewport (isometric)
// - Each letter has a "VU fill" driven by Music_GetVULevels() that conforms to letter shape
// - VU fill covers front face AND visible iso side faces for full 3D fill effect
// - 2D (XYZRHW) only for stability
// - No float->int casts

#include "VURXDKScene.h"

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

static void DrawQuadFilled(float x0, float y0, float x1, float y1,
    float x2, float y2, float x3, float y3,
    DWORD c)
{
    if (!g_pDevice) return;
    // Two triangles: (0,1,2) and (1,3,2)
    Vtx2D q[4] =
    {
        { x0, y0, 0.0f, 1.0f, c },
        { x1, y1, 0.0f, 1.0f, c },
        { x2, y2, 0.0f, 1.0f, c },
        { x3, y3, 0.0f, 1.0f, c },
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

static void DrawRect(float x0, float y0, float x1, float y1, DWORD c)
{
    DrawQuadFilled(x0, y0, x1, y0, x0, y1, x1, y1, c);
}

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

struct Seg { V2 a, b; };

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

static DWORD TwinkleLite(DWORD c, int add)
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

// Darken a colour for the iso side face — gives depth contrast
static DWORD DarkenCol(DWORD c, float factor)
{
    BYTE a = (BYTE)(c >> 24);
    BYTE r = (BYTE)(c >> 16);
    BYTE g = (BYTE)(c >> 8);
    BYTE b = (BYTE)(c >> 0);
    unsigned rr = (unsigned)((float)r * factor); if (rr > 255u) rr = 255u;
    unsigned gg = (unsigned)((float)g * factor); if (gg > 255u) gg = 255u;
    unsigned bb = (unsigned)((float)b * factor); if (bb > 255u) bb = 255u;
    return D3DCOLOR_ARGB(a, (BYTE)rr, (BYTE)gg, (BYTE)bb);
}

static void IsoProject(float inX, float inY, float depth, float* outX, float* outY)
{
    const float dx = depth * 1.10f;
    const float dy = -depth * 0.75f;
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

        DrawLineThick(bx0, by0, bx1, by1, thick, colBack);
        DrawLineThick(fx0, fy0, fx1, fy1, thick, colFront);
        DrawLineThick(fx0, fy0, bx0, by0, thick, colEdge);
        DrawLineThick(fx1, fy1, bx1, by1, thick, colEdge);
    }
}

// Draw VU fill on front face AND visible iso side faces.
//
// Front face: same scanline approach as before — scan Y levels,
// find left/right edges of each segment, fill horizontal strips.
//
// Side faces: for each segment, extrude the front line to the back
// (iso offset) and fill the quad between them.  The fill is clipped
// to the same VU level.  Side face is darkened relative to front
// so the 3D read is clear.
static void DrawLetterFillConforming(const Seg* segs, int segCount,
    float x, float y, float w, float h,
    int level, DWORD baseCol,
    float depthPx)
{
    float fillPercent = ClampF((float)level * (1.0f / 255.0f), 0.0f, 1.0f);
    float fillTopY = 1.0f - fillPercent;  // normalised Y above which we don't fill

    // ── Front face fill (horizontal scanlines) ────────────────────────────────
    const int scanLines = 80;
    for (int scan = 0; scan < scanLines; ++scan)
    {
        float scanY = 1.0f - ((float)scan / (float)scanLines);
        if (scanY < fillTopY) continue;

        float minX = 1.0f, maxX = 0.0f;
        bool  found = false;

        for (int i = 0; i < segCount; ++i)
        {
            float y0 = segs[i].a.y, y1 = segs[i].b.y;
            float x0 = segs[i].a.x, x1 = segs[i].b.x;
            if ((y0 <= scanY && scanY <= y1) || (y1 <= scanY && scanY <= y0))
            {
                float t = (scanY - y0) / (y1 - y0 + 0.0001f);
                float xInt = x0 + t * (x1 - x0);
                if (xInt < minX) minX = xInt;
                if (xInt > maxX) maxX = xInt;
                found = true;
            }
        }

        if (found && maxX > minX)
        {
            float worldY = y + scanY * h;
            float worldX0 = x + minX * w;
            float worldX1 = x + maxX * w;
            float lineH = h / (float)scanLines;

            float brightness = 1.0f + (1.0f - scanY) * 0.6f;
            unsigned bu = (unsigned)(brightness * 80.0f);
            if (bu > 255u) bu = 255u;
            DWORD col = TwinkleLite(baseCol, (int)bu);

            DrawRect(worldX0, worldY, worldX1, worldY + lineH, col);
        }
    }

    // ── Iso side face fill ────────────────────────────────────────────────────
    // For each letter segment, the iso extrusion produces a visible quad face.
    // We fill it as a solid quad clipped to the VU level.
    //
    // The visible side is the top/right face (iso offset is up-right).
    // We clip: any part of the segment above fillTopY is not filled.
    // The colour is darkened (~65%) relative to the front so it reads as
    // a different plane — classic isometric shading convention.
    DWORD sideCol = DarkenCol(baseCol, 0.55f);

    for (int i = 0; i < segCount; ++i)
    {
        float na0 = segs[i].a.x, nb0 = segs[i].a.y;
        float na1 = segs[i].b.x, nb1 = segs[i].b.y;

        // Clip segment to fill region: only the portion where Y >= fillTopY
        // (in normalised letter coords Y increases downward, fill rises upward)
        float ya = nb0, yb = nb1;

        // Clamp each endpoint to fillTopY — don't draw above the fill line
        float cya = ya < fillTopY ? fillTopY : ya;
        float cyb = yb < fillTopY ? fillTopY : yb;

        // If both endpoints are fully above fill, skip
        if (cya >= 1.0f && cyb >= 1.0f) continue;
        // If both are clipped to top, skip
        if (cya <= fillTopY && cyb <= fillTopY) continue;

        // Interpolate X for clipped Y positions (in case we clipped one end)
        float dyTotal = yb - ya;
        float xa, xb;
        if (dyTotal < 0.0001f && dyTotal > -0.0001f)
        {
            xa = na0; xb = na1;
        }
        else
        {
            float ta = (cya - ya) / dyTotal;
            float tb = (cyb - ya) / dyTotal;
            xa = na0 + ta * (na1 - na0);
            xb = na0 + tb * (na1 - na0);
        }

        // Front face corners of the clipped segment (world space)
        float fx0 = x + xa * w, fy0 = y + cya * h;
        float fx1 = x + xb * w, fy1 = y + cyb * h;

        // Back face corners (iso projected)
        float bx0, by0, bx1, by1;
        IsoProject(fx0, fy0, depthPx, &bx0, &by0);
        IsoProject(fx1, fy1, depthPx, &bx1, &by1);

        // Draw filled quad: front0, back0, front1, back1
        // (triangle strip order)
        DrawQuadFilled(fx0, fy0, bx0, by0, fx1, fy1, bx1, by1, sideCol);
    }

    // ── Top cap fill ──────────────────────────────────────────────────────────
    // At the fill waterline (fillTopY) draw a horizontal quad that caps the
    // extrusion — front edge to iso back edge.  Without this the letters look
    // hollow when viewed from above in isometric.
    // Colour is brightened slightly relative to the front (top faces catch more
    // light in classic isometric convention).
    if (fillPercent > 0.01f)
    {
        DWORD topCol = DarkenCol(baseCol, 0.80f);  // slightly brighter than side

        // Find left/right front-face X edges at fillTopY (same as scanline logic)
        float minX = 1.0f, maxX = 0.0f;
        bool  found = false;

        for (int i = 0; i < segCount; ++i)
        {
            float y0 = segs[i].a.y, y1 = segs[i].b.y;
            float x0 = segs[i].a.x, x1 = segs[i].b.x;
            float scanY = fillTopY;
            if ((y0 <= scanY && scanY <= y1) || (y1 <= scanY && scanY <= y0))
            {
                float t = (scanY - y0) / (y1 - y0 + 0.0001f);
                float xInt = x0 + t * (x1 - x0);
                if (xInt < minX) minX = xInt;
                if (xInt > maxX) maxX = xInt;
                found = true;
            }
        }

        // Also include segment endpoints that land exactly at fillTopY
        // by scanning one strip above (prevents gaps at sharp corners)
        float scanAbove = fillTopY - (1.0f / (float)scanLines);
        for (int i = 0; i < segCount; ++i)
        {
            float y0 = segs[i].a.y, y1 = segs[i].b.y;
            float x0 = segs[i].a.x, x1 = segs[i].b.x;
            if ((y0 <= scanAbove && scanAbove <= y1) || (y1 <= scanAbove && scanAbove <= y0))
            {
                float t = (scanAbove - y0) / (y1 - y0 + 0.0001f);
                float xInt = x0 + t * (x1 - x0);
                if (xInt < minX) minX = xInt;
                if (xInt > maxX) maxX = xInt;
                found = true;
            }
        }

        if (found && maxX > minX)
        {
            // Front edge of the cap (at fillTopY world Y)
            float worldY = y + fillTopY * h;
            float fx0 = x + minX * w;
            float fx1 = x + maxX * w;

            // Back edge (iso projected from front edge)
            float bx0, by0, bx1, by1;
            IsoProject(fx0, worldY, depthPx, &bx0, &by0);
            IsoProject(fx1, worldY, depthPx, &bx1, &by1);

            // Quad: front-left, front-right, back-left, back-right
            DrawQuadFilled(fx0, worldY, fx1, worldY, bx0, by0, bx1, by1, topCol);
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

void VURXDKScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();
}

void VURXDKScene_Shutdown()
{
    s_active = false;
}

bool VURXDKScene_IsFinished()
{
    if (!s_active) return true;
    return (TimeMs() >= SCENE_DURATION_MS);
}

void VURXDKScene_Render(float)
{
    if (!s_active || !g_pDevice) return;

    int vu[4] = { 0,0,0,0 };
    Music_GetVULevels(vu);

    SetupFrameStates();
    DrawRect(0.0f, 0.0f, SCREEN_W, SCREEN_H, D3DCOLOR_XRGB(0, 0, 0));

    const float marginX = 18.0f;
    const float topY = 38.0f;
    const float letterH = 400.0f;
    const float gap = 10.0f;
    const float totalW = SCREEN_W - marginX * 2.0f;
    const float letterW = (totalW - gap * 3.0f) / 4.0f;
    const float thick = 7.0f;
    const float depth = 32.0f;

    const int lvlR = vu[0];
    const int lvlX = vu[1];
    const int lvlD = vu[2];
    const int lvlK = vu[3];

    DWORD fillR = D3DCOLOR_ARGB(135, 70, 165, 255);
    DWORD fillX = D3DCOLOR_ARGB(135, 80, 255, 180);
    DWORD fillD = D3DCOLOR_ARGB(135, 255, 140, 90);
    DWORD fillK = D3DCOLOR_ARGB(135, 255, 220, 110);

    DWORD wireFront = D3DCOLOR_ARGB(235, 235, 245, 255);
    DWORD wireBack = D3DCOLOR_ARGB(120, 80, 110, 160);
    DWORD wireEdge = D3DCOLOR_ARGB(200, 140, 190, 255);

    float x = marginX;

    // Fill drawn first (behind wireframe), wire drawn on top
    DrawLetterFillConforming(LETTER_R, (int)(sizeof(LETTER_R) / sizeof(LETTER_R[0])), x, topY, letterW, letterH, lvlR, fillR, depth);
    DrawLetterWireIso(LETTER_R, (int)(sizeof(LETTER_R) / sizeof(LETTER_R[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);
    x += letterW + gap;

    DrawLetterFillConforming(LETTER_X, (int)(sizeof(LETTER_X) / sizeof(LETTER_X[0])), x, topY, letterW, letterH, lvlX, fillX, depth);
    DrawLetterWireIso(LETTER_X, (int)(sizeof(LETTER_X) / sizeof(LETTER_X[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);
    x += letterW + gap;

    DrawLetterFillConforming(LETTER_D, (int)(sizeof(LETTER_D) / sizeof(LETTER_D[0])), x, topY, letterW, letterH, lvlD, fillD, depth);
    DrawLetterWireIso(LETTER_D, (int)(sizeof(LETTER_D) / sizeof(LETTER_D[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);
    x += letterW + gap;

    DrawLetterFillConforming(LETTER_K, (int)(sizeof(LETTER_K) / sizeof(LETTER_K[0])), x, topY, letterW, letterH, lvlK, fillK, depth);
    DrawLetterWireIso(LETTER_K, (int)(sizeof(LETTER_K) / sizeof(LETTER_K[0])), x, topY, letterW, letterH, thick, depth, wireFront, wireBack, wireEdge);

    DrawRect(0.0f, SCREEN_H - 18.0f, SCREEN_W, SCREEN_H, D3DCOLOR_ARGB(85, 70, 140, 255));
}