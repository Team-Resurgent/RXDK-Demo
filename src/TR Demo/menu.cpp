// menu.cpp - Overlay menu system for RXDK demoscene
//
// Design:
//   - Renders as a translucent panel over the live running scene
//   - Self-contained animated nebula/starfield background (no external scene deps)
//   - Fades in/out smoothly using the same XYZRHW quad technique as main.cpp
//   - No heap allocation — all state is static
//   - No float->int casts (__ftol2_sse safe via inline Ftoi)
//   - Uses DrawText() from font.h
//   - Controller: BACK=toggle, D-Pad=navigate, A=select, B=back/close submenu
//
// State machine:
//   CLOSED -> [BACK] -> FADING_IN -> OPEN -> [BACK/B] -> FADING_OUT -> CLOSED
//   OPEN   -> [A on Jump/Select] -> SUBMENU_JUMP | SUBMENU_SELECT
//   SUBMENU -> [B] -> OPEN
//   SUBMENU_JUMP -> [A] -> JUMP_PREP (1.5s) -> emits GetRequestedScene() -> FADING_OUT
//
// Jump logic:
//   s_jumpScene holds the chosen DemoSceneId (or -2 for dashboard).
//   It is reset to -1 on every close so a plain B-press never leaks a stale ID.
//   s_jumpTarget is only set to a valid value at the moment MS_FADING_OUT
//   completes AND s_jumpScene was explicitly set this session.

#include "Menu.h"
#include "input.h"
#include "font.h"
#include "music.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// ------------------------------------------------------------
// RXDK-safe float->int  (no __ftol2_sse)
// ------------------------------------------------------------
static __forceinline int Ftoi(float f)
{
    int i;
    __asm { fld f }
    __asm { fistp i }
    return i;
}

static __forceinline float ClampF(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static __forceinline BYTE ClampByte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

// ------------------------------------------------------------
// Layout constants
// ------------------------------------------------------------
static const float SW = 640.0f;
static const float SH = 480.0f;

// Panel dims — tall enough to hold 8-row sub-lists + hints without clipping
static const float PANEL_X = 110.0f;
static const float PANEL_Y = 50.0f;
static const float PANEL_W = 420.0f;
static const float PANEL_H = 370.0f;

// Item list inside panel
static const float ITEM_X = PANEL_X + 32.0f;
static const float ITEM_Y0 = PANEL_Y + 58.0f;   // first item Y (below title+separator)
static const float ITEM_STEP = 46.0f;             // gap between items
static const float ITEM_SCALE = 1.6f;

// Sub-list (Jump / Scene select)
static const float SUB_X = PANEL_X + 44.0f;
static const float SUB_Y0 = PANEL_Y + 58.0f;
static const float SUB_STEP = 35.0f;
static const float SUB_SCALE = 1.4f;
static const int   SUB_VISIBLE = 8;    // 8 rows × 35px = 280px; fits above hint area

// Colors
static const DWORD COL_PANEL_BG = D3DCOLOR_ARGB(185, 8, 12, 18);
static const DWORD COL_ACCENT = D3DCOLOR_ARGB(255, 16, 200, 60); // Xbox green
static const DWORD COL_TITLE = D3DCOLOR_ARGB(255, 255, 255, 255);
static const DWORD COL_ITEM = D3DCOLOR_ARGB(255, 180, 200, 180);
static const DWORD COL_DISABLED_SUB = D3DCOLOR_ARGB(255, 90, 100, 90);
static const DWORD COL_HINT = D3DCOLOR_ARGB(255, 100, 130, 110);
static const DWORD COL_VALUE_ON = D3DCOLOR_ARGB(255, 40, 220, 80);
static const DWORD COL_VALUE_OFF = D3DCOLOR_ARGB(255, 180, 60, 60);

// Timing
static const DWORD FADE_IN_MS = 350;
static const DWORD FADE_OUT_MS = 250;
static const DWORD JUMP_PREP_MS = 1500;   // framebuffer settle before scene init
static const DWORD NAV_REPEAT_MS = 160;    // held D-pad repeat rate

// ------------------------------------------------------------
// Self-contained animated background — panel-contained
// Nebula gradient + depth-sorted parallax stars rendered only
// inside the panel rect. Uses D3D scissor rect to clip cleanly.
// Star animation mirrors Credits exactly: depth z drives size,
// twinkle varies quad size not just brightness.
// ------------------------------------------------------------

// Trig LUT
static const int  BG_LUT_N = 1024;
static float      s_bgSin[BG_LUT_N];
static float      s_bgCos[BG_LUT_N];
static bool       s_bgLutReady = false;

static void BG_BuildLUT()
{
    if (s_bgLutReady) return;
    for (int i = 0; i < BG_LUT_N; ++i)
    {
        float a = (float)i * (2.0f * 3.14159265f) / (float)BG_LUT_N;
        s_bgSin[i] = sinf(a);
        s_bgCos[i] = cosf(a);
    }
    s_bgLutReady = true;
}

// Stars — depth-based like Credits
static const int BG_STAR_N = 220;
struct BgStar
{
    float nx, ny;      // normalised position 0..1 within panel
    float z;           // depth 0=far 1=near (drives size + brightness)
    int   phaseIdx;    // LUT offset for twinkle
    int   baseBright;  // base brightness 60..220
    BYTE  colType;     // 0=blue-white 1=cyan 2=xbox-green 3=white 4=teal
};
static BgStar s_bgStars[BG_STAR_N];
static bool   s_bgStarsBuilt = false;
static DWORD  s_bgStartTick = 0;

static unsigned s_bgRng = 0xFEED1234u;
static unsigned BgRand()
{
    s_bgRng = s_bgRng * 1664525u + 1013904223u;
    return s_bgRng;
}

static void BG_BuildStars()
{
    if (s_bgStarsBuilt) return;
    s_bgRng ^= GetTickCount();
    for (int i = 0; i < BG_STAR_N; ++i)
    {
        unsigned r = BgRand();
        s_bgStars[i].nx = (float)(r & 0xFFFFu) * (1.0f / 65535.0f);
        r = BgRand();
        s_bgStars[i].ny = (float)(r & 0xFFFFu) * (1.0f / 65535.0f);
        r = BgRand();
        // depth: bias towards far stars (more far than near, like a real sky)
        float zraw = (float)(r & 1023u) * (1.0f / 1023.0f);
        s_bgStars[i].z = zraw * zraw;   // squared = more far stars
        r = BgRand();
        s_bgStars[i].phaseIdx = (int)(r & 1023u);
        // brightness scales with depth: far=dim, near=bright
        s_bgStars[i].baseBright = 60 + Ftoi(s_bgStars[i].z * 160.0f);
        r = BgRand();
        s_bgStars[i].colType = (BYTE)(r % 5u);
    }
    s_bgStarsBuilt = true;
}

// Draw the nebula gradient clipped to the panel rect
static void BG_DrawNebula(DWORD tMs, float px, float py, float pw, float ph)
{
    int ph0 = (int)((tMs / 14u) & 1023u);
    int ph1 = (int)((tMs / 17u) & 1023u);
    int ph2 = (int)((tMs / 11u) & 1023u);

    // Deep space: very dark Xbox-green tones
    BYTE topR = ClampByte(5 + Ftoi(4.0f * s_bgSin[ph0]));
    BYTE topG = ClampByte(18 + Ftoi(10.0f * s_bgSin[(ph0 + 150) & 1023]));
    BYTE topB = ClampByte(28 + Ftoi(12.0f * s_bgSin[ph1]));

    BYTE botR = ClampByte(8 + Ftoi(5.0f * s_bgSin[ph2]));
    BYTE botG = ClampByte(35 + Ftoi(18.0f * s_bgSin[(ph2 + 200) & 1023]));
    BYTE botB = ClampByte(22 + Ftoi(10.0f * s_bgSin[(ph1 + 300) & 1023]));

    DWORD cTop = D3DCOLOR_ARGB(255, topR, topG, topB);
    DWORD cBot = D3DCOLOR_ARGB(255, botR, botG, botB);

    struct V2D { float x, y, z, rhw; DWORD c; };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    // Subtle green-teal mid glow — punches up the depth feel
    float gx = px + pw * 0.5f;
    float gy = py + ph * 0.38f;
    int phG = (int)((tMs / 6u) & 1023u);
    float gPulse = 0.7f + s_bgSin[phG] * 0.2f;
    float gR = pw * (0.45f + 0.05f * gPulse);
    BYTE  gA = ClampByte(Ftoi(35.0f * gPulse));
    DWORD gc = D3DCOLOR_ARGB(gA, 20, 160, 80);
    DWORD ge = D3DCOLOR_ARGB(0, 5, 40, 20);

    V2D bg[4] =
    {
        { px,      py,      0.0f, 1.0f, cTop },
        { px + pw, py,      0.0f, 1.0f, cTop },
        { px,      py + ph, 0.0f, 1.0f, cBot },
        { px + pw, py + ph, 0.0f, 1.0f, cBot },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(V2D));

    // Glow circle — additive
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

    const int GSEG = 32;
    V2D gfan[GSEG + 2];
    gfan[0] = { gx, gy, 0.0f, 1.0f, gc };
    for (int s = 0; s <= GSEG; ++s)
    {
        int ai = (s * BG_LUT_N / GSEG) & (BG_LUT_N - 1);
        gfan[s + 1] = { gx + s_bgCos[ai] * gR, gy + s_bgSin[ai] * gR, 0.0f, 1.0f, ge };
    }
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, GSEG, gfan, sizeof(V2D));
}

// Draw animated depth stars clipped to the panel rect
static void BG_DrawStars(DWORD tMs, float px, float py, float pw, float ph)
{
    struct V2D { float x, y, z, rhw; DWORD c; };

    int base = (int)((tMs / 8u) & 1023u);

    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

    for (int i = 0; i < BG_STAR_N; ++i)
    {
        const BgStar& st = s_bgStars[i];

        // Use a per-star phase offset derived from index to fully desync twinkles.
        // Two LUT offsets — one drives size, one drives brightness independently.
        int phS = (base + st.phaseIdx) & (BG_LUT_N - 1);
        int phB = (base + ((st.phaseIdx + 256) & (BG_LUT_N - 1))) & (BG_LUT_N - 1);

        // Twinkle size: 0.7x..1.3x — exactly like Credits
        float twinkle = 0.70f + 0.60f * (0.5f + 0.5f * s_bgSin[phS]);   // 0.70..1.30
        float baseSize = 0.7f + st.z * 2.6f;   // far=tiny, near=large
        float size = baseSize * twinkle;

        // Twinkle brightness: never goes below 40% of base
        float brightMul = 0.60f + 0.40f * (0.5f + 0.5f * s_bgSin[phB]);  // 0.60..1.00
        int br = ClampByte(Ftoi((float)st.baseBright * brightMul));
        BYTE b = (BYTE)br;

        DWORD c;
        switch (st.colType)
        {
        case 0: c = D3DCOLOR_ARGB(b, ClampByte(br - 25), ClampByte(br - 12), b);        break; // blue-white
        case 1: c = D3DCOLOR_ARGB(b, 0, ClampByte(br - 10), b);                       break; // cyan
        case 2: c = D3DCOLOR_ARGB(b, 0, b, ClampByte(br >> 1));                     break; // xbox-green
        case 3: c = D3DCOLOR_ARGB(b, b, b, b);                                      break; // pure white
        default:c = D3DCOLOR_ARGB(b, ClampByte(br >> 1), b, ClampByte(br - 10));        break; // teal
        }

        // Screen position within panel
        float sx = px + st.nx * pw;
        float sy = py + st.ny * ph;

        V2D q[4] =
        {
            { sx,        sy,        0.0f, 1.0f, c },
            { sx + size, sy,        0.0f, 1.0f, c },
            { sx,        sy + size, 0.0f, 1.0f, c },
            { sx + size, sy + size, 0.0f, 1.0f, c },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void BG_Draw(float panelY)
{
    if (!g_pDevice) return;
    BG_BuildLUT();
    BG_BuildStars();

    DWORD tMs = GetTickCount() - s_bgStartTick;

    // Panel rect — stars and nebula are drawn to these bounds directly,
    // no scissor needed since all geometry is computed within px/py/pw/ph.
    float px = PANEL_X;
    float py = panelY;
    float pw = PANEL_W;
    float ph = PANEL_H;

    BG_DrawNebula(tMs, px, py, pw, ph);
    BG_DrawStars(tMs, px, py, pw, ph);

    // Clean up
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}


enum MenuState
{
    MS_CLOSED = 0,
    MS_FADING_IN,
    MS_OPEN,
    MS_SUBMENU_JUMP,
    MS_SUBMENU_SELECT,
    MS_JUMP_PREP,
    MS_FADING_OUT
};

static MenuState s_state = MS_CLOSED;
static DWORD     s_stateStart = 0;      // tick when current state began
static float     s_panelAlpha = 0.0f;   // 0..1 (drives both panel + item alpha)
static float     s_slideY = 20.0f;  // panel vertical offset (slides in)

// Main menu cursor
static int       s_cursor = 0;
static const int MAIN_ITEMS = 4;

// Sub-menu state
static int       s_subCursor = 0;
static int       s_subScroll = 0;      // top-of-list scroll offset
static int       s_sceneCount = 0;

// Scene enable bitmask (max 16 scenes — WORD covers SCENE_COUNT ≤ 16)
static WORD      s_sceneEnabled = 0xFFFF; // all enabled by default

// Jump request output
static int       s_jumpTarget = -1;     // -1 = no pending jump
static int       s_jumpScene = -1;     // scene chosen, waiting for prep

// Music state (shadows main.cpp musicPaused)
static bool      s_musicMuted = false;

// Nav repeat state
static DWORD     s_navHeldSince = 0;
static DWORD     s_navLastRepeat = 0;
static bool      s_navHeld = false;

// Tick reference (supplied each Update)
static DWORD     s_nowTicks = 0;

// ------------------------------------------------------------
// Scene names (must match DemoSceneId order in main.cpp)
// ------------------------------------------------------------
static const char* s_sceneNames[] =
{
    "Intro",
    "Chrome",
    "Plasma",
    "Ball",
    "Ring",
    "Galaxy",
    "VU RXDK",
    "X Logo",
    "Cube",
    "Cube Env",
    "Drip",
    "Maze",
    "Post FX",
    "Credits",
    "City",
};
static const int SCENE_NAMES_MAX = 15;

// Main menu item labels
static const char* s_mainLabels[MAIN_ITEMS] =
{
    "Jump to Scene",
    "Scene Select",
    "Toggle Music",
    "Exit to Dashboard"
};

// ------------------------------------------------------------
// Primitive draw helpers (no tex, XYZRHW+DIFFUSE)
// ------------------------------------------------------------
struct MV { float x, y, z, rhw; DWORD col; };

static void DrawQuad(float x, float y, float w, float h, DWORD col)
{
    if (!g_pDevice) return;
    MV v[4] =
    {
        { x,     y,     0.f, 1.f, col },
        { x + w, y,     0.f, 1.f, col },
        { x,     y + h, 0.f, 1.f, col },
        { x + w, y + h, 0.f, 1.f, col },
    };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(MV));
}

static void DrawHLine(float x, float y, float w, DWORD col)
{
    DrawQuad(x, y, w, 2.0f, col);
}

// Scale an ARGB color's alpha by [0..1]
static DWORD ScaleAlpha(DWORD col, float t)
{
    BYTE a = (BYTE)(((col >> 24) & 0xFF) * t);
    return (col & 0x00FFFFFF) | ((DWORD)a << 24);
}

// Build a pulsing selector color (Xbox green, breathing alpha)
static DWORD SelectorColor(float t)
{
    // t = time in seconds
    float pulse = 0.70f + 0.30f * sinf(t * 3.5f);
    // Bright green fill for selected row background
    BYTE a = ClampByte(Ftoi(pulse * 60.0f));   // subtle tint
    return D3DCOLOR_ARGB(a, 20, 210, 70);
}

// ------------------------------------------------------------
// Set 2D render states
// ------------------------------------------------------------
static void Begin2D()
{
    if (!g_pDevice) return;
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetTexture(0, NULL);
}

static void End2D()
{
    if (!g_pDevice) return;
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
}

// ------------------------------------------------------------
// Text helper — applies global panel alpha
// ------------------------------------------------------------
static void MenuText(float x, float y, const char* txt, float scale, DWORD col)
{
    // Modulate the text color's alpha by s_panelAlpha
    BYTE baseA = (BYTE)((col >> 24) & 0xFF);
    BYTE finalA = ClampByte(Ftoi((float)baseA * s_panelAlpha));
    DWORD c = (col & 0x00FFFFFF) | ((DWORD)finalA << 24);
    DrawText(x, y, txt, scale, c);
}

// Approximate text width (5x7 bitmap, 6px advance at scale=1)
static float TextW(const char* txt, float scale)
{
    if (!txt) return 0.f;
    int n = 0;
    while (txt[n]) ++n;
    return (float)n * 6.0f * scale;
}

// ------------------------------------------------------------
// Navigation helpers
// ------------------------------------------------------------
static void ClampCursor(int& cur, int count)
{
    if (cur < 0)       cur = count - 1;
    if (cur >= count)  cur = 0;
}

static void ClampSubScroll()
{
    int visible = SUB_VISIBLE;
    if (s_subScroll < 0) s_subScroll = 0;
    int maxScroll = s_sceneCount - visible;
    if (maxScroll < 0) maxScroll = 0;
    if (s_subScroll > maxScroll) s_subScroll = maxScroll;

    // Keep cursor in view
    if (s_subCursor < s_subScroll)
        s_subScroll = s_subCursor;
    if (s_subCursor >= s_subScroll + visible)
        s_subScroll = s_subCursor - visible + 1;
}

// Handle held D-pad repeat — returns true on initial press OR after repeat interval
static bool NavPressed(WORD pressed, WORD held, WORD btn)
{
    if (pressed & btn)
    {
        s_navHeld = true;
        s_navHeldSince = s_nowTicks;
        s_navLastRepeat = s_nowTicks;
        return true;
    }
    if ((held & btn) && s_navHeld)
    {
        DWORD heldFor = s_nowTicks - s_navHeldSince;
        if (heldFor > 400)  // initial delay before repeat kicks in
        {
            if ((s_nowTicks - s_navLastRepeat) >= NAV_REPEAT_MS)
            {
                s_navLastRepeat = s_nowTicks;
                return true;
            }
        }
    }
    return false;
}

static void ClearNavHeld(WORD held)
{
    // Reset held state when no nav buttons are held
    WORD navMask = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
        XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
    if (!(held & navMask)) s_navHeld = false;
}

// ------------------------------------------------------------
// State transitions
// ------------------------------------------------------------
static void SetState(MenuState ns)
{
    s_state = ns;
    s_stateStart = s_nowTicks;
}

// ------------------------------------------------------------
// Public API — Init
// ------------------------------------------------------------
void Menu_Init(int sceneCount)
{
    s_sceneCount = (sceneCount > SCENE_NAMES_MAX) ? SCENE_NAMES_MAX : sceneCount;
    s_sceneEnabled = (WORD)((1u << s_sceneCount) - 1u);  // all enabled
    s_state = MS_CLOSED;
    s_panelAlpha = 0.0f;
    s_slideY = 20.0f;
    s_cursor = 0;
    s_subCursor = 0;
    s_subScroll = 0;
    s_jumpTarget = -1;
    s_jumpScene = -1;
    s_musicMuted = false;
    s_navHeld = false;
    s_bgStartTick = GetTickCount();
    BG_BuildLUT();
    BG_BuildStars();
}

// ------------------------------------------------------------
// Public API — Toggle
// ------------------------------------------------------------
void Menu_Toggle()
{
    switch (s_state)
    {
    case MS_CLOSED:
    case MS_FADING_OUT:
        s_cursor = 0;
        s_jumpScene = -1;    // clear any stale jump from a previous session
        SetState(MS_FADING_IN);
        break;

    case MS_OPEN:
    case MS_SUBMENU_JUMP:
    case MS_SUBMENU_SELECT:
        SetState(MS_FADING_OUT);
        break;

    default:
        break;
    }
}

// ------------------------------------------------------------
// Public API — Update (returns true if input was consumed)
// ------------------------------------------------------------
bool Menu_Update(DWORD nowTicks, WORD pressed)
{
    s_nowTicks = nowTicks;

    // Current held state (not just pressed) for repeat nav
    // We approximate "held" from the pressed mask — caller passes pressed edges.
    // For repeat we need raw hold; we'll track it internally via the BTN_ mask.
    // Since main.cpp computes `pressed = buttons & ~lastButtons`, we reconstruct
    // "currently held" by checking GetButtons() directly.
    // (input.h GetButtons() returns current state, not edge.)
    extern WORD GetButtons();
    WORD held = GetButtons();

    DWORD elapsed = nowTicks - s_stateStart;

    switch (s_state)
    {
        // ----------------------------------------------------------------
    case MS_CLOSED:
        s_panelAlpha = 0.0f;
        s_slideY = 20.0f;
        return false;

        // ----------------------------------------------------------------
    case MS_FADING_IN:
    {
        float t = ClampF((float)elapsed / (float)FADE_IN_MS, 0.f, 1.f);
        // Ease-out: t^0.5 approximated via two lerp steps
        float ease = t * (2.0f - t);   // smoothstep-ish
        s_panelAlpha = ease;
        s_slideY = 20.0f * (1.0f - ease);
        if (elapsed >= FADE_IN_MS)
        {
            s_panelAlpha = 1.0f;
            s_slideY = 0.0f;
            SetState(MS_OPEN);
        }
        return true;
    }

    // ----------------------------------------------------------------
    case MS_FADING_OUT:
    {
        float t = ClampF((float)elapsed / (float)FADE_OUT_MS, 0.f, 1.f);
        s_panelAlpha = 1.0f - t;
        s_slideY = 20.0f * t;
        if (elapsed >= FADE_OUT_MS)
        {
            s_panelAlpha = 0.0f;
            s_slideY = 20.0f;
            SetState(MS_CLOSED);
            // Only expose jump if one was explicitly requested this session.
            // s_jumpScene == -1 means a plain close (B or BACK) — emit nothing.
            if (s_jumpScene != -1)
            {
                s_jumpTarget = s_jumpScene;
                s_jumpScene = -1;
            }
        }
        return true;
    }

    // ----------------------------------------------------------------
    case MS_JUMP_PREP:
    {
        // Hold for JUMP_PREP_MS to let the framebuffer settle, then fade out
        // and signal the jump. The current scene keeps rendering during this.
        s_panelAlpha = 1.0f;  // keep menu fully visible during prep
        if (elapsed >= JUMP_PREP_MS)
        {
            // Now fade out and signal jump
            SetState(MS_FADING_OUT);
        }
        return true;
    }

    // ----------------------------------------------------------------
    case MS_OPEN:
    {
        s_panelAlpha = 1.0f;
        s_slideY = 0.0f;
        ClearNavHeld(held);

        if (NavPressed(pressed, held, XINPUT_GAMEPAD_DPAD_UP))
        {
            s_cursor--;
            ClampCursor(s_cursor, MAIN_ITEMS);
        }
        if (NavPressed(pressed, held, XINPUT_GAMEPAD_DPAD_DOWN))
        {
            s_cursor++;
            ClampCursor(s_cursor, MAIN_ITEMS);
        }

        if (pressed & BTN_A)
        {
            switch (s_cursor)
            {
            case 0:  // Jump to Scene
                s_subCursor = 0;
                s_subScroll = 0;
                SetState(MS_SUBMENU_JUMP);
                break;

            case 1:  // Scene Select
                s_subCursor = 0;
                s_subScroll = 0;
                SetState(MS_SUBMENU_SELECT);
                break;

            case 2:  // Toggle Music
                s_musicMuted = !s_musicMuted;
                if (s_musicMuted) Music_Pause();
                else              Music_Play();
                break;

            case 3:  // Exit to Dashboard
                // Call ExitToDashboard() via extern — defined in main.cpp
                // We can't call it directly from here without including internals.
                // Instead, signal via a special jump index (-2).
                s_jumpScene = -2;
                SetState(MS_FADING_OUT);
                break;
            }
        }

        // B or BACK = close menu
        if ((pressed & BTN_B) || (pressed & BTN_BACK))
            SetState(MS_FADING_OUT);

        return true;
    }

    // ----------------------------------------------------------------
    case MS_SUBMENU_JUMP:
    {
        s_panelAlpha = 1.0f;
        ClearNavHeld(held);

        if (NavPressed(pressed, held, XINPUT_GAMEPAD_DPAD_UP))
        {
            s_subCursor--;
            if (s_subCursor < 0) s_subCursor = s_sceneCount - 1;
            ClampSubScroll();
        }
        if (NavPressed(pressed, held, XINPUT_GAMEPAD_DPAD_DOWN))
        {
            s_subCursor++;
            if (s_subCursor >= s_sceneCount) s_subCursor = 0;
            ClampSubScroll();
        }

        if (pressed & BTN_A)
        {
            // Start jump prep countdown
            s_jumpScene = s_subCursor;
            SetState(MS_JUMP_PREP);
        }

        if (pressed & BTN_B)
            SetState(MS_OPEN);

        return true;
    }

    // ----------------------------------------------------------------
    case MS_SUBMENU_SELECT:
    {
        s_panelAlpha = 1.0f;
        ClearNavHeld(held);

        if (NavPressed(pressed, held, XINPUT_GAMEPAD_DPAD_UP))
        {
            s_subCursor--;
            if (s_subCursor < 0) s_subCursor = s_sceneCount - 1;
            ClampSubScroll();
        }
        if (NavPressed(pressed, held, XINPUT_GAMEPAD_DPAD_DOWN))
        {
            s_subCursor++;
            if (s_subCursor >= s_sceneCount) s_subCursor = 0;
            ClampSubScroll();
        }

        if (pressed & BTN_A)
        {
            // Toggle enable bit for this scene
            WORD bit = (WORD)(1u << s_subCursor);
            s_sceneEnabled ^= bit;

            // Don't allow ALL scenes to be disabled
            if (!s_sceneEnabled)
                s_sceneEnabled = bit;  // re-enable the one we just disabled
        }

        if (pressed & BTN_B)
            SetState(MS_OPEN);

        return true;
    }

    default:
        return false;
    }
}

// ------------------------------------------------------------
// Render helpers
// ------------------------------------------------------------

// Draw the main panel backdrop
static void RenderPanel(float t)
{
    float py = PANEL_Y + s_slideY;

    // Main panel body — semi-opaque dark glass over the nebula
    BYTE pa = ClampByte(Ftoi(((COL_PANEL_BG >> 24) & 0xFF) * t));
    DWORD panelCol = (COL_PANEL_BG & 0x00FFFFFF) | ((DWORD)pa << 24);
    DrawQuad(PANEL_X, py, PANEL_W, PANEL_H, panelCol);

    // Outer border (dim green line)
    BYTE ba = ClampByte(Ftoi(100.0f * t));
    DrawHLine(PANEL_X, py, PANEL_W, D3DCOLOR_ARGB(ba, 20, 180, 60));
    DrawHLine(PANEL_X, py + PANEL_H, PANEL_W, D3DCOLOR_ARGB(ba, 20, 180, 60));
    DrawQuad(PANEL_X, py, 2.f, PANEL_H, D3DCOLOR_ARGB(ba, 20, 180, 60));
    DrawQuad(PANEL_X + PANEL_W - 2.f, py, 2.f, PANEL_H, D3DCOLOR_ARGB(ba, 20, 180, 60));

    // Top accent bar — solid Xbox green
    BYTE aa = ClampByte(Ftoi(255.0f * t));
    DrawQuad(PANEL_X, py, PANEL_W, 4.f, D3DCOLOR_ARGB(aa, 16, 200, 60));

    // Title text
    const char* title = "RXDK DEMO MENU";
    float tw = TextW(title, 1.6f);
    float tx = PANEL_X + (PANEL_W - tw) * 0.5f;
    MenuText(tx, py + 14.f, title, 1.6f, COL_TITLE);

    // Separator under title
    DrawHLine(PANEL_X + 12.f, py + 42.f, PANEL_W - 24.f, D3DCOLOR_ARGB(ClampByte(Ftoi(60.f * t)), 16, 200, 60));
}

// Draw main menu items
static void RenderMainItems(float t)
{
    float py = PANEL_Y + s_slideY;
    float time = (float)s_nowTicks * 0.001f;

    for (int i = 0; i < MAIN_ITEMS; ++i)
    {
        float iy = py + ITEM_Y0 - PANEL_Y + (float)i * ITEM_STEP;

        bool sel = (s_state == MS_OPEN && i == s_cursor);

        if (sel)
        {
            // Pulsing green highlight bar
            DWORD hcol = SelectorColor(time);
            DrawQuad(PANEL_X + 6.f, iy - 4.f, PANEL_W - 12.f, ITEM_STEP - 6.f, hcol);

            // Arrow
            MenuText(ITEM_X - 18.f, iy, ">", ITEM_SCALE, ScaleAlpha(COL_ACCENT, t));
        }

        // Item label
        DWORD col = sel ? ScaleAlpha(COL_ACCENT, t) : ScaleAlpha(COL_ITEM, t);
        MenuText(ITEM_X, iy, s_mainLabels[i], ITEM_SCALE, col);

        // Inline value for Toggle Music
        if (i == 2)
        {
            const char* val = s_musicMuted ? "[OFF]" : "[ON]";
            DWORD vcol = s_musicMuted ? ScaleAlpha(COL_VALUE_OFF, t) : ScaleAlpha(COL_VALUE_ON, t);
            float vx = PANEL_X + PANEL_W - TextW(val, ITEM_SCALE) - 20.f;
            MenuText(vx, iy, val, ITEM_SCALE, vcol);
        }
    }
}

// Draw jump-to-scene submenu
static void RenderSubJump(float t)
{
    float py = PANEL_Y + s_slideY;
    float time = (float)s_nowTicks * 0.001f;

    // Sub-header
    MenuText(PANEL_X + 14.f, py + 14.f, "JUMP TO SCENE", 1.4f, ScaleAlpha(COL_ACCENT, t));
    DrawHLine(PANEL_X + 12.f, py + 42.f, PANEL_W - 24.f, D3DCOLOR_ARGB(ClampByte(Ftoi(60.f * t)), 16, 200, 60));

    int visible = SUB_VISIBLE;
    for (int vi = 0; vi < visible; ++vi)
    {
        int si = s_subScroll + vi;
        if (si >= s_sceneCount) break;

        float iy = py + SUB_Y0 - PANEL_Y + (float)vi * SUB_STEP;
        bool sel = (si == s_subCursor);

        if (sel)
        {
            DWORD hcol = SelectorColor(time);
            DrawQuad(PANEL_X + 6.f, iy - 3.f, PANEL_W - 12.f, SUB_STEP - 4.f, hcol);
            MenuText(SUB_X - 16.f, iy, ">", SUB_SCALE, ScaleAlpha(COL_ACCENT, t));
        }

        const char* name = (si < SCENE_NAMES_MAX) ? s_sceneNames[si] : "???";
        DWORD col = sel ? ScaleAlpha(COL_ACCENT, t) : ScaleAlpha(COL_ITEM, t);
        MenuText(SUB_X, iy, name, SUB_SCALE, col);
    }

    // Scroll indicators
    if (s_subScroll > 0)
        MenuText(PANEL_X + PANEL_W - 20.f, py + SUB_Y0 - PANEL_Y - 14.f, "^", SUB_SCALE, ScaleAlpha(COL_HINT, t));
    if (s_subScroll + visible < s_sceneCount)
        MenuText(PANEL_X + PANEL_W - 20.f, py + SUB_Y0 - PANEL_Y + (float)visible * SUB_STEP, "v", SUB_SCALE, ScaleAlpha(COL_HINT, t));
}

// Draw scene-select submenu (enable/disable toggles)
static void RenderSubSelect(float t)
{
    float py = PANEL_Y + s_slideY;
    float time = (float)s_nowTicks * 0.001f;

    MenuText(PANEL_X + 14.f, py + 14.f, "SCENE SELECT", 1.4f, ScaleAlpha(COL_ACCENT, t));
    DrawHLine(PANEL_X + 12.f, py + 42.f, PANEL_W - 24.f, D3DCOLOR_ARGB(ClampByte(Ftoi(60.f * t)), 16, 200, 60));

    int visible = SUB_VISIBLE;
    for (int vi = 0; vi < visible; ++vi)
    {
        int si = s_subScroll + vi;
        if (si >= s_sceneCount) break;

        float iy = py + SUB_Y0 - PANEL_Y + (float)vi * SUB_STEP;
        bool sel = (si == s_subCursor);
        bool enabled = (s_sceneEnabled & (1u << si)) != 0;

        if (sel)
        {
            DWORD hcol = SelectorColor(time);
            DrawQuad(PANEL_X + 6.f, iy - 3.f, PANEL_W - 12.f, SUB_STEP - 4.f, hcol);
            MenuText(SUB_X - 16.f, iy, ">", SUB_SCALE, ScaleAlpha(COL_ACCENT, t));
        }

        const char* name = (si < SCENE_NAMES_MAX) ? s_sceneNames[si] : "???";
        DWORD col = sel
            ? ScaleAlpha(COL_ACCENT, t)
            : (enabled ? ScaleAlpha(COL_ITEM, t) : ScaleAlpha(COL_DISABLED_SUB, t));
        MenuText(SUB_X, iy, name, SUB_SCALE, col);

        // Enable toggle on the right
        const char* tag = enabled ? "[ON] " : "[OFF]";
        DWORD tcol = enabled ? ScaleAlpha(COL_VALUE_ON, t) : ScaleAlpha(COL_VALUE_OFF, t);
        float tx = PANEL_X + PANEL_W - TextW(tag, SUB_SCALE) - 14.f;
        MenuText(tx, iy, tag, SUB_SCALE, tcol);
    }

    if (s_subScroll > 0)
        MenuText(PANEL_X + PANEL_W - 20.f, py + SUB_Y0 - PANEL_Y - 14.f, "^", SUB_SCALE, ScaleAlpha(COL_HINT, t));
    if (s_subScroll + visible < s_sceneCount)
        MenuText(PANEL_X + PANEL_W - 20.f, py + SUB_Y0 - PANEL_Y + (float)visible * SUB_STEP, "v", SUB_SCALE, ScaleAlpha(COL_HINT, t));
}

// Draw jump prep countdown overlay
static void RenderJumpPrep(float t)
{
    float py = PANEL_Y + s_slideY;

    // Show the target scene name centered in the panel
    const char* name = (s_jumpScene >= 0 && s_jumpScene < SCENE_NAMES_MAX)
        ? s_sceneNames[s_jumpScene]
        : "???";

    // Centered label
    const char* launching = "Launching:";
    float lw = TextW(launching, 1.4f);
    float nw = TextW(name, 1.8f);

    MenuText(PANEL_X + (PANEL_W - lw) * 0.5f, py + 110.f, launching, 1.4f, ScaleAlpha(COL_HINT, t));
    MenuText(PANEL_X + (PANEL_W - nw) * 0.5f, py + 140.f, name, 1.8f, ScaleAlpha(COL_ACCENT, t));

    // Countdown bar
    float progress = ClampF((float)(s_nowTicks - s_stateStart) / (float)JUMP_PREP_MS, 0.f, 1.f);
    float bx = PANEL_X + 24.f;
    float bw = PANEL_W - 48.f;
    float by = py + 200.f;

    // Bar background
    DrawQuad(bx, by, bw, 8.f, D3DCOLOR_ARGB(ClampByte(Ftoi(80.f * t)), 40, 60, 40));
    // Bar fill
    BYTE fa = ClampByte(Ftoi(200.f * t));
    DrawQuad(bx, by, bw * progress, 8.f, D3DCOLOR_ARGB(fa, 16, 200, 60));
}

// Draw hint bar at bottom of panel
static void RenderHints(float t)
{
    float py = PANEL_Y + s_slideY;
    // hint text height at scale 1.1: 7*1.1 + 1.1*0.9 shadow = 8.69px
    // place top of text 20px from panel bottom, separator 10px above that
    float hy = py + PANEL_H - 22.f;

    DrawHLine(PANEL_X + 12.f, hy - 10.f, PANEL_W - 24.f,
        D3DCOLOR_ARGB(ClampByte(Ftoi(40.f * t)), 16, 200, 60));

    const char* hint = (s_state == MS_OPEN)
        ? "A=Select  B=Back  BACK=Close"
        : "A=Confirm  B=Back";

    float hw = TextW(hint, 1.1f);
    MenuText(PANEL_X + (PANEL_W - hw) * 0.5f, hy, hint, 1.1f, ScaleAlpha(COL_HINT, t));
}

// ------------------------------------------------------------
// Public API — Render
// ------------------------------------------------------------
void Menu_Render()
{
    if (!g_pDevice) return;
    if (s_state == MS_CLOSED) return;

    float t = s_panelAlpha;   // 0..1 drives all alpha

    // Draw animated nebula+stars background inside the panel rect.
    // Pass current panel Y (includes slide offset) so it tracks the panel.
    BG_Draw(PANEL_Y + s_slideY);

    Begin2D();

    RenderPanel(t);

    switch (s_state)
    {
    case MS_OPEN:
    case MS_FADING_IN:
    case MS_FADING_OUT:
        RenderMainItems(t);
        break;

    case MS_SUBMENU_JUMP:
        RenderSubJump(t);
        break;

    case MS_SUBMENU_SELECT:
        RenderSubSelect(t);
        break;

    case MS_JUMP_PREP:
        RenderJumpPrep(t);
        break;

    default:
        break;
    }

    RenderHints(t);

    End2D();
}

// ------------------------------------------------------------
// Public API — queries
// ------------------------------------------------------------
bool Menu_IsOpen()
{
    return s_state != MS_CLOSED;
}

int Menu_GetRequestedScene()
{
    int r = s_jumpTarget;
    s_jumpTarget = -1;
    return r;
}

bool Menu_IsSceneEnabled(int sceneId)
{
    if (sceneId < 0 || sceneId >= 16) return true;
    return (s_sceneEnabled & (WORD)(1u << sceneId)) != 0;
}

bool Menu_IsMusicMuted()
{
    return s_musicMuted;
}