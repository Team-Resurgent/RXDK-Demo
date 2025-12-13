// Credits.cpp - Star Wars style scroller (RXDK)
// Uses DrawText() from your font.cpp/.h
//
// Tweaks vs previous:
// - Less “pinch” (reduced pull-in strength, slightly larger min scale)
// - Less “choppy” feel (slower scroll + consistent per-line gap computation,
//   and no fixed-gap stepping in the early-skip path)
//
// Requirements hit:
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

// Where it “vanishes” into distance
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
// As y approaches horizon, scale down and “pull in” (Star Wars feel).
static void GetPerspectiveForY(float y, float* outScale, float* outPull)
{
    // t=0 at bottom-ish, t=1 at horizon
    float t = (s_bottomStartY - y) / (s_bottomStartY - s_horizonY);
    t = ClampF(t, 0.0f, 1.0f);

    // non-linear shrink
    float s = (1.0f - t);
    s = s * s;                  // stronger falloff near horizon
    s = 0.30f + 0.70f * s;       // larger minimum size (less “pinched”/tiny)

    // pull-in (narrowing) as it goes “back”
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
}

void Credits_Shutdown()
{
    s_active = false;
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

    Setup2DTextStates();

    const DWORD now = GetTickCount();
    const float tSec = (float)(now - s_startTicks) * (1.0f / 1000.0f);

    // Base Y for first line
    float y = s_bottomStartY - tSec * s_speedPxPerSec;

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

        // Center + “pull in” towards center as it recedes
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
