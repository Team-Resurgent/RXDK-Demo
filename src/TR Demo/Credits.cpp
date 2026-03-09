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
#include "music.h"

// Avoid __ftol2_sse on Xbox
static __declspec(noinline) int Ftoi(float f)
{
    int i;
    __asm {
        fld   f
        fistp i
    }
    return i;
}

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

// ------------------------------------------------------------
// Shooting stars
// ------------------------------------------------------------

static const int SHOOT_COUNT = 6;

struct ShootStar
{
    float x, y;        // current head position
    float vx, vy;      // velocity (px/sec)
    float life;        // 0.0 = dead, >0 = seconds remaining
    float maxLife;     // total life for fade calc
    DWORD color;       // streak colour
};

static ShootStar s_shoots[SHOOT_COUNT];
static unsigned  s_shootSeed = 0xDEADBEEF;

static unsigned ShootRand()
{
    s_shootSeed = s_shootSeed * 1664525u + 1013904223u;
    return s_shootSeed;
}

static void SpawnShoot(int slot, float time)
{
    ShootStar& ss = s_shoots[slot];

    // Random start along top or right edge
    unsigned r = ShootRand();
    ss.x = (float)(r % 640u);
    r = ShootRand();
    ss.y = (float)(r % 120u);         // upper third

    // Velocity: mostly diagonal down-left or down-right, fast
    r = ShootRand();
    float spd = 280.0f + (float)(r % 200u);
    r = ShootRand();
    float ang = 0.8f + (float)(r % 100u) * 0.01f;  // ~45-90 deg downward
    r = ShootRand();
    float hdir = (r & 1u) ? 1.0f : -1.0f;

    ss.vx = hdir * spd * 0.6f;
    ss.vy = spd * ang;

    // Life: 0.3 - 0.7 seconds
    r = ShootRand();
    ss.maxLife = 0.3f + (float)(r % 40u) * 0.01f;
    ss.life = ss.maxLife;

    // Colour: white, blue-white, or gold
    r = ShootRand();
    int ct = r % 3;
    if (ct == 0) ss.color = 0xFFFFFFFF;          // white
    else if (ct == 1) ss.color = 0xFFCCDDFF;          // blue-white
    else              ss.color = 0xFFFFEEAA;           // gold
}

static void InitShoots()
{
    s_shootSeed ^= GetTickCount();
    for (int i = 0; i < SHOOT_COUNT; ++i)
        s_shoots[i].life = 0.0f;
}

// ------------------------------------------------------------
// Title hold duration before scroll begins
// ------------------------------------------------------------
static const float TITLE_HOLD_SEC = 3.0f;   // seconds title stays fixed
static const float TITLE_FADE_SEC = 0.6f;   // fade out duration

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

// ------------------------------------------------------------
// Nebula background gradient
// ------------------------------------------------------------

static void DrawNebulaBackground()
{
    extern LPDIRECT3DDEVICE8 g_pDevice;
    if (!g_pDevice) return;

    struct GV { float x, y, z, rhw; DWORD c; };

    // Two-stop vertical gradient: deep purple at top, near-black at bottom
    // Drawn as a triangle strip quad
    DWORD cTop = D3DCOLOR_XRGB(18, 8, 38);   // deep indigo/purple
    DWORD cMid = D3DCOLOR_XRGB(5, 5, 22);   // dark blue
    DWORD cBot = D3DCOLOR_XRGB(2, 2, 8);   // near-black

    // Three rows: top, midpoint, bottom — two quads
    GV verts[6] =
    {
        {    0.f,   0.f, 0.f, 1.f, cTop },
        { 640.f,    0.f, 0.f, 1.f, cTop },
        {    0.f, 200.f, 0.f, 1.f, cMid },
        { 640.f,  200.f, 0.f, 1.f, cMid },
        {    0.f, 480.f, 0.f, 1.f, cBot },
        { 640.f,  480.f, 0.f, 1.f, cBot },
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 4, verts, sizeof(GV));
}

// ------------------------------------------------------------
// Shooting star update + render
// ------------------------------------------------------------

static void UpdateAndRenderShoots(float tSec, float dt)
{
    extern LPDIRECT3DDEVICE8 g_pDevice;
    if (!g_pDevice) return;

    // Spawn logic: random intervals, keep pool cycling
    {
        // Use low bits of tSec quantised to 0.4s buckets to gate spawns
        // without per-frame RNG — just revive dead slots occasionally
        static float s_nextSpawn = 0.5f;
        if (tSec >= s_nextSpawn)
        {
            // Find a dead slot
            for (int i = 0; i < SHOOT_COUNT; ++i)
            {
                if (s_shoots[i].life <= 0.0f)
                {
                    SpawnShoot(i, tSec);
                    break;
                }
            }
            unsigned r = ShootRand();
            s_nextSpawn = tSec + 0.35f + (float)(r % 60u) * 0.01f;
        }
    }

    struct LV { float x, y, z, rhw; DWORD c; };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);      // additive
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);

    for (int i = 0; i < SHOOT_COUNT; ++i)
    {
        ShootStar& ss = s_shoots[i];
        if (ss.life <= 0.0f) continue;

        ss.life -= dt;
        if (ss.life < 0.0f) ss.life = 0.0f;

        // Advance head
        ss.x += ss.vx * dt;
        ss.y += ss.vy * dt;

        // Fade: bright at birth, dim at end
        float frac = ss.life / ss.maxLife;     // 1=new, 0=dead
        int headA = Ftoi(220.f * frac);
        int tailA = Ftoi(60.f * frac);
        if (headA > 255) headA = 255;
        if (tailA > 255) tailA = 255;

        // Tail is behind the head by velocity * maxLife * 0.4
        float tailLen = 0.4f;
        float tx = ss.x - ss.vx * tailLen;
        float ty = ss.y - ss.vy * tailLen;

        // Extract base colour channels
        BYTE cr = (BYTE)((ss.color >> 16) & 0xFF);
        BYTE cg = (BYTE)((ss.color >> 8) & 0xFF);
        BYTE cb = (BYTE)(ss.color & 0xFF);

        LV seg[2] =
        {
            { tx,    ty,    0.f, 1.f, D3DCOLOR_ARGB(tailA, cr, cg, cb) },
            { ss.x,  ss.y,  0.f, 1.f, D3DCOLOR_ARGB(headA, cr, cg, cb) },
        };

        g_pDevice->DrawPrimitiveUP(D3DPT_LINELIST, 1, seg, sizeof(LV));
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
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

// NOTE: Keep these static and simple�no dynamic allocation.
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
    InitShoots();
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

    // Delta time for shooting star physics (cap at 100ms to avoid spiral on hitch)
    static DWORD s_lastTicks = 0;
    if (s_lastTicks == 0) s_lastTicks = now;
    float dt = (float)(now - s_lastTicks) * 0.001f;
    if (dt > 0.1f) dt = 0.1f;
    s_lastTicks = now;

    // -------------------------------------------------------------------------
    // Music VU — bass drives a brightness pulse on name colours
    // -------------------------------------------------------------------------
    int vu[4] = { 0, 0, 0, 0 };
    if (Music_IsPlaying())
        Music_GetVULevels(vu);
    // vuFlash: 0-40 additive brightness on the lowest visible name
    int vuFlash = (vu[0] * 40) >> 8;

    // -------------------------------------------------------------------------
    // Title hold: scroll doesn't start until TITLE_HOLD_SEC has passed
    // -------------------------------------------------------------------------
    float scrollT = tSec - TITLE_HOLD_SEC;
    if (scrollT < 0.0f) scrollT = 0.0f;

    // Title hold alpha: full during hold, fades out over TITLE_FADE_SEC
    // after scroll begins (so it doesn't draw twice once it scrolls in)
    BYTE titleHoldAlpha = 0;
    if (tSec < TITLE_HOLD_SEC)
    {
        // Fade in over first 0.5s
        float fin = tSec / 0.5f;
        if (fin > 1.0f) fin = 1.0f;
        titleHoldAlpha = (BYTE)Ftoi(255.f * fin);
    }
    else
    {
        // Fade out as scroll begins
        float fout = 1.0f - (tSec - TITLE_HOLD_SEC) / TITLE_FADE_SEC;
        if (fout < 0.0f) fout = 0.0f;
        titleHoldAlpha = (BYTE)Ftoi(255.f * fout);
    }

    // -------------------------------------------------------------------------
    // Background: nebula gradient then starfield then shooting stars
    // -------------------------------------------------------------------------
    DrawNebulaBackground();

    UpdateStarfield(scrollT * s_speedPxPerSec);
    RenderStarfield();

    UpdateAndRenderShoots(tSec, dt);

    // -------------------------------------------------------------------------
    // Credits text scroll
    // -------------------------------------------------------------------------
    Setup2DTextStates();

    // Scroll y driven by scrollT (delayed by title hold)
    float y = s_bottomStartY - scrollT * s_speedPxPerSec;

    // Track the lowest visible name line index for VU flash
    int lowestNameLine = -1;
    float lowestNameY = -9999.f;

    for (int i = 0; i < LINE_COUNT; ++i)
    {
        const CreditLine& L = s_lines[i];

        float scale, pull;
        GetPerspectiveForY(y, &scale, &pull);

        float gap = s_lineGap * (0.75f + 0.35f * (scale / s_baseScale));

        if (y < (s_horizonY - 140.0f))
        {
            y += gap;
            continue;
        }

        // Skip title line here — drawn separately as held overlay below
        if (i == 0 && L.type == LT_Title)
        {
            y += gap;
            continue;
        }

        if (L.type == LT_Blank || !L.text || L.text[0] == '\0')
        {
            y += gap;
            continue;
        }

        float sMul = 1.0f;
        if (L.type == LT_Title)       sMul = 1.25f;
        else if (L.type == LT_Label)  sMul = 0.95f;

        float sFinal = scale * sMul;

        const float w = MeasureTextWidth(L.text, sFinal);
        float x = s_centerX - (w * 0.5f);
        x = s_centerX + (x - s_centerX) * pull;

        // Fade near horizon
        BYTE a = 255;
        if (y < (s_horizonY + 30.0f))
        {
            float tt = (y - s_horizonY) / 30.0f;
            tt = ClampF(tt, 0.0f, 1.0f);
            a = (BYTE)Ftoi(255.f * tt);
        }

        DWORD c = L.color;
        BYTE r = (BYTE)((c >> 16) & 0xFF);
        BYTE g = (BYTE)((c >> 8) & 0xFF);
        BYTE b = (BYTE)(c & 0xFF);

        // Track lowest on-screen name for VU flash
        if (L.type == LT_Name && y > lowestNameY && y < SCREEN_H)
        {
            lowestNameY = y;
            lowestNameLine = i;
        }

        DrawText(x, y, L.text, sFinal, ARGB(a, r, g, b));

        y += gap;
    }

    // VU flash: re-draw lowest name line slightly brighter
    if (lowestNameLine >= 0 && vuFlash > 0)
    {
        const CreditLine& L = s_lines[lowestNameLine];

        float scale, pull;
        // Recompute y for that line — easier to just use lowestNameY
        GetPerspectiveForY(lowestNameY, &scale, &pull);
        float sFinal = scale;

        const float w = MeasureTextWidth(L.text, sFinal);
        float x = s_centerX - (w * 0.5f);
        x = s_centerX + (x - s_centerX) * pull;

        DWORD c = L.color;
        int r = (int)((c >> 16) & 0xFF) + vuFlash; if (r > 255) r = 255;
        int g = (int)((c >> 8) & 0xFF) + vuFlash; if (g > 255) g = 255;
        int b = (int)(c & 0xFF) + vuFlash; if (b > 255) b = 255;

        // Draw additive flash pass on top
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);

        DrawText(x, lowestNameY, L.text, sFinal,
            ARGB((BYTE)vuFlash * 3, (BYTE)r, (BYTE)g, (BYTE)b));

        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }

    // -------------------------------------------------------------------------
    // Title hold overlay — drawn last so it's always on top while fading
    // -------------------------------------------------------------------------
    if (titleHoldAlpha > 0 && s_lines[0].text)
    {
        // Large, centred, slightly above middle
        float sFinal = s_baseScale * 1.6f;
        const float w = MeasureTextWidth(s_lines[0].text, sFinal);
        float x = s_centerX - (w * 0.5f);
        float ty = SCREEN_H * 0.38f;

        DWORD c = s_lines[0].color;
        BYTE r = (BYTE)((c >> 16) & 0xFF);
        BYTE g = (BYTE)((c >> 8) & 0xFF);
        BYTE b = (BYTE)(c & 0xFF);

        // Restore normal blend for title text
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

        DrawText(x, ty, s_lines[0].text, sFinal, ARGB(titleHoldAlpha, r, g, b));
    }

    End2DTextStates();
}