// IntroScene.cpp - Gradient + DDS logo intro (32-bit ARGB only)

#include "IntroScene.h"

#include <xtl.h>
#include <xgraphics.h>   // for XGSwizzleRect
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"        // DrawText from Xbox-RGB font

// Device provided by main.cpp
extern LPDIRECT3DDEVICE8 g_pDevice;

// Match your main resolution
static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

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

struct GradVertex
{
    float x, y, z, rhw;
    DWORD color;
};

#define GRAD_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// -----------------------------------------------------------------------------
// DDS header (subset)
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

// -----------------------------------------------------------------------------
// Scene state
// -----------------------------------------------------------------------------

static bool               s_introActive = false;

static LPDIRECT3DTEXTURE8 s_logoTex = NULL; // tr.dds
static int                s_logoW = 0;
static int                s_logoH = 0;

static LPDIRECT3DTEXTURE8 s_xbsTex = NULL; // xbs.dds
static int                s_xbsW = 0;
static int                s_xbsH = 0;

static int                s_frameCount = 0;

enum IntroPhase
{
    PHASE_PRESENTED = 0,
    PHASE_DARKONE1,
    PHASE_LOGO_TR,
    PHASE_MUSIC_BY,
    PHASE_DARKONE2,
    PHASE_SUPPORT_XBS,
    PHASE_DONE
};

static IntroPhase         s_phase = PHASE_PRESENTED;
static int                s_phaseFrame = 0;   // frames in current phase

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static void DrawFullscreenGradient()
{
    if (!g_pDevice) return;

    GradVertex v[4];

    const float z = 0.0f;
    const float rhw = 1.0f;

    DWORD topColor = D3DCOLOR_XRGB(10, 30, 70);
    DWORD bottomColor = D3DCOLOR_XRGB(0, 0, 0);

    // TL
    v[0].x = 0.0f;     v[0].y = 0.0f;
    v[0].z = z;        v[0].rhw = rhw;
    v[0].color = topColor;

    // TR
    v[1].x = SCREEN_W; v[1].y = 0.0f;
    v[1].z = z;        v[1].rhw = rhw;
    v[1].color = topColor;

    // BL
    v[2].x = 0.0f;     v[2].y = SCREEN_H;
    v[2].z = z;        v[2].rhw = rhw;
    v[2].color = bottomColor;

    // BR
    v[3].x = SCREEN_W; v[3].y = SCREEN_H;
    v[3].z = z;        v[3].rhw = rhw;
    v[3].color = bottomColor;

    g_pDevice->SetVertexShader(GRAD_FVF);
    g_pDevice->SetTexture(0, NULL);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    g_pDevice->DrawPrimitiveUP(
        D3DPT_TRIANGLESTRIP,
        2,
        v,
        sizeof(GradVertex)
    );
}

// Simple centered text using the Xbox-RGB bitmap font.
// We avoid float->int casts; centering is approximate but much closer now.
static void DrawCenteredText(const char* s, float y, float scale, DWORD color)
{
    if (!s || !s[0]) return;

    // Assume 8 px base glyph width; scale stretches it.
    const float baseCharW = 8.0f;
    float textW = (float)strlen(s) * baseCharW * scale;
    float x = (SCREEN_W - textW) * 0.5f;

    DrawText(x, y, s, scale, color);
}

// Integer fade: frame-based, no float->int casts anywhere.
static int ComputeFadeAlphaInt(int frame, int fadeInFrames, int holdFrames, int fadeOutFrames)
{
    if (frame < 0)
        return 0;

    if (fadeInFrames < 1)  fadeInFrames = 1;
    if (holdFrames < 0)  holdFrames = 0;
    if (fadeOutFrames < 1) fadeOutFrames = 1;

    if (frame < fadeInFrames)
    {
        // 0 -> 255
        return (frame * 255) / fadeInFrames;
    }

    frame -= fadeInFrames;
    if (frame < holdFrames)
    {
        return 255;
    }

    frame -= holdFrames;
    if (frame < fadeOutFrames)
    {
        // 255 -> 0
        int down = (frame * 255) / fadeOutFrames;
        int a = 255 - down;
        if (a < 0)   a = 0;
        if (a > 255) a = 255;
        return a;
    }

    return 0;
}

// -----------------------------------------------------------------------------
// Super-strict DDS loader for OG Xbox
// Only supports square, power-of-two, uncompressed A8R8G8B8 textures.
// Uses XGSwizzleRect to match the GPU’s swizzle layout.
// -----------------------------------------------------------------------------

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

    // --- Read magic ---
    DWORD magic = 0;
    if (!ReadFile(hFile, &magic, sizeof(DWORD), &bytesRead, NULL) ||
        bytesRead != sizeof(DWORD) ||
        magic != 0x20534444)  // "DDS "
    {
        CloseHandle(hFile);
        return NULL;
    }

    // --- Read header ---
    DDS_HEADER hdr;
    if (!ReadFile(hFile, &hdr, sizeof(DDS_HEADER), &bytesRead, NULL) ||
        bytesRead != sizeof(DDS_HEADER))
    {
        CloseHandle(hFile);
        return NULL;
    }

    // Validate header sizes
    if (hdr.size != 124 || hdr.ddspf.size != 32)
    {
        CloseHandle(hFile);
        return NULL;
    }

    const DWORD DDPF_FOURCC = 0x4;
    const DWORD DDPF_RGB = 0x40;
    const DWORD DDPF_ALPHAPIXELS = 0x1;

    // Must NOT be FOURCC (compressed)
    if (hdr.ddspf.flags & DDPF_FOURCC)
    {
        CloseHandle(hFile);
        return NULL;
    }

    // 32-bit ARGB with expected masks
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

    // Require square, power-of-two
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

    // --- Create swizzled Xbox texture (default behaviour) ---
    LPDIRECT3DTEXTURE8 tex = NULL;
    if (FAILED(g_pDevice->CreateTexture(
        (UINT)w,
        (UINT)h,
        1,
        0,                  // default usage -> swizzled
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

    // Swizzle from linear BGRA into Xbox texture layout
    XGSwizzleRect(
        pixels,          // pSource
        w * 4,           // pitch in bytes
        NULL,            // pRect (NULL -> full surface)
        lr.pBits,        // pDest (swizzled texture memory)
        w,               // width in texels
        h,               // height in texels
        NULL,            // pPoint (NULL -> dest origin)
        4                // bytes per pixel
    );

    tex->UnlockRect(0);
    free(pixels);

    outW = w;
    outH = h;
    return tex;
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void IntroScene_Init()
{
    s_introActive = true;
    s_frameCount = 0;

    s_phase = PHASE_PRESENTED;
    s_phaseFrame = 0;

    // Expects square, power-of-two A8R8G8B8 DDS (e.g. 512x512)
    s_logoTex = LoadTextureFromDDS("D:\\tr.dds", s_logoW, s_logoH);
    s_xbsTex = LoadTextureFromDDS("D:\\xbs.dds", s_xbsW, s_xbsH);
}

void IntroScene_Shutdown()
{
    s_introActive = false;

    if (s_logoTex)
    {
        s_logoTex->Release();
        s_logoTex = NULL;
    }

    if (s_xbsTex)
    {
        s_xbsTex->Release();
        s_xbsTex = NULL;
    }
}

void IntroScene_Render(float demoTime)
{
    (void)demoTime; // timing is frame-based, we ignore demoTime to avoid float->int

    if (!s_introActive || !g_pDevice)
        return;

    // Advance global & phase frame counters
    s_frameCount++;
    s_phaseFrame++;

    // Caller is responsible for Clear() / BeginScene() / EndScene() / Present()

    // Background gradient always present
    DrawFullscreenGradient();

    if (s_phase == PHASE_DONE)
        return;

    // Assuming ~60fps. Convert desired seconds to frames:
    // Presented: 0.7 in, 1.0 hold, 0.7 out
    const int PRESENTED_IN = 42;
    const int PRESENTED_HOLD = 60;
    const int PRESENTED_OUT = 42;

    // Darkone: 0.7 in, 2.0 hold, 0.7 out
    const int DARK_IN = 42;
    const int DARK_HOLD = 120;
    const int DARK_OUT = 42;

    // Logo: 1.0 in, 8.0 hold, 1.0 out
    const int LOGO_IN = 60;
    const int LOGO_HOLD = 480;
    const int LOGO_OUT = 60;

    // Music By: 0.7 in, 1.5 hold, 0.7 out
    const int MUSIC_IN = 42;
    const int MUSIC_HOLD = 90;
    const int MUSIC_OUT = 42;

    // Support: 0.7 in, 4.0 hold, 0.7 out
    const int SUPPORT_IN = 42;
    const int SUPPORT_HOLD = 240;
    const int SUPPORT_OUT = 42;

    DWORD col;
    int   fade;

    switch (s_phase)
    {
    case PHASE_PRESENTED:
        fade = ComputeFadeAlphaInt(s_phaseFrame, PRESENTED_IN, PRESENTED_HOLD, PRESENTED_OUT);
        if (fade > 0)
        {
            // Text fade = brightness fade (full alpha for font)
            col = D3DCOLOR_ARGB(255, fade, fade, fade);
            DrawCenteredText("Presented By:", 190.0f, 2.0f, col);
        }
        if (s_phaseFrame > (PRESENTED_IN + PRESENTED_HOLD + PRESENTED_OUT))
        {
            s_phase = PHASE_DARKONE1;
            s_phaseFrame = 0;
        }
        break;

    case PHASE_DARKONE1:
        fade = ComputeFadeAlphaInt(s_phaseFrame, DARK_IN, DARK_HOLD, DARK_OUT);
        if (fade > 0)
        {
            col = D3DCOLOR_ARGB(255, fade, fade, fade);
            DrawCenteredText("Darkone83", 200.0f, 2.8f, col);
        }
        if (s_phaseFrame > (DARK_IN + DARK_HOLD + DARK_OUT))
        {
            s_phase = PHASE_LOGO_TR;
            s_phaseFrame = 0;
        }
        break;

    case PHASE_LOGO_TR:
        fade = ComputeFadeAlphaInt(s_phaseFrame, LOGO_IN, LOGO_HOLD, LOGO_OUT);
        if (fade > 0 && s_logoTex && s_logoW > 0 && s_logoH > 0)
        {
            DWORD colLogo = D3DCOLOR_ARGB(fade, 255, 255, 255);

            float t = (float)s_frameCount * 0.02f;

            float scale = 0.60f + 0.05f * sinf(t * 1.5f);
            float driftX = 6.0f * sinf(t * 0.45f);
            float driftY = 4.0f * sinf(t * 0.30f);
            float angle = 0.10f * sinf(t * 0.80f);   // gentle wobble

            float cosA = cosf(angle);
            float sinA = sinf(angle);

            float w = s_logoW * scale;
            float h = s_logoH * scale;
            float cx = SCREEN_W * 0.5f + driftX;
            float cy = SCREEN_H * 0.5f + driftY;

            float hw = w * 0.5f;
            float hh = h * 0.5f;

            float x0 = -hw, y0 = -hh;
            float x1 = +hw, y1 = -hh;
            float x2 = -hw, y2 = +hh;
            float x3 = +hw, y3 = +hh;

            IntroVertex v[4];
            float rx, ry;

            // TL
            rx = x0 * cosA - y0 * sinA;
            ry = x0 * sinA + y0 * cosA;
            v[0].x = cx + rx; v[0].y = cy + ry;
            v[0].z = 0.0f; v[0].rhw = 1.0f;
            v[0].color = colLogo; v[0].u = 0.0f; v[0].v = 0.0f;

            // TR
            rx = x1 * cosA - y1 * sinA;
            ry = x1 * sinA + y1 * cosA;
            v[1].x = cx + rx; v[1].y = cy + ry;
            v[1].z = 0.0f; v[1].rhw = 1.0f;
            v[1].color = colLogo; v[1].u = 1.0f; v[1].v = 0.0f;

            // BL
            rx = x2 * cosA - y2 * sinA;
            ry = x2 * sinA + y2 * cosA;
            v[2].x = cx + rx; v[2].y = cy + ry;
            v[2].z = 0.0f; v[2].rhw = 1.0f;
            v[2].color = colLogo; v[2].u = 0.0f; v[2].v = 1.0f;

            // BR
            rx = x3 * cosA - y3 * sinA;
            ry = x3 * sinA + y3 * cosA;
            v[3].x = cx + rx; v[3].y = cy + ry;
            v[3].z = 0.0f; v[3].rhw = 1.0f;
            v[3].color = colLogo; v[3].u = 1.0f; v[3].v = 1.0f;

            g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
            g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
            g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
            g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
            g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
            g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

            g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
            g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

            g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
            g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
            g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);

            g_pDevice->SetTexture(0, s_logoTex);
            g_pDevice->SetVertexShader(INTRO_FVF);

            g_pDevice->DrawPrimitiveUP(
                D3DPT_TRIANGLESTRIP,
                2,
                v,
                sizeof(IntroVertex)
            );
        }
        if (s_phaseFrame > (LOGO_IN + LOGO_HOLD + LOGO_OUT))
        {
            s_phase = PHASE_MUSIC_BY;
            s_phaseFrame = 0;
        }
        break;

    case PHASE_MUSIC_BY:
        fade = ComputeFadeAlphaInt(s_phaseFrame, MUSIC_IN, MUSIC_HOLD, MUSIC_OUT);
        if (fade > 0)
        {
            col = D3DCOLOR_ARGB(255, fade, fade, fade);
            DrawCenteredText("Music By:", 190.0f, 2.2f, col);
        }
        if (s_phaseFrame > (MUSIC_IN + MUSIC_HOLD + MUSIC_OUT))
        {
            s_phase = PHASE_DARKONE2;
            s_phaseFrame = 0;
        }
        break;

    case PHASE_DARKONE2:
        fade = ComputeFadeAlphaInt(s_phaseFrame, DARK_IN, DARK_HOLD, DARK_OUT);
        if (fade > 0)
        {
            col = D3DCOLOR_ARGB(255, fade, fade, fade);
            DrawCenteredText("Darkone83", 200.0f, 2.8f, col);
        }
        if (s_phaseFrame > (DARK_IN + DARK_HOLD + DARK_OUT))
        {
            s_phase = PHASE_SUPPORT_XBS;
            s_phaseFrame = 0;
        }
        break;

    case PHASE_SUPPORT_XBS:
        fade = ComputeFadeAlphaInt(s_phaseFrame, SUPPORT_IN, SUPPORT_HOLD, SUPPORT_OUT);
        if (fade > 0)
        {
            // Top text: larger and properly centered
            DWORD textCol = D3DCOLOR_ARGB(255, fade, fade, fade);
            DrawCenteredText("Proudly Supporting:", 60.0f, 2.0f, textCol);

            // xbs.dds in dead-center, with a different style from tr.dds:
            //  - No rotation
            //  - Bigger breathing pulse
            //  - Squash & stretch
            //  - Circular drift
            if (s_xbsTex && s_xbsW > 0 && s_xbsH > 0)
            {
                DWORD texCol = D3DCOLOR_ARGB(fade, 255, 255, 255);

                float t = (float)s_frameCount * 0.02f;

                float baseScale = 0.70f + 0.12f * sinf(t * 1.5f);
                float squash = 1.0f + 0.18f * sinf(t * 3.0f);
                float stretch = 1.0f - 0.14f * sinf(t * 3.0f);

                float orbitX = 14.0f * sinf(t * 0.7f);
                float orbitY = 10.0f * cosf(t * 0.9f);

                float w = s_xbsW * baseScale * squash;
                float h = s_xbsH * baseScale * stretch;

                // True center of screen, plus small orbit offset
                float cx = SCREEN_W * 0.5f + orbitX;
                float cy = SCREEN_H * 0.5f + orbitY + 90.0f;

                float left = cx - w * 0.5f;
                float right = cx + w * 0.5f;
                float top = cy - h * 0.5f;
                float bottom = cy + h * 0.5f;

                IntroVertex v[4];

                v[0].x = left;  v[0].y = top;    v[0].z = 0.0f; v[0].rhw = 1.0f;
                v[0].color = texCol; v[0].u = 0.0f; v[0].v = 0.0f;

                v[1].x = right; v[1].y = top;    v[1].z = 0.0f; v[1].rhw = 1.0f;
                v[1].color = texCol; v[1].u = 1.0f; v[1].v = 0.0f;

                v[2].x = left;  v[2].y = bottom; v[2].z = 0.0f; v[2].rhw = 1.0f;
                v[2].color = texCol; v[2].u = 0.0f; v[2].v = 1.0f;

                v[3].x = right; v[3].y = bottom; v[3].z = 0.0f; v[3].rhw = 1.0f;
                v[3].color = texCol; v[3].u = 1.0f; v[3].v = 1.0f;

                g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
                g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
                g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
                g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
                g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
                g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
                g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

                g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
                g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
                g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);

                g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_POINT);
                g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_POINT);
                g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);

                g_pDevice->SetTexture(0, s_xbsTex);
                g_pDevice->SetVertexShader(INTRO_FVF);

                g_pDevice->DrawPrimitiveUP(
                    D3DPT_TRIANGLESTRIP,
                    2,
                    v,
                    sizeof(IntroVertex)
                );
            }
        }
        if (s_phaseFrame > (SUPPORT_IN + SUPPORT_HOLD + SUPPORT_OUT))
        {
            s_phase = PHASE_DONE;
            s_phaseFrame = 0;
            // Optionally: s_introActive = false; // hand off to next scene
        }
        break;

    case PHASE_DONE:
    default:
        break;
    }
}
