// Credits.cpp - Star Wars style scroller with dynamic starfield (RXDK)
// Uses DrawText() from your font.cpp/.h
//
// Features:
// - Dynamic parallax starfield background
// - Star Wars perspective text scroll
// - No per-frame allocations
// - No float->int casts (avoid __ftol2_sse)
// - Each shoutout name is a different color

#include "Credits.h"

#include <xtl.h>
#include <xgraphics.h>
#include <string.h>

#include "font.h"

// ------------------------------------------------------------
// Scene control
// ------------------------------------------------------------

static bool  s_active = false;
static DWORD s_startTicks = 0;

// ------------------------------------------------------------
// Starfield
// ------------------------------------------------------------

static const int STAR_COUNT = 200;
static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

struct Star
{
    float x, y;        // Screen position
    float z;           // Depth (0.0 = far, 1.0 = near)
    float baseX;       // Base X position for parallax
    BYTE brightness;   // 0-255
    BYTE colorType;    // 0-7 for different star colors
};

static Star s_stars[STAR_COUNT];
static bool s_starsInit = false;

// Simple LCG for star initialization (Init-only, no per-frame RNG)
static unsigned s_starSeed = 0x1234ABCD;

static unsigned StarRand()
{
    s_starSeed = s_starSeed * 1664525u + 1013904223u;
    return s_starSeed;
}

static void InitStarfield()
{
    if (s_starsInit) return;

    s_starSeed ^= GetTickCount();

    for (int i = 0; i < STAR_COUNT; ++i)
    {
        Star& s = s_stars[i];

        // Random depth
        unsigned r = StarRand();
        unsigned zInt = (r & 1023u);  // 0-1023
        s.z = (float)zInt * (1.0f / 1023.0f);  // 0.0-1.0

        // Base X position (will parallax)
        r = StarRand();
        unsigned xInt = (r % 640u);
        s.baseX = (float)xInt;
        s.x = s.baseX;

        // Y position
        r = StarRand();
        unsigned yInt = (r % 480u);
        s.y = (float)yInt;

        // Brightness based on depth (far = dim, near = bright)
        unsigned brightInt = 80u + (unsigned)(s.z * 175.0f);
        if (brightInt > 255u) brightInt = 255u;
        s.brightness = (BYTE)brightInt;

        // Color type (variety of star colors)
        r = StarRand();
        s.colorType = (BYTE)(r & 7u);  // 0-7
    }

    s_starsInit = true;
}

static void UpdateStarfield(float scrollY)
{
    // Parallax: stars move based on depth and scroll position
    // Far stars move less, near stars move more
    for (int i = 0; i < STAR_COUNT; ++i)
    {
        Star& s = s_stars[i];

        // Vertical scroll (stars move up as credits scroll up)
        // Faster stars = closer
        float speed = 0.15f + s.z * 0.35f;  // 0.15-0.50
        s.y -= speed;

        // Wrap around
        if (s.y < -10.0f)
            s.y += SCREEN_H + 20.0f;

        // Horizontal parallax based on scroll position
        // Creates slight drift as credits scroll
        float parallax = (scrollY * 0.02f) * (s.z - 0.5f);
        s.x = s.baseX + parallax;

        // Wrap horizontal
        if (s.x < 0.0f) s.x += SCREEN_W;
        if (s.x > SCREEN_W) s.x -= SCREEN_W;
    }
}

static DWORD GetStarColor(BYTE colorType, BYTE brightness, float time)
{
    // Pulsing factor (subtle)
    float pulse = 0.85f + 0.15f * sinf(time * 0.5f + (float)colorType * 0.7f);
    unsigned b = (unsigned)((float)brightness * pulse);
    if (b > 255u) b = 255u;

    BYTE br = (BYTE)b;

    // Different star color types
    switch (colorType)
    {
    case 0: // Blue-white (most common)
        return D3DCOLOR_ARGB(br, br, br, 255);
    case 1: // Cyan
        return D3DCOLOR_ARGB(br, (BYTE)(br >> 1), br, 255);
    case 2: // Pink/Magenta
        return D3DCOLOR_ARGB(br, 255, (BYTE)(br >> 1), 255);
    case 3: // Yellow
        return D3DCOLOR_ARGB(br, 255, 255, (BYTE)(br >> 1));
    case 4: // Orange
        return D3DCOLOR_ARGB(br, 255, (BYTE)(br >> 1) + 80, (BYTE)(br >> 2));
    case 5: // Purple
        return D3DCOLOR_ARGB(br, 200, 100, 255);
    case 6: // Green-white
        return D3DCOLOR_ARGB(br, (BYTE)(br >> 1), 255, (BYTE)(br >> 1));
    case 7: // Pure white (bright)
        return D3DCOLOR_ARGB(br, 255, 255, 255);
    default:
        return D3DCOLOR_ARGB(br, br, br, br);
    }
}

static void RenderStarfield()
{
    extern LPDIRECT3DDEVICE8 g_pDevice;
    if (!g_pDevice) return;

    DWORD now = GetTickCount();
    float time = (float)(now - s_startTicks) * 0.001f;

    struct StarVtx
    {
        float x, y, z, rhw;
        DWORD color;
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    // Draw stars as points (2x2 pixels for visibility)
    for (int i = 0; i < STAR_COUNT; ++i)
    {
        const Star& s = s_stars[i];

        DWORD col = GetStarColor(s.colorType, s.brightness, time + (float)i * 0.1f);

        // Draw as small quad
        float size = 1.0f + s.z * 1.5f;  // Bigger when closer

        // Twinkle effect - vary size slightly
        float twinkle = 0.9f + 0.2f * sinf(time * 2.0f + (float)i * 0.3f);
        size *= twinkle;

        StarVtx quad[4] =
        {
            { s.x,        s.y,        0.0f, 1.0f, col },
            { s.x + size, s.y,        0.0f, 1.0f, col },
            { s.x,        s.y + size, 0.0f, 1.0f, col },
            { s.x + size, s.y + size, 0.0f, 1.0f, col },
        };

        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(StarVtx));
    }
}

// ------------------------------------------------------------
// Helpers
// ------------------------------------------------------------

static inline float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static inline DWORD ARGB(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return D3DCOLOR_ARGB(a, r, g, b);
}

// Approximate text width for centering (5x7 font, typically 6px advance incl spacing)
static float MeasureTextWidth(const char* text, float scale)
{
    if (!text) return 0.0f;
    const size_t n = strlen(text);
    return (float)n * 6.0f * scale;
}

// Render states for 2D text (font.cpp likely uses XYZRHW internally; keep it safe)
static void Setup2DTextStates()
{
    extern LPDIRECT3DDEVICE8 g_pDevice;
    if (!g_pDevice) return;

    g_pDevice->SetTexture(0, NULL);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
}

static void End2DTextStates()
{
    extern LPDIRECT3DDEVICE8 g_pDevice;
    if (!g_pDevice) return;

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

// ------------------------------------------------------------
// Credits content
// ------------------------------------------------------------

enum LineType
{
    LT_Blank = 0,
    LT_Title,
    LT_Label,
    LT_Name
};

struct CreditLine
{
    const char* text;
    LineType    type;
    DWORD       color;
};

// Palette: each name gets a different color (readable on black)
static const DWORD C_TITLE = 0xFFFFFFFF;
static const DWORD C_LABEL = 0xFFB8D8FF;

static const DWORD C1 = 0xFFFFD27D; // warm gold
static const DWORD C2 = 0xFF7DE8FF; // cyan
static const DWORD C3 = 0xFF9CFF7D; // neon green
static const DWORD C4 = 0xFFFF7DF5; // magenta
static const DWORD C5 = 0xFF7D9BFF; // blue
static const DWORD C6 = 0xFFFF7D7D; // red
static const DWORD C7 = 0xFFD6FF7D; // yellow-green
static const DWORD C8 = 0xFF7DFFB8; // mint
static const DWORD C9 = 0xFFFFB07D; // orange
static const DWORD C10 = 0xFFB07DFF; // purple
static const DWORD C11 = 0xFF7DFF7D; // green
static const DWORD C12 = 0xFFFF7DB0; // pink
static const DWORD C13 = 0xFF7DE0B0; // aqua-green

// NOTE: Keep these static and simple—no dynamic allocation.
static const CreditLine s_lines[] =
{
    { "Credits",                     LT_Title, C_TITLE },
    { "",                            LT_Blank, 0 },

    { "Built in:",                   LT_Label, C_LABEL },
    { "RXDK",                        LT_Name,  C1 },
    { "",                            LT_Blank, 0 },

    { "Coded By:",                   LT_Label, C_LABEL },
    { "Darkone83",                   LT_Name,  C2 },
    { "",                            LT_Blank, 0 },

    { "Shoutouts:",                  LT_Label, C_LABEL },
    { "",                            LT_Blank, 0 },

    { "EqUiNox",                     LT_Name,  C3  },
    { "Haguero",                     LT_Name,  C4  },
    { "Andr0",                       LT_Name,  C5  },
    { "MeTalFAN",                    LT_Name,  C6  },
    { "ToxicMedz",                   LT_Name,  C7  },
    { "mast3rmind777",               LT_Name,  C8  },
    { "LD50 II",                     LT_Name,  C9  },
    { "Rocky5",                      LT_Name,  C10 },
    { "Harcroft",                    LT_Name,  C11 },
    { "Team Resurgent",              LT_Name,  C12 },
    { "Team Cerbios",                LT_Name,  C13 },
    { "The Xbox-Scene Discord",      LT_Name,  C2  },
    { "And the OGX community",       LT_Name,  C1  },

    { "",                            LT_Blank, 0 },
    { "",                            LT_Blank, 0 },
};

static const int LINE_COUNT = (int)(sizeof(s_lines) / sizeof(s_lines[0]));

// ------------------------------------------------------------
// Scroll / perspective settings
// ------------------------------------------------------------

// Slower = smoother with integer-ish font renderers
static float s_speedPxPerSec = 34.0f;

// Near-camera text size
static float s_baseScale = 2.10f;

// Base gap between lines (before perspective)
static float s_lineGap = 26.0f;

// Start off-screen (bottom)
static float s_bottomStartY = 520.0f;

// Where it "vanishes" into distance
static float s_horizonY = 90.0f;

// 640/2
static float s_centerX = 320.0f;

// Total virtual height (for IsFinished)
static float ComputeTotalHeight()
{
    const float per = s_lineGap * 1.15f;
    return (float)LINE_COUNT * per + 220.0f;
}

// Perspective mapping:
// As y approaches horizon, scale down and "pull in" (Star Wars feel).
static void GetPerspectiveForY(float y, float* outScale, float* outPull)
{
    // t=0 at bottom-ish, t=1 at horizon
    float t = (s_bottomStartY - y) / (s_bottomStartY - s_horizonY);
    t = ClampF(t, 0.0f, 1.0f);

    // non-linear shrink
    float s = (1.0f - t);
    s = s * s;                  // stronger falloff near horizon
    s = 0.30f + 0.70f * s;       // larger minimum size (less "pinched"/tiny)

    // pull-in (narrowing) as it goes "back"
    // (reduced from 0.65 -> 0.45 to avoid over-pinching)
    float pull = 1.0f - 0.45f * t;

    *outScale = s_baseScale * s;
    *outPull = pull;
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void Credits_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();
    InitStarfield();
}

void Credits_Shutdown()
{
    s_active = false;
    s_starsInit = false;
}

bool Credits_IsFinished()
{
    if (!s_active) return true;

    const DWORD now = GetTickCount();
    const float tSec = (float)(now - s_startTicks) * (1.0f / 1000.0f);

    // When the last line has passed beyond the horizon, end.
    const float totalH = ComputeTotalHeight();
    const float lastY = s_bottomStartY - tSec * s_speedPxPerSec + totalH;

    return (lastY < (s_horizonY - 40.0f));
}

void Credits_Render(float)
{
    extern LPDIRECT3DDEVICE8 g_pDevice;
    if (!s_active || !g_pDevice) return;

    const DWORD now = GetTickCount();
    const float tSec = (float)(now - s_startTicks) * (1.0f / 1000.0f);

    // Base Y for first line
    float y = s_bottomStartY - tSec * s_speedPxPerSec;

    // Update and render starfield background
    UpdateStarfield(tSec * s_speedPxPerSec);
    RenderStarfield();

    // Render credits text
    Setup2DTextStates();

    for (int i = 0; i < LINE_COUNT; ++i)
    {
        const CreditLine& L = s_lines[i];

        // Compute perspective + per-line gap BEFORE any skipping,
        // so y advances smoothly (no fixed-gap stepping).
        float scale, pull;
        GetPerspectiveForY(y, &scale, &pull);

        float gap = s_lineGap * (0.75f + 0.35f * (scale / s_baseScale));

        // Early skip far beyond top for perf (but keep correct spacing)
        if (y < (s_horizonY - 140.0f))
        {
            y += gap;
            continue;
        }

        if (L.type == LT_Blank || !L.text || L.text[0] == '\0')
        {
            y += gap;
            continue;
        }

        // Type-based sizing
        float sMul = 1.0f;
        if (L.type == LT_Title) sMul = 1.25f;
        else if (L.type == LT_Label) sMul = 0.95f;
        else sMul = 1.00f;

        float sFinal = scale * sMul;

        // Center + "pull in" towards center as it recedes
        const float w = MeasureTextWidth(L.text, sFinal);
        float x = s_centerX - (w * 0.5f);

        x = s_centerX + (x - s_centerX) * pull;

        // Fade out near horizon
        BYTE a = 255;
        if (y < (s_horizonY + 30.0f))
        {
            float tt = (y - s_horizonY) / 30.0f;  // 0..1
            tt = ClampF(tt, 0.0f, 1.0f);
            a = (BYTE)(255.0f * tt);
        }

        DWORD c = L.color;
        BYTE r = (BYTE)((c >> 16) & 0xFF);
        BYTE g = (BYTE)((c >> 8) & 0xFF);
        BYTE b = (BYTE)((c) & 0xFF);

        DrawText(x, y, L.text, sFinal, ARGB(a, r, g, b));

        y += gap;
    }

    End2DTextStates();
}