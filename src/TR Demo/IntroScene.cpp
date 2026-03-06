// =============================================================================
// IntroScene.cpp
// Electronic / Tech Intro Scene | RXDK / NV2A Fixed-Function
// =============================================================================
//
// OVERVIEW
//   Electronic-themed intro sequence. Circuit board background with scrolling
//   traces and pulsing nodes, glitch effect on logo textures, typewriter-style
//   text reveal. Total runtime ~15-20 seconds at 60fps.
//
// PHASES (~60fps assumed)
//   Phase 1 - PRESENTED_BY : "Presented By:" typewriter + "Darkone83" reveal
//   Phase 2 - LOGO_TR      : TR logo with DOT3-style color glitch passes
//   Phase 3 - SUPPORT_XBS  : "Proudly Supporting:" + XBS logo with glitch
//   Total runtime: ~17 seconds
//
// BACKGROUND
//   Procedural PCB: dark FR4 green substrate, copper H/V/45deg traces with
//   varying widths (power=3px, signal=1.5px), traveling signal pulse on each
//   trace, 15 vias as filled octagons with copper rings, 5 IC footprints
//   (DIP outlines with pad rows), 8 diagonal 45deg trace stubs at junctions.
//   All geometry pulsed with sine brightness -- no textures required.
//
// EFFECTS
//   Typewriter text: characters revealed one per N frames, cursor blink
//   Logo glitch: UV coordinate distortion -- 16 horizontal slices, each gets
//     independent UV X shift + V compress during burst -> VHS scanline tear
//     alpha flicker on random slices during glitch burst
//   Signal pulse: bright green-white dot travels along each trace segment
//
// PIPELINE CONSTRAINTS
//   Fixed-function FVF only -- no vertex or pixel shaders
//   No __ftol2_sse -- all float->int via explicit cast helpers
//   Frame-based timing -- demoTime float ignored, s_frameCount used
//   All geometry via DrawPrimitiveUP -- no persistent VBs in intro
//
// TEXTURES
//   D:\tex\tr.dds  -- Logo texture (square, power-of-two, A8R8G8B8)
//   D:\tex\xbs.dds -- XBS logo texture (square, power-of-two, A8R8G8B8)
//
// =============================================================================

#include "IntroScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"

extern LPDIRECT3DDEVICE8 g_pDevice;

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;
static const float PI = 3.14159265f;

// -----------------------------------------------------------------------------
// Vertex types
// -----------------------------------------------------------------------------

struct IntroVertex
{
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
#define INTRO_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

struct ColorVertex
{
    float x, y, z, rhw;
    DWORD color;
};
#define COLOR_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// -----------------------------------------------------------------------------
// Helpers -- no float->int cast issues
// -----------------------------------------------------------------------------

// Inline asm float->int -- avoids __ftol2_sse linker error on MSVC/Xbox
static __declspec(noinline) int Ftoi(float f)
{
    int i;
    __asm {
        fld   f
        fistp i
    }
    return i;
}

static int ClampI(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// -----------------------------------------------------------------------------
// DDS loader (unchanged from original -- proven working)
// -----------------------------------------------------------------------------

#pragma pack(push, 1)
struct DDS_PIXELFORMAT { DWORD size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask; };
struct DDS_HEADER
{
    DWORD size, flags, height, width, pitchOrLinearSize, depth, mipMapCount;
    DWORD reserved1[11];
    DDS_PIXELFORMAT ddspf;
    DWORD caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

static LPDIRECT3DTEXTURE8 LoadTextureFromDDS(const char* path, int& outW, int& outH)
{
    outW = 0; outH = 0;
    if (!g_pDevice || !path) return NULL;

    HANDLE hFile = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return NULL;

    DWORD bytesRead = 0;
    DWORD magic = 0;
    if (!ReadFile(hFile, &magic, sizeof(DWORD), &bytesRead, NULL) ||
        bytesRead != sizeof(DWORD) || magic != 0x20534444)
    {
        CloseHandle(hFile); return NULL;
    }

    DDS_HEADER hdr;
    if (!ReadFile(hFile, &hdr, sizeof(DDS_HEADER), &bytesRead, NULL) ||
        bytesRead != sizeof(DDS_HEADER))
    {
        CloseHandle(hFile); return NULL;
    }

    if (hdr.size != 124 || hdr.ddspf.size != 32)
    {
        CloseHandle(hFile); return NULL;
    }

    if (hdr.ddspf.flags & 0x4) { CloseHandle(hFile); return NULL; } // no FOURCC
    if (hdr.ddspf.rgbBitCount != 32 ||
        hdr.ddspf.rMask != 0x00FF0000 || hdr.ddspf.gMask != 0x0000FF00 ||
        hdr.ddspf.bMask != 0x000000FF || hdr.ddspf.aMask != 0xFF000000)
    {
        CloseHandle(hFile); return NULL;
    }

    int w = (int)hdr.width, h = (int)hdr.height;
    if (w <= 0 || h <= 0 || w != h || (w & (w - 1)) != 0)
    {
        CloseHandle(hFile); return NULL;
    }

    DWORD pixelBytes = (DWORD)(w * h * 4);
    BYTE* pixels = (BYTE*)malloc(pixelBytes);
    if (!pixels) { CloseHandle(hFile); return NULL; }

    if (!ReadFile(hFile, pixels, pixelBytes, &bytesRead, NULL) || bytesRead != pixelBytes)
    {
        free(pixels); CloseHandle(hFile); return NULL;
    }
    CloseHandle(hFile);

    LPDIRECT3DTEXTURE8 tex = NULL;
    if (FAILED(g_pDevice->CreateTexture((UINT)w, (UINT)h, 1, 0, D3DFMT_A8R8G8B8, 0, &tex)))
    {
        free(pixels); return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release(); free(pixels); return NULL;
    }

    XGSwizzleRect(pixels, w * 4, NULL, lr.pBits, w, h, NULL, 4);
    tex->UnlockRect(0);
    free(pixels);
    outW = w; outH = h;
    return tex;
}

// -----------------------------------------------------------------------------
// Scene state
// -----------------------------------------------------------------------------

static bool               s_introActive = false;
static LPDIRECT3DTEXTURE8 s_logoTex = NULL;
static int                s_logoW = 0;
static int                s_logoH = 0;
static LPDIRECT3DTEXTURE8 s_xbsTex = NULL;
static int                s_xbsW = 0;
static int                s_xbsH = 0;
static int                s_frameCount = 0;

enum IntroPhase
{
    PHASE_PRESENTED = 0,
    PHASE_LOGO_TR,
    PHASE_MUSIC_BY,
    PHASE_SUPPORT_XBS,
    PHASE_DONE
};
static IntroPhase s_phase = PHASE_PRESENTED;
static int        s_phaseFrame = 0;

// Typewriter state
static int  s_twChars = 0;   // characters revealed so far
static int  s_twLine2 = 0;   // characters on second line
static bool s_cursorBlink = true;

// =============================================================================
// CIRCUIT BOARD BACKGROUND
// =============================================================================

// Deterministic pseudo-random from seed
static int PseudoRand(int seed)
{
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed ^= seed >> 4;
    seed *= 0x27d4eb2d;
    seed ^= seed >> 15;
    return seed & 0x7fffffff;
}

// =============================================================================
// PCB CIRCUIT BOARD BACKGROUND
// Dark green substrate, copper traces (H/V/45deg), vias, IC footprints,
// silkscreen outlines, traveling signal pulse along traces
// =============================================================================

// =============================================================================
// PCB HELPERS
// =============================================================================

// Draw a filled thick line segment as a quad (for trace width)
static void DrawThickLine(float x1, float y1, float x2, float y2,
    float width, DWORD col)
{
    float dx = x2 - x1, dy = y2 - y1;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float nx = -dy / len * width * 0.5f;
    float ny = dx / len * width * 0.5f;

    ColorVertex v[4] = {
        { x1 + nx - 0.5f, y1 + ny - 0.5f, 0.f,1.f, col },
        { x1 - nx - 0.5f, y1 - ny - 0.5f, 0.f,1.f, col },
        { x2 + nx - 0.5f, y2 + ny - 0.5f, 0.f,1.f, col },
        { x2 - nx - 0.5f, y2 - ny - 0.5f, 0.f,1.f, col },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(ColorVertex));
}

// Draw a PCB trace route: sequence of waypoints connected by 45-deg chamfered
// corners.  pts = flat array {x0,y0, x1,y1, ...}, nPts = number of points.
// At each interior corner a 45-deg diagonal stub replaces the sharp 90-deg turn.
static void DrawPCBTrace(const float* pts, int nPts, float width, DWORD col)
{
    if (nPts < 2) return;
    const float CHAMFER = width * 3.0f;   // chamfer leg length

    for (int i = 0; i < nPts - 1; ++i)
    {
        float ax = pts[i * 2], ay = pts[i * 2 + 1];
        float bx = pts[i * 2 + 2], by = pts[i * 2 + 3];

        // Shorten segment ends at interior corners to make room for chamfer
        float dx = bx - ax, dy = by - ay;
        float len = sqrtf(dx * dx + dy * dy);
        if (len < 1.f) continue;

        float sx = dx / len, sy = dy / len;

        // Pull back start if previous segment exists (interior corner)
        float startX = ax, startY = ay;
        if (i > 0)
        {
            startX = ax + sx * CHAMFER;
            startY = ay + sy * CHAMFER;
        }
        // Pull back end if next segment exists
        float endX = bx, endY = by;
        if (i < nPts - 2)
        {
            endX = bx - sx * CHAMFER;
            endY = by - sy * CHAMFER;
        }

        DrawThickLine(startX, startY, endX, endY, width, col);

        // Draw chamfer diagonal at end corner (connecting this seg end to next seg start)
        if (i < nPts - 2)
        {
            float cx2 = pts[(i + 1) * 2], cy2 = pts[(i + 1) * 2 + 1];
            float nx2 = pts[(i + 2) * 2], ny2 = pts[(i + 2) * 2 + 1];
            float dx2 = nx2 - cx2, dy2 = ny2 - cy2;
            float len2 = sqrtf(dx2 * dx2 + dy2 * dy2);
            if (len2 > 0.5f)
            {
                float sx2 = dx2 / len2, sy2 = dy2 / len2;
                float chamferStartX = cx2 + sx2 * CHAMFER;
                float chamferStartY = cy2 + sy2 * CHAMFER;
                DrawThickLine(endX, endY, chamferStartX, chamferStartY, width, col);
            }
        }
    }
}

// Draw a serpentine (accordion) trace -- length-matching pattern
// cx,cy = center start, dir=0 horizontal dir=1 vertical
// amplitude = peak-to-peak half-width, period = one full zigzag length
// count = number of full periods
static void DrawSerpentine(float startX, float startY,
    float runLen, float amplitude, float period,
    float width, DWORD col, int vertical)
{
    const int STEPS = 6;   // segments per half-period (straight+diagonal+straight)
    float halfP = period * 0.5f;
    float seg = halfP / (float)STEPS;
    float cx = startX, cy = startY;
    int dir = 1;

    // Number of half-periods that fit in runLen
    int nHalf = Ftoi(runLen / halfP);
    if (nHalf < 1) nHalf = 1;

    for (int h = 0; h < nHalf; ++h)
    {
        float amp = amplitude * (float)dir;
        // Each half-period: straight run, diagonal out, straight, diagonal back, straight
        float pts[10];
        if (!vertical)
        {
            // Horizontal main direction, amplitude in Y
            float x0 = cx, y0 = cy;
            float x1 = cx + seg, y1 = cy;
            float x2 = cx + seg * 2.f, y2 = cy + amp;
            float x3 = cx + seg * 4.f, y3 = cy + amp;
            float x4 = cx + seg * 5.f, y4 = cy;
            float x5 = cx + halfP, y5 = cy;
            pts[0] = x0; pts[1] = y0; pts[2] = x1; pts[3] = y1;
            pts[4] = x2; pts[5] = y2; pts[6] = x3; pts[7] = y3;
            pts[8] = x4; pts[9] = y4;
            DrawPCBTrace(pts, 5, width, col);
            DrawThickLine(x4, y4, x5, y5, width, col);
            cx += halfP;
        }
        else
        {
            // Vertical main direction, amplitude in X
            float x0 = cx, y0 = cy;
            float x1 = cx, y1 = cy + seg;
            float x2 = cx + amp, y2 = cy + seg * 2.f;
            float x3 = cx + amp, y3 = cy + seg * 4.f;
            float x4 = cx, y4 = cy + seg * 5.f;
            float x5 = cx, y5 = cy + halfP;
            pts[0] = x0; pts[1] = y0; pts[2] = x1; pts[3] = y1;
            pts[4] = x2; pts[5] = y2; pts[6] = x3; pts[7] = y3;
            pts[8] = x4; pts[9] = y4;
            DrawPCBTrace(pts, 5, width, col);
            DrawThickLine(x4, y4, x5, y5, width, col);
            cy += halfP;
        }
        dir = -dir;
    }
}

// Via -- filled octagon + ring
static void DrawVia(float cx, float cy, float radius, float bright, float alpha)
{
    int   a = ClampI(Ftoi(alpha * 220.f), 0, 220);
    int   br = ClampI(Ftoi(bright * 180.f), 20, 180);
    DWORD fillC = D3DCOLOR_ARGB(a, 0, br, br / 2);
    DWORD ringC = D3DCOLOR_ARGB(a, br / 2, br, br / 3);

    const int SIDES = 8;
    ColorVertex fan[SIDES + 2];
    fan[0].x = cx - 0.5f; fan[0].y = cy - 0.5f; fan[0].z = 0.f; fan[0].rhw = 1.f; fan[0].color = fillC;
    for (int k = 0; k <= SIDES; ++k) {
        float a2 = (float)k / (float)SIDES * 6.28318f;
        fan[k + 1].x = cx + cosf(a2) * radius - 0.5f; fan[k + 1].y = cy + sinf(a2) * radius - 0.5f;
        fan[k + 1].z = 0.f; fan[k + 1].rhw = 1.f; fan[k + 1].color = fillC;
    }
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SIDES, fan, sizeof(ColorVertex));

    ColorVertex ring[SIDES + 1];
    float ringR = radius + 2.5f;
    for (int k = 0; k <= SIDES; ++k) {
        float a2 = (float)k / (float)SIDES * 6.28318f;
        ring[k].x = cx + cosf(a2) * ringR - 0.5f; ring[k].y = cy + sinf(a2) * ringR - 0.5f;
        ring[k].z = 0.f; ring[k].rhw = 1.f; ring[k].color = ringC;
    }
    g_pDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, SIDES, ring, sizeof(ColorVertex));
}

// IC footprint -- DIP outline with pad rows
static void DrawICFootprint(float cx, float cy, float w, float h,
    int pins, float bright, float alpha)
{
    if (!g_pDevice) return;
    int   a = ClampI(Ftoi(alpha * 200.f), 0, 200);
    int   br = ClampI(Ftoi(bright * 160.f), 20, 160);
    DWORD col = D3DCOLOR_ARGB(a, 0, br, br / 3);

    ColorVertex outline[5] = {
        { cx - w * 0.5f - 0.5f, cy - h * 0.5f - 0.5f, 0.f,1.f,col },
        { cx + w * 0.5f - 0.5f, cy - h * 0.5f - 0.5f, 0.f,1.f,col },
        { cx + w * 0.5f - 0.5f, cy + h * 0.5f - 0.5f, 0.f,1.f,col },
        { cx - w * 0.5f - 0.5f, cy + h * 0.5f - 0.5f, 0.f,1.f,col },
        { cx - w * 0.5f - 0.5f, cy - h * 0.5f - 0.5f, 0.f,1.f,col },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_LINESTRIP, 4, outline, sizeof(ColorVertex));

    // Pin 1 notch
    float notchX = cx - w * 0.5f + 6.f;
    DWORD notchCol = D3DCOLOR_ARGB(a, 0, br / 2, br / 4);
    ColorVertex notch[2] = {
        { notchX - 4.f - 0.5f, cy - h * 0.5f - 0.5f, 0.f,1.f,notchCol },
        { notchX + 4.f - 0.5f, cy - h * 0.5f - 0.5f, 0.f,1.f,notchCol },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, 1, notch, sizeof(ColorVertex));

    float padH = (h - 8.f) / (float)(pins > 1 ? pins - 1 : 1);
    float padSz = 3.5f;
    int   padBr = ClampI(Ftoi(bright * 200.f), 40, 200);
    DWORD padCol = D3DCOLOR_ARGB(a, padBr / 4, padBr, padBr / 2);
    for (int p = 0; p < pins; ++p) {
        float py = cy - h * 0.5f + 4.f + (float)p * padH;
        float lx = cx - w * 0.5f - padSz * 0.5f;
        ColorVertex lpad[4] = {
            {lx - padSz - 0.5f,py - padSz * 0.5f - 0.5f,0.f,1.f,padCol},
            {lx + padSz - 0.5f,py - padSz * 0.5f - 0.5f,0.f,1.f,padCol},
            {lx - padSz - 0.5f,py + padSz * 0.5f - 0.5f,0.f,1.f,padCol},
            {lx + padSz - 0.5f,py + padSz * 0.5f - 0.5f,0.f,1.f,padCol},
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, lpad, sizeof(ColorVertex));
        float rx = cx + w * 0.5f + padSz * 0.5f;
        ColorVertex rpad[4] = {
            {rx - padSz - 0.5f,py - padSz * 0.5f - 0.5f,0.f,1.f,padCol},
            {rx + padSz - 0.5f,py - padSz * 0.5f - 0.5f,0.f,1.f,padCol},
            {rx - padSz - 0.5f,py + padSz * 0.5f - 0.5f,0.f,1.f,padCol},
            {rx + padSz - 0.5f,py + padSz * 0.5f - 0.5f,0.f,1.f,padCol},
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, rpad, sizeof(ColorVertex));
    }
}

// Traveling pulse dot along a multi-segment trace
static void DrawTracePulse(float x1, float y1, float x2, float y2,
    float phase, float traceWidth, float alpha)
{
    float pos = fmodf(phase, 1.0f);
    if (pos < 0.f) pos += 1.f;
    float px = x1 + (x2 - x1) * pos;
    float py = y1 + (y2 - y1) * pos;
    float hw = traceWidth * 3.f;
    int   a = ClampI(Ftoi(alpha * 255.f), 0, 255);
    DWORD col = D3DCOLOR_ARGB(a, 80, 255, 180);
    ColorVertex quad[4] = {
        {px - hw - 0.5f,py - hw - 0.5f,0.f,1.f,col},
        {px + hw - 0.5f,py - hw - 0.5f,0.f,1.f,col},
        {px - hw - 0.5f,py + hw - 0.5f,0.f,1.f,col},
        {px + hw - 0.5f,py + hw - 0.5f,0.f,1.f,col},
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(ColorVertex));
}

static void DrawCircuitBackground(int frame)
{
    if (!g_pDevice) return;

    g_pDevice->SetVertexShader(COLOR_FVF);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    float t = (float)frame * 0.016f;

    // ── Dark PCB substrate ────────────────────────────────────────────────────
    {
        ColorVertex bg[4] = {
            {  -0.5f,   -0.5f, 0.f,1.f, D3DCOLOR_XRGB(4,14, 8) },
            {639.5f,    -0.5f, 0.f,1.f, D3DCOLOR_XRGB(4,14, 8) },
            {  -0.5f, 479.5f,  0.f,1.f, D3DCOLOR_XRGB(2, 8, 4) },
            {639.5f,  479.5f,  0.f,1.f, D3DCOLOR_XRGB(2, 8, 4) },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(ColorVertex));
    }

    // ── Route A: IC0 right -> IC1 left  (top power bus, 3px) ────────────────
    {
        float pulse = 0.50f + 0.28f * sinf(t * 0.8f);
        int   br = ClampI(Ftoi(pulse * 100.f), 20, 120);
        DWORD col = D3DCOLOR_ARGB(200, 0, br, br / 3);
        float pts[] = { 145.f,130.f,  300.f,130.f,  360.f,130.f,  495.f,130.f };
        DrawPCBTrace(pts, 4, 3.0f, col);
        DrawTracePulse(145.f, 130.f, 495.f, 130.f, fmodf(t * 0.20f, 1.f), 3.0f, pulse * 0.8f);
    }
    // ── Route B: board left -> IC0 left pads  (power input) ──────────────────
    {
        float pulse = 0.48f + 0.25f * sinf(t * 0.9f + 0.5f);
        int   br = ClampI(Ftoi(pulse * 95.f), 18, 115);
        DWORD col = D3DCOLOR_ARGB(195, 0, br, br / 3);
        float pts[] = { 0.f,130.f,  95.f,130.f };
        DrawPCBTrace(pts, 2, 3.0f, col);
        DrawTracePulse(0.f, 130.f, 95.f, 130.f, fmodf(t * 0.22f + 0.3f, 1.f), 3.0f, pulse * 0.8f);
    }
    // ── Route C: IC1 right -> board right  (power out) ───────────────────────
    {
        float pulse = 0.46f + 0.26f * sinf(t * 0.85f + 1.0f);
        int   br = ClampI(Ftoi(pulse * 92.f), 16, 112);
        DWORD col = D3DCOLOR_ARGB(192, 0, br, br / 3);
        float pts[] = { 545.f,130.f,  640.f,130.f };
        DrawPCBTrace(pts, 2, 3.0f, col);
        DrawTracePulse(545.f, 130.f, 640.f, 130.f, fmodf(t * 0.19f + 0.6f, 1.f), 3.0f, pulse * 0.8f);
    }
    // ── Route D: IC0 bottom -> IC2 top  (signal, 45-deg jog) ─────────────────
    {
        float pulse = 0.40f + 0.28f * sinf(t * 1.0f + 1.2f);
        int   br = ClampI(Ftoi(pulse * 68.f), 10, 85);
        DWORD col = D3DCOLOR_ARGB(178, 0, br, br / 3);
        float pts[] = { 120.f,170.f,  120.f,210.f,  180.f,250.f,  290.f,250.f };
        DrawPCBTrace(pts, 4, 1.5f, col);
        DrawTracePulse(120.f, 170.f, 290.f, 250.f, fmodf(t * 0.28f + 0.4f, 1.f), 1.5f, pulse * 0.7f);
    }
    // ── Route E: IC1 bottom -> IC2 top-right  (signal, jog left) ─────────────
    {
        float pulse = 0.38f + 0.30f * sinf(t * 1.1f + 2.0f);
        int   br = ClampI(Ftoi(pulse * 65.f), 10, 82);
        DWORD col = D3DCOLOR_ARGB(174, 0, br, br / 3);
        float pts[] = { 520.f,180.f,  520.f,215.f,  460.f,250.f,  350.f,250.f };
        DrawPCBTrace(pts, 4, 1.5f, col);
        DrawTracePulse(520.f, 180.f, 350.f, 250.f, fmodf(t * 0.26f + 0.8f, 1.f), 1.5f, pulse * 0.7f);
    }
    // ── Route F: IC2 left -> IC3 right  (signal) ─────────────────────────────
    {
        float pulse = 0.42f + 0.27f * sinf(t * 0.95f + 0.9f);
        int   br = ClampI(Ftoi(pulse * 66.f), 10, 83);
        DWORD col = D3DCOLOR_ARGB(176, 0, br, br / 3);
        float pts[] = { 290.f,300.f,  180.f,300.f,  120.f,345.f };
        DrawPCBTrace(pts, 3, 1.5f, col);
        DrawTracePulse(290.f, 300.f, 120.f, 345.f, fmodf(t * 0.31f + 0.2f, 1.f), 1.5f, pulse * 0.7f);
    }
    // ── Route G: IC2 right -> IC4 left  (signal) ─────────────────────────────
    {
        float pulse = 0.39f + 0.29f * sinf(t * 1.05f + 1.5f);
        int   br = ClampI(Ftoi(pulse * 64.f), 10, 80);
        DWORD col = D3DCOLOR_ARGB(173, 0, br, br / 3);
        float pts[] = { 350.f,300.f,  430.f,300.f,  480.f,330.f,  508.f,360.f };
        DrawPCBTrace(pts, 4, 1.5f, col);
        DrawTracePulse(350.f, 300.f, 508.f, 360.f, fmodf(t * 0.24f + 1.1f, 1.f), 1.5f, pulse * 0.7f);
    }
    // ── Route H: IC3 bottom -> GND board bottom  (power, 3px) ────────────────
    {
        float pulse = 0.44f + 0.26f * sinf(t * 0.88f + 2.8f);
        int   br = ClampI(Ftoi(pulse * 90.f), 16, 108);
        DWORD col = D3DCOLOR_ARGB(188, 0, br, br / 3);
        float pts[] = { 100.f,415.f,  100.f,450.f,  60.f,480.f };
        DrawPCBTrace(pts, 3, 3.0f, col);
    }
    // ── Route I: IC4 bottom -> GND board bottom  (power, 3px) ────────────────
    {
        float pulse = 0.43f + 0.25f * sinf(t * 0.82f + 3.2f);
        int   br = ClampI(Ftoi(pulse * 88.f), 15, 106);
        DWORD col = D3DCOLOR_ARGB(186, 0, br, br / 3);
        float pts[] = { 530.f,410.f,  530.f,450.f,  570.f,480.f };
        DrawPCBTrace(pts, 3, 3.0f, col);
    }
    // ── Route J: IC2 bottom -> board bottom  (data out) ──────────────────────
    {
        float pulse = 0.37f + 0.28f * sinf(t * 1.15f + 1.7f);
        int   br = ClampI(Ftoi(pulse * 60.f), 8, 76);
        DWORD col = D3DCOLOR_ARGB(165, 0, br, br / 3);
        float pts[] = { 320.f,350.f,  320.f,400.f,  280.f,440.f,  280.f,480.f };
        DrawPCBTrace(pts, 4, 1.5f, col);
        DrawTracePulse(320.f, 350.f, 280.f, 480.f, fmodf(t * 0.33f + 0.5f, 1.f), 1.5f, pulse * 0.7f);
    }
    // ── Serpentine A: between IC0 and IC1 top (length match on power bus) ─────
    {
        float pulse = 0.45f + 0.28f * sinf(t * 1.2f + 0.6f);
        int   br = ClampI(Ftoi(pulse * 70.f), 10, 88);
        DWORD col = D3DCOLOR_ARGB(175, 0, br, br / 4);
        DrawSerpentine(200.f, 60.f, 220.f, 16.f, 40.f, 1.5f, col, 0);
    }
    // ── Serpentine B: IC3 top going up (impedance stub) ───────────────────────
    {
        float pulse = 0.40f + 0.27f * sinf(t * 0.95f + 1.8f);
        int   br = ClampI(Ftoi(pulse * 65.f), 10, 82);
        DWORD col = D3DCOLOR_ARGB(170, 0, br, br / 4);
        DrawSerpentine(100.f, 270.f, 60.f, 13.f, 34.f, 1.5f, col, 1);
    }
    // ── Serpentine C: IC4 left area (DDR length match) ───────────────────────
    {
        float pulse = 0.42f + 0.26f * sinf(t * 1.05f + 2.4f);
        int   br = ClampI(Ftoi(pulse * 67.f), 10, 85);
        DWORD col = D3DCOLOR_ARGB(172, 0, br, br / 4);
        DrawSerpentine(400.f, 420.f, 160.f, 15.f, 38.f, 1.5f, col, 0);
    }
    // ── Vias at chamfer junctions ─────────────────────────────────────────────
    struct ViaPos { float x, y; float r; };
    static const ViaPos vias[] = {
        { 120.f, 210.f, 4.f },   // Route D bend
        { 180.f, 250.f, 4.f },   // Route D corner
        { 520.f, 215.f, 4.f },   // Route E bend
        { 460.f, 250.f, 4.f },   // Route E corner
        { 180.f, 300.f, 4.f },   // Route F corner
        { 120.f, 345.f, 4.f },   // Route F -> IC3 top
        { 430.f, 300.f, 4.f },   // Route G corner
        { 480.f, 330.f, 4.f },   // Route G corner 2
        { 100.f, 450.f, 5.f },   // Route H power via
        { 530.f, 450.f, 5.f },   // Route I power via
        { 320.f, 400.f, 4.f },   // Route J bend
        { 280.f, 440.f, 4.f },   // Route J corner
        { 200.f,  60.f, 4.f },   // Serpentine A start
        { 100.f, 270.f, 4.f },   // Serpentine B start
        { 400.f, 420.f, 4.f },   // Serpentine C start
    };
    const int N_VIAS = sizeof(vias) / sizeof(ViaPos);
    for (int i = 0; i < N_VIAS; ++i) {
        float pulse = 0.5f + 0.5f * sinf(t * 1.3f + (float)i * 0.44f);
        DrawVia(vias[i].x, vias[i].y, vias[i].r, pulse, 0.85f);
    }
    // ── IC footprints -- positions match trace endpoints above ─────────────────
    struct ICPos { float cx, cy, w, h; int pins; };
    static const ICPos ics[] = {
        { 120.f, 130.f, 50.f,  80.f,  8  },   // IC0 top-left
        { 520.f, 140.f, 50.f,  80.f,  8  },   // IC1 top-right
        { 320.f, 300.f, 60.f, 100.f, 10  },   // IC2 center
        { 100.f, 380.f, 40.f,  70.f,  6  },   // IC3 bottom-left
        { 530.f, 370.f, 44.f,  80.f,  8  },   // IC4 bottom-right
    };
    const int N_ICS = sizeof(ics) / sizeof(ICPos);
    for (int i = 0; i < N_ICS; ++i) {
        float pulse = 0.45f + 0.30f * sinf(t * 0.6f + (float)i * 1.1f);
        DrawICFootprint(ics[i].cx, ics[i].cy, ics[i].w, ics[i].h,
            ics[i].pins, pulse, 0.90f);
    }
}



static void DrawLogoWithGlitch(LPDIRECT3DTEXTURE8 tex, int texW, int texH,
    float cx, float cy, float dispW, float dispH,
    int alpha, int frame)
{
    if (!tex || texW <= 0 || texH <= 0 || alpha <= 0) return;

    float w = dispW;
    float h = dispH;
    float t = (float)frame * 0.02f;

    // Subtle continuous drift -- only when called for TR logo
    // XBS is passed cy = SCREEN_H*0.5f exactly, drift kept very small so it
    // stays visually centered at all times
    // Small drift for visual life -- kept tight so logos stay visually centered
    float driftX = 2.f * sinf(t * 0.9f);
    float driftY = 1.5f * sinf(t * 0.7f);

    float left = cx - w * 0.5f + driftX;
    float right = cx + w * 0.5f + driftX;
    float top = cy - h * 0.5f + driftY;
    float bot = cy + h * 0.5f + driftY;

    // Glitch burst: every ~40 frames, 8-frame burst of UV tearing
    bool  glitching = ((frame / 40) % 4 == 0) && ((frame % 40) < 8);
    float burstT = glitching ? (float)(frame % 8) / 8.f : 0.f;

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    g_pDevice->SetTexture(0, tex);
    g_pDevice->SetVertexShader(INTRO_FVF);

    // Draw logo as horizontal slices -- each slice gets independent UV X shift
    // during glitch, creating a scanline-tear / VHS-style distortion
    const int SLICES = 16;
    float sliceH = h / (float)SLICES;

    for (int s2 = 0; s2 < SLICES; ++s2)
    {
        float v0 = (float)s2 / (float)SLICES;
        float v1 = (float)(s2 + 1) / (float)SLICES;

        float sy0 = top + (float)s2 * sliceH;
        float sy1 = top + (float)(s2 + 1) * sliceH;

        // UV X distortion -- only during glitch, different per slice
        float uShift = 0.f;
        if (glitching)
        {
            // Each slice gets a different random-ish shift based on its index
            int   seed = PseudoRand(s2 * 37 + frame);
            float mag = (float)(seed % 100) / 100.f * 0.12f * burstT;
            // Some slices shift right, some left, some not at all
            if ((seed % 3) == 0)      uShift = mag;
            else if ((seed % 3) == 1) uShift = -mag;
            // else no shift -- gives partial tear look
        }

        // UV V distortion -- slight vertical stretch/compress per slice in glitch
        float vShift = 0.f;
        if (glitching && ((PseudoRand(s2 * 13 + frame * 2)) % 5) == 0)
            vShift = burstT * 0.02f * (((PseudoRand(s2)) % 2) ? 1.f : -1.f);

        // Alpha flicker on some slices during glitch
        int sliceAlpha = alpha;
        if (glitching && ((PseudoRand(s2 * 7 + frame)) % 4) == 0)
            sliceAlpha = ClampI(alpha / 3, 0, 255);

        DWORD col = D3DCOLOR_ARGB(sliceAlpha, 255, 255, 255);

        IntroVertex v[4] = {
            { left - 0.5f,  sy0 - 0.5f, 0.f,1.f, col, 0.f + uShift, v0 + vShift },
            { right - 0.5f, sy0 - 0.5f, 0.f,1.f, col, 1.f + uShift, v0 + vShift },
            { left - 0.5f,  sy1 - 0.5f, 0.f,1.f, col, 0.f + uShift, v1 + vShift },
            { right - 0.5f, sy1 - 0.5f, 0.f,1.f, col, 1.f + uShift, v1 + vShift },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(IntroVertex));
    }

    // Restore blend
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// =============================================================================
// Fade alpha helper

// =============================================================================

static int FadeAlpha(int frame, int fadeIn, int hold, int fadeOut)
{
    if (frame < 0) return 0;
    if (frame < fadeIn)
        return ClampI((frame * 255) / (fadeIn < 1 ? 1 : fadeIn), 0, 255);
    frame -= fadeIn;
    if (frame < hold) return 255;
    frame -= hold;
    if (frame < fadeOut)
        return ClampI(255 - (frame * 255) / (fadeOut < 1 ? 1 : fadeOut), 0, 255);
    return 0;
}

// =============================================================================
// State cleanup
// =============================================================================

static void EndFrameCleanup()
{
    if (!g_pDevice) return;
    for (int si = 0; si < 4; ++si)
    {
        g_pDevice->SetTexture(si, NULL);
        g_pDevice->SetTextureStageState(si, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(si, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(si, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(si, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
    }
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(255, 255, 255));
}

// =============================================================================
// Public API
// =============================================================================

void IntroScene_Init()
{
    s_introActive = true;
    s_frameCount = 0;
    s_phase = PHASE_PRESENTED;
    s_phaseFrame = 0;
    s_twChars = 0;
    s_twLine2 = 0;
    s_cursorBlink = true;

    s_logoTex = LoadTextureFromDDS("D:\\tex\\tr.dds", s_logoW, s_logoH);
    s_xbsTex = LoadTextureFromDDS("D:\\tex\\xbs.dds", s_xbsW, s_xbsH);
}

void IntroScene_Shutdown()
{
    s_introActive = false;
    if (s_logoTex) { s_logoTex->Release(); s_logoTex = NULL; }
    if (s_xbsTex) { s_xbsTex->Release();  s_xbsTex = NULL; }
}

bool IntroScene_IsFinished()
{
    return s_phase == PHASE_DONE;
}

void IntroScene_Render(float /*demoTime*/)
{
    if (!s_introActive || !g_pDevice) return;

    s_frameCount++;
    s_phaseFrame++;

    // -------------------------------------------------------------------------
    // Timing constants (at ~30fps on real HW, ~17s total)
    // -------------------------------------------------------------------------
    // PHASE_PRESENTED: "Presented By:" types in, then "Darkone83" types in
    const int PRES_TOTAL = 330;   // ~11s at 30fps
    const int PRES_TYPEOUT = 42;    // fade out

    // PHASE_LOGO_TR: TR logo with glitch
    const int LOGO_TOTAL = 240;   // ~8s at 30fps
    const int LOGO_FADEIN = 20;
    const int LOGO_FADEOUT = 20;

    // PHASE_MUSIC_BY: "Music By:" + "Darkone83" typewriter
    const int MUSIC_TOTAL = 210;  // ~7s at 30fps
    const int MUSIC_FADEOUT = 42;

    // PHASE_SUPPORT_XBS: XBS logo
    const int XBS_TOTAL = 180;   // ~6s at 30fps
    const int XBS_FADEIN = 20;
    const int XBS_FADEOUT = 20;

    // Draw circuit board every frame
    DrawCircuitBackground(s_frameCount);

    int   fade = 0;
    DWORD col = 0;

    switch (s_phase)
    {
        // -------------------------------------------------------------------------
    case PHASE_PRESENTED:
    {
        const char* L1 = "Presented By:";
        const char* L2 = "Darkone83";
        const int   L1_LEN = (int)strlen(L1);
        const int   L2_LEN = (int)strlen(L2);

        // Typewriter: one char every 4 frames
        int totalChars = s_phaseFrame / 4;
        s_twChars = ClampI(totalChars, 0, L1_LEN);
        int overflow = totalChars - L1_LEN - 8; // 8 frame pause between lines
        s_twLine2 = ClampI(overflow, 0, L2_LEN);

        // Fade out at end
        int fadeAlpha = 255;
        int timeLeft = PRES_TOTAL - s_phaseFrame;
        if (timeLeft < PRES_TYPEOUT)
            fadeAlpha = ClampI((timeLeft * 255) / PRES_TYPEOUT, 0, 255);

        col = D3DCOLOR_ARGB(255,
            ClampI((fadeAlpha * 0) / 255, 0, 255),
            ClampI((fadeAlpha * 220) / 255, 0, 255),
            ClampI((fadeAlpha * 180) / 255, 0, 255));  // teal text to match circuit

        DWORD col2 = D3DCOLOR_ARGB(255,
            ClampI((fadeAlpha * 0) / 255, 0, 255),
            ClampI((fadeAlpha * 255) / 255, 0, 255),
            ClampI((fadeAlpha * 255) / 255, 0, 255)); // bright cyan for name

        // Draw line 1
        if (s_twChars > 0)
        {
            char buf[64] = { 0 };
            strncpy(buf, L1, s_twChars);
            bool cur1 = (s_twLine2 == 0) && ((s_frameCount / 8) % 2 == 0);
            if (cur1) { int bl = (int)strlen(buf); if (bl < 62) { buf[bl] = '_'; buf[bl + 1] = 0; } }
            float tw = (float)strlen(buf) * 8.f * 2.2f;
            DrawText((SCREEN_W - tw) * 0.5f, 185.f, buf, 2.2f, col);
        }
        // Draw line 2
        if (s_twLine2 > 0)
        {
            char buf[64] = { 0 };
            strncpy(buf, L2, s_twLine2);
            bool cur2 = (s_twLine2 < L2_LEN) && ((s_frameCount / 8) % 2 == 0);
            if (cur2) { int bl = (int)strlen(buf); if (bl < 62) { buf[bl] = '_'; buf[bl + 1] = 0; } }
            float tw = (float)strlen(buf) * 8.f * 2.8f;
            DrawText((SCREEN_W - tw) * 0.5f, 220.f, buf, 2.8f, col2);
        }

        if (s_phaseFrame >= PRES_TOTAL)
        {
            s_phase = PHASE_LOGO_TR;
            s_phaseFrame = 0;
            s_twChars = 0; s_twLine2 = 0;
        }
        break;
    }

    // -------------------------------------------------------------------------
    case PHASE_LOGO_TR:
    {
        fade = FadeAlpha(s_phaseFrame, LOGO_FADEIN, LOGO_TOTAL - LOGO_FADEIN - LOGO_FADEOUT, LOGO_FADEOUT);

        DrawLogoWithGlitch(s_logoTex, s_logoW, s_logoH,
            SCREEN_W * 0.5f, SCREEN_H * 0.5f,
            320.f, 320.f, fade, s_frameCount);

        if (s_phaseFrame >= LOGO_TOTAL)
        {
            s_phase = PHASE_MUSIC_BY;
            s_phaseFrame = 0;
            s_twChars = 0; s_twLine2 = 0;
        }
        break;
    }

    // -------------------------------------------------------------------------
    case PHASE_MUSIC_BY:
    {
        const char* L1 = "Music By:";
        const char* L2 = "Darkone83";
        const int   L1_LEN = (int)strlen(L1);
        const int   L2_LEN = (int)strlen(L2);

        int totalChars = s_phaseFrame / 4;
        s_twChars = ClampI(totalChars, 0, L1_LEN);
        int overflow = totalChars - L1_LEN - 8;
        s_twLine2 = ClampI(overflow, 0, L2_LEN);

        int fadeAlpha = 255;
        int timeLeft = MUSIC_TOTAL - s_phaseFrame;
        if (timeLeft < MUSIC_FADEOUT)
            fadeAlpha = ClampI((timeLeft * 255) / MUSIC_FADEOUT, 0, 255);

        // Warm amber/gold tones for music credit -- different from teal presented
        col = D3DCOLOR_ARGB(255,
            ClampI((fadeAlpha * 220) / 255, 0, 255),
            ClampI((fadeAlpha * 160) / 255, 0, 255),
            ClampI((fadeAlpha * 20) / 255, 0, 255));
        DWORD col2 = D3DCOLOR_ARGB(255,
            ClampI((fadeAlpha * 255) / 255, 0, 255),
            ClampI((fadeAlpha * 200) / 255, 0, 255),
            ClampI((fadeAlpha * 40) / 255, 0, 255));

        if (s_twChars > 0)
        {
            char buf[64] = { 0 };
            strncpy(buf, L1, s_twChars);
            bool cur1 = (s_twLine2 == 0) && ((s_frameCount / 8) % 2 == 0);
            if (cur1) { int bl = (int)strlen(buf); if (bl < 62) { buf[bl] = '_'; buf[bl + 1] = 0; } }
            float tw = (float)strlen(buf) * 8.f * 2.2f;
            DrawText((SCREEN_W - tw) * 0.5f, 185.f, buf, 2.2f, col);
        }
        if (s_twLine2 > 0)
        {
            char buf[64] = { 0 };
            strncpy(buf, L2, s_twLine2);
            bool cur2 = (s_twLine2 < L2_LEN) && ((s_frameCount / 8) % 2 == 0);
            if (cur2) { int bl = (int)strlen(buf); if (bl < 62) { buf[bl] = '_'; buf[bl + 1] = 0; } }
            float tw = (float)strlen(buf) * 8.f * 2.8f;
            DrawText((SCREEN_W - tw) * 0.5f, 220.f, buf, 2.8f, col2);
        }

        if (s_phaseFrame >= MUSIC_TOTAL)
        {
            s_phase = PHASE_SUPPORT_XBS;
            s_phaseFrame = 0;
            s_twChars = 0; s_twLine2 = 0;
        }
        break;
    }

    // -------------------------------------------------------------------------
    case PHASE_SUPPORT_XBS:
    {
        fade = FadeAlpha(s_phaseFrame, XBS_FADEIN, XBS_TOTAL - XBS_FADEIN - XBS_FADEOUT, XBS_FADEOUT);

        // Typewriter for "Proudly Supporting:" -- types in during fade-in
        const char* L1 = "Proudly Supporting:";
        int L1_LEN = (int)strlen(L1);
        s_twChars = ClampI(s_phaseFrame / 3, 0, L1_LEN);

        int textAlpha = ClampI(fade, 0, 255);
        col = D3DCOLOR_ARGB(255,
            0,
            ClampI((textAlpha * 220) / 255, 0, 255),
            ClampI((textAlpha * 180) / 255, 0, 255));

        // Text at top (y=60), logo below center -- matches original layout
        if (s_twChars > 0)
        {
            char buf[64] = { 0 };
            strncpy(buf, L1, s_twChars);
            bool cur = (s_twChars < L1_LEN) && ((s_frameCount / 8) % 2 == 0);
            if (cur) { int bl = (int)strlen(buf); if (bl < 62) { buf[bl] = '_'; buf[bl + 1] = 0; } }
            float tw = (float)strlen(buf) * 8.f * 2.0f;
            DrawText((SCREEN_W - tw) * 0.5f, 60.f, buf, 2.0f, col);
        }

        // Logo at SCREEN_H*0.5 + 90 = cy=330, same as original
        // 340x340 display size -- slightly larger to fill frame better
        DrawLogoWithGlitch(s_xbsTex, s_xbsW, s_xbsH,
            SCREEN_W * 0.5f, SCREEN_H * 0.5f + 90.f,
            340.f, 340.f, fade, s_frameCount);

        if (s_phaseFrame >= XBS_TOTAL)
        {
            s_phase = PHASE_DONE;
            s_phaseFrame = 0;
        }
        break;
    }

    case PHASE_DONE:
    default:
        break;
    }

    EndFrameCleanup();
}