// PostFXScene.cpp  — CRT Signal Corruption
// RXDK / Original Xbox — DX8 fixed-function pipeline
// 15 second scene, no shaders, no render targets.
//
// Concept:
//   A clean BIOS-style screen is displayed, then progressively eaten by
//   analog CRT signal corruption — scanlines, horizontal tears, chromatic
//   aberration, barrel warp, static bursts, phosphor burn, and finally a
//   catastrophic signal collapse to a dying horizontal line.
//
// Scene arc (15s):
//   0- 3s  CLEAN    — stable BIOS screen, slow scanline sweep, very faint noise
//   3- 7s  ONSET    — occasional horizontal tears, chromatic fringing starts
//   7-11s  DECAY    — warp grid distorts content, static blocks, color bleeding
//  11-15s  COLLAPSE — vignette closes, screen tears apart, final white line shrink
//
// All effects:
//   - BIOS layout redrawn from scratch (no dependency on BiosScreen.cpp)
//   - Scanline overlay: dark bands sweeping downward each frame
//   - Horizontal tear: random rows displaced left/right, fade in/out
//   - Chromatic aberration: R/G/B quads drawn offset (additive)
//   - Warp grid: 64x48 mesh, barrel distortion grows over time
//   - Phosphor glow: additive bloom pass over center region
//   - Static blocks: random bright noise rectangles
//   - Signal collapse: white horizontal line contracts to nothing at t=15s
//
// Architecture: DrawPrimitiveUP throughout, one dynamic VB for warp grid.
// All text via font.cpp DrawText. wsprintfA for number formatting.

#include "PostFXScene.h"
#include "font.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// ─────────────────────────────────────────────────────────────────────────────
//  Screen constants
// ─────────────────────────────────────────────────────────────────────────────
static const float SW = 640.0f;
static const float SH = 480.0f;
static const float CX = SW * 0.5f;
static const float CY = SH * 0.5f;

// ─────────────────────────────────────────────────────────────────────────────
//  Scene arc
// ─────────────────────────────────────────────────────────────────────────────
static const float ARC_ONSET = 3.0f;
static const float ARC_DECAY = 7.0f;
static const float ARC_COLLAPSE = 11.0f;
static const float ARC_END = 15.0f;

static inline float PhaseT(float t, float start, float end)
{
    if (t <= start) return 0.f;
    float f = (t - start) / (end - start);
    return f > 1.f ? 1.f : f;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Vertex types
// ─────────────────────────────────────────────────────────────────────────────
struct V2D { float x, y, z, rhw; DWORD color; };
#define FVF_V2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

// ─────────────────────────────────────────────────────────────────────────────
//  Warp grid
// ─────────────────────────────────────────────────────────────────────────────
static const int GRID_X = 64;
static const int GRID_Y = 48;
static const int NVX = GRID_X + 1;
static const int NVY = GRID_Y + 1;

static LPDIRECT3DVERTEXBUFFER8 s_gridVB = nullptr;
static LPDIRECT3DINDEXBUFFER8  s_gridIB = nullptr;
static int                     s_numVerts = 0;
static int                     s_numIndices = 0;

// ─────────────────────────────────────────────────────────────────────────────
//  RNG
// ─────────────────────────────────────────────────────────────────────────────
static unsigned s_rng = 0xDEADB33F;
static inline unsigned Rng()
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}
static inline float RngF() { return (float)(Rng() & 0xFFFF) / 65535.f; }

// ─────────────────────────────────────────────────────────────────────────────
//  Math helpers
// ─────────────────────────────────────────────────────────────────────────────
static inline float Clamp01(float x) { return x < 0.f ? 0.f : x > 1.f ? 1.f : x; }
static inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

static inline BYTE F2B(float f)
{
    f = f < 0.f ? 0.f : f > 1.f ? 1.f : f;
    float v = f * 255.f;
    return (v >= 255.f) ? (BYTE)255u : (BYTE)(unsigned)v;
}

static __forceinline int Ftoi(float f)
{
    int i;
    __asm { fld f
    fistp i }
    return i;
}

// ─────────────────────────────────────────────────────────────────────────────
//  2D draw helpers
// ─────────────────────────────────────────────────────────────────────────────
static void SetState2D(bool additive = false)
{
    g_pDevice->SetVertexShader(FVF_V2D);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    if (additive)
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    else
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
}

static void FillRect(float x0, float y0, float x1, float y1, DWORD col)
{
    V2D v[4] =
    {
        { x0, y0, 0.f, 1.f, col },
        { x1, y0, 0.f, 1.f, col },
        { x0, y1, 0.f, 1.f, col },
        { x1, y1, 0.f, 1.f, col },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V2D));
}

static void FillRectGrad(float x0, float y0, float x1, float y1, DWORD cTop, DWORD cBot)
{
    V2D v[4] =
    {
        { x0, y0, 0.f, 1.f, cTop },
        { x1, y0, 0.f, 1.f, cTop },
        { x0, y1, 0.f, 1.f, cBot },
        { x1, y1, 0.f, 1.f, cBot },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V2D));
}

static void HLine(float y, float x0, float x1, DWORD col)
{
    FillRect(x0, y, x1, y + 1.f, col);
}

// Text width helper (6px per char at scale 1.0)
static float TW(const char* s, float sc)
{
    int n = 0; while (s[n]) n++;
    return (float)n * 6.f * sc;
}

static void DrawTextR(float rx, float y, const char* s, float sc, DWORD col)
{
    DrawText(rx - TW(s, sc), y, s, sc, col);
}

// ─────────────────────────────────────────────────────────────────────────────
//  BIOS colors
// ─────────────────────────────────────────────────────────────────────────────
static const DWORD COL_BG = D3DCOLOR_XRGB(10, 10, 26);
static const DWORD COL_BAR_TOP = D3DCOLOR_XRGB(26, 42, 90);
static const DWORD COL_BAR_BOT = D3DCOLOR_XRGB(26, 26, 42);
static const DWORD COL_WHITE = D3DCOLOR_XRGB(220, 220, 220);
static const DWORD COL_CYAN = D3DCOLOR_XRGB(80, 220, 255);
static const DWORD COL_YELLOW = D3DCOLOR_XRGB(255, 220, 60);
static const DWORD COL_GRAY = D3DCOLOR_XRGB(130, 130, 150);
static const DWORD COL_GREEN = D3DCOLOR_XRGB(80, 220, 100);
static const DWORD COL_BORDER = D3DCOLOR_XRGB(50, 80, 160);

static const float TOP_BAR_H = 32.f;
static const float BOT_BAR_H = 30.f;
static const float BOT_BAR_Y = SH - BOT_BAR_H;
static const float CONTENT_Y = TOP_BAR_H + 18.f;
static const float TS = 1.5f;   // content text scale
static const float LM = 32.f;   // left margin
static const float VM = 160.f;  // value column X

// ─────────────────────────────────────────────────────────────────────────────
//  Draw the clean BIOS layout (no corruption applied here)
//  Called first every frame — corruption layers on top
// ─────────────────────────────────────────────────────────────────────────────
static void DrawBiosLayout(float t)
{
    SetState2D(false);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    // Background
    FillRect(0.f, 0.f, SW, SH, COL_BG);

    // Top bar gradient
    FillRectGrad(0.f, 0.f, SW, TOP_BAR_H,
        D3DCOLOR_XRGB(30, 55, 110),
        D3DCOLOR_XRGB(18, 35, 75));
    HLine(TOP_BAR_H, 0.f, SW, COL_BORDER);
    HLine(TOP_BAR_H + 1.f, 0.f, SW, D3DCOLOR_XRGB(60, 100, 200));

    // Bottom bar
    HLine(BOT_BAR_Y - 1.f, 0.f, SW, COL_BORDER);
    FillRectGrad(0.f, BOT_BAR_Y, SW, SH,
        D3DCOLOR_XRGB(18, 18, 35),
        D3DCOLOR_XRGB(10, 10, 22));

    // ── Text ──
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetVertexShader(FVF_V2D);
    g_pDevice->SetTexture(0, NULL);

    // Top bar version string
    float barTextY = (TOP_BAR_H - 7.f * 1.3f) * 0.5f;
    DrawTextR(SW - 10.f, barTextY, "XBIOS v2.0  (C) 2025", 1.3f, COL_WHITE);

    // Content rows
    float y = CONTENT_Y;

    DrawText(LM, y, "CPU TYPE    :", TS, COL_GRAY);
    DrawText(VM, y, "Xbox PIII Coppermine  733MHz", TS, COL_WHITE);
    y += 18.f;

    DrawText(LM, y, "CO-PROCESSOR:", TS, COL_GRAY);
    DrawText(VM, y, "Installed", TS, COL_GREEN);
    y += 18.f + 10.f;

    // Memory test — count animates in clean phase then holds
    {
        int totalKB = 65536;
        int countKB = totalKB;
        float memT = t / ARC_ONSET;  // animates over first 3s
        if (memT < 1.f)
        {
            float v = memT * (float)totalKB;
            countKB = Ftoi(v);
            if (countKB < 0)       countKB = 0;
            if (countKB > totalKB) countKB = totalKB;
        }
        char buf[32];
        wsprintfA(buf, "%dK", countKB);
        DrawText(LM, y, "MEMORY TEST :", TS, COL_GRAY);
        DrawText(VM, y, buf, TS, COL_CYAN);
        if (t >= 2.0f)
        {
            float okX = VM + TW(buf, TS) + 8.f;
            DrawText(okX, y, "OK", TS, COL_GREEN);
        }
    }
    y += 18.f + 10.f;

    DrawText(LM, y, "IDE CH 0    :", TS, COL_GRAY);
    DrawText(VM, y, "HDD  WD800BB  80.0GB  UDMA5", TS, COL_WHITE);
    y += 18.f;

    DrawText(LM, y, "IDE CH 1    :", TS, COL_GRAY);
    DrawText(VM, y, "DVD  Thomson  TGM600  [MASTER]", TS, COL_WHITE);
    y += 18.f;

    DrawText(LM, y, "BOOT DEVICE :", TS, COL_GRAY);
    DrawText(VM, y, "HDD  [C:]", TS, COL_CYAN);
    y += 18.f + 10.f;

    DrawText(LM, y, "USB PORTS   :", TS, COL_GRAY);
    DrawText(VM, y, "4x  USB1.1  OK", TS, COL_GREEN);
    y += 18.f;

    DrawText(LM, y, "AV OUTPUT   :", TS, COL_GRAY);
    DrawText(VM, y, "Composite / S-Video / HDTV", TS, COL_WHITE);
    y += 18.f + 20.f;

    // Initializing line (appears at t >= 2.2s)
    if (t >= 2.2f)
    {
        bool blink = (Ftoi(t * 2.f) & 1) == 0;
        DrawText(LM, y, "Initializing system...", TS, COL_GRAY);
        if (blink)
        {
            float cx = LM + TW("Initializing system...", TS);
            g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            FillRect(cx, y, cx + TS * 4.f, y + 7.f * TS, COL_GRAY);
            g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        }
    }

    // Bottom bar
    float botY = BOT_BAR_Y + (BOT_BAR_H - 7.f * 1.3f) * 0.5f;
    DrawText(LM, botY, "[A] Continue", 1.3f, COL_YELLOW);
    float bx = LM + TW("[A] Continue", 1.3f) + 10.f;
    DrawText(bx, botY, "[B] Exit to Dashboard", 1.3f, COL_YELLOW);

    // Progress bar (fills over 15s then holds full)
    {
        float fill = Clamp01(t / ARC_END);
        float barRX = SW - 12.f;
        float barW = 100.f;
        float barH = 7.f;
        float barX0 = barRX - barW;
        float barY0 = botY + (7.f * 1.3f - barH) * 0.5f;
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        FillRect(barX0, barY0, barRX, barY0 + barH, D3DCOLOR_XRGB(20, 20, 40));
        float fw = fill * barW;
        if (fw > 1.f)
            FillRectGrad(barX0, barY0, barX0 + fw, barY0 + barH,
                D3DCOLOR_XRGB(60, 160, 255), D3DCOLOR_XRGB(30, 90, 180));
        HLine(barY0, barX0, barRX, COL_BORDER);
        HLine(barY0 + barH, barX0, barRX, COL_BORDER);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Scanline overlay
//  Dark horizontal bands sweep downward continuously.
//  Intensity ramps up from onset.
// ─────────────────────────────────────────────────────────────────────────────
static void DrawScanlines(float t)
{
    float onsetT = PhaseT(t, 0.f, ARC_ONSET);
    float alpha = 0.08f + onsetT * 0.25f;  // 8%..33% dark

    SetState2D(false);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);

    DWORD col = D3DCOLOR_ARGB(F2B(alpha), 0, 0, 0);

    // Every other row — sweep offset by time so they roll slowly
    float rollOff = fmodf(t * 18.f, 4.f);  // 4px period scroll
    for (float y = rollOff; y < SH; y += 4.f)
        FillRect(0.f, y, SW, y + 2.f, col);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Horizontal tears
//  Random rows are displaced horizontally. Onset phase only.
// ─────────────────────────────────────────────────────────────────────────────
static const int TEAR_MAX = 12;
struct Tear { float y; float dx; float life; float maxLife; };
static Tear  s_tears[TEAR_MAX];
static int   s_tearCount = 0;
static float s_tearTimer = 0.f;

static void UpdateTears(float t, float dt)
{
    float onsetT = PhaseT(t, ARC_ONSET, ARC_DECAY);
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);

    // Spawn rate: 0 before onset, peaks mid-decay, stays high in collapse
    float spawnRate = onsetT * 2.f + decayT * 6.f;  // tears/sec
    s_tearTimer += dt * spawnRate;
    while (s_tearTimer >= 1.f && s_tearCount < TEAR_MAX)
    {
        s_tearTimer -= 1.f;
        Tear& tr = s_tears[s_tearCount++];
        tr.y = RngF() * SH;
        float mag = 8.f + RngF() * 60.f * (1.f + decayT * 2.f);
        tr.dx = (Rng() & 1) ? mag : -mag;
        tr.maxLife = 0.05f + RngF() * 0.2f;
        tr.life = tr.maxLife;
    }

    // Age tears
    for (int i = 0; i < s_tearCount; )
    {
        s_tears[i].life -= dt;
        if (s_tears[i].life <= 0.f)
        {
            s_tears[i] = s_tears[--s_tearCount];
        }
        else ++i;
    }
}

static void DrawTears(float t)
{
    if (s_tearCount == 0) return;

    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);
    float collapseT = PhaseT(t, ARC_COLLAPSE, ARC_END);

    SetState2D(false);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    for (int i = 0; i < s_tearCount; ++i)
    {
        const Tear& tr = s_tears[i];
        float fade = tr.life / tr.maxLife;
        float h = 2.f + decayT * 8.f + collapseT * 12.f;

        // Source row: sample a horizontal slice from the BIOS bg color
        // We draw a slightly offset copy of the background tinted with noise
        float nx = tr.dx * fade;
        BYTE  noise = F2B(0.6f + 0.4f * RngF());
        DWORD col = D3DCOLOR_XRGB(noise >> 1, noise >> 2, noise);

        // Overwrite the row with a shifted color band
        FillRect(0.f, tr.y, SW, tr.y + h, COL_BG);  // erase
        FillRect(nx, tr.y, SW + nx, tr.y + h, col);  // shifted tint
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Chromatic aberration
//  Additive R/G/B passes of the screen content, slightly offset.
//  Drawn as thin full-screen horizontal strips rather than re-rendering text.
//  Grows stronger in decay/collapse.
// ─────────────────────────────────────────────────────────────────────────────
static void DrawChromaticAberration(float t)
{
    float onsetT = PhaseT(t, ARC_ONSET, ARC_DECAY);
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);
    float collapseT = PhaseT(t, ARC_COLLAPSE, ARC_END);

    float shift = onsetT * 3.f + decayT * 8.f + collapseT * 18.f;
    float alpha = onsetT * 0.15f + decayT * 0.30f + collapseT * 0.45f;
    if (shift < 0.5f) return;

    SetState2D(true);  // additive

    BYTE  a = F2B(alpha);
    float pulse = 0.7f + 0.3f * sinf(t * 7.3f);
    BYTE  ap = F2B(alpha * pulse);

    // R channel: shift right
    DWORD colR = D3DCOLOR_ARGB(ap, a, 0, 0);
    // G channel: shift left
    DWORD colG = D3DCOLOR_ARGB(ap, 0, a, 0);
    // B channel: shift down
    DWORD colB = D3DCOLOR_ARGB(ap, 0, 0, a);

    // Draw thin horizontal bands across the whole screen
    // each one slightly offset per channel — simulates RGB separation
    const float BAND = 4.f;
    for (float y = 0.f; y < SH; y += BAND)
    {
        float ph = sinf(y * 0.04f + t * 2.1f);  // per-row phase
        float rOff = shift * (1.f + ph * 0.3f);
        float gOff = shift * (0.6f + ph * 0.2f);

        FillRect(rOff, y, SW + rOff, y + BAND, colR);
        FillRect(-gOff, y, SW - gOff, y + BAND, colG);
        FillRect(0.f, y + shift * 0.4f, SW, y + BAND + shift * 0.4f, colB);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Warp grid — barrel distortion grows in decay phase
// ─────────────────────────────────────────────────────────────────────────────
static void UpdateWarpGrid(float t)
{
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);
    float collapseT = PhaseT(t, ARC_COLLAPSE, ARC_END);

    // Barrel strength: 0 in clean/onset, grows to strong distortion in collapse
    float barrel = decayT * 0.35f + collapseT * 0.55f;
    // Rolling horizontal wave in decay
    float wave = decayT * 8.f + collapseT * 20.f;

    V2D* v = nullptr;
    s_gridVB->Lock(0, 0, (BYTE**)&v, 0);
    int idx = 0;
    for (int y = 0; y <= GRID_Y; ++y)
    {
        float fy = (float)y / GRID_Y;
        float py = fy * SH;
        float dy = (py - CY) / CY;  // -1..1

        for (int x = 0; x <= GRID_X; ++x)
        {
            float fx = (float)x / GRID_X;
            float px = fx * SW;
            float dx = (px - CX) / CX;  // -1..1

            float r2 = dx * dx + dy * dy;

            // Barrel distortion: push outward from center
            float bFactor = 1.f + barrel * r2;
            float wx = CX + dx * CX * bFactor;
            float wy = CY + dy * CY * bFactor;

            // Horizontal rolling wave (simulates sync loss)
            float waveOff = wave * sinf(fy * 6.2832f * 3.f + t * 8.f);
            wx += waveOff;

            // Vertical judder in collapse
            if (collapseT > 0.f)
            {
                float judder = collapseT * 12.f * sinf(fy * 20.f + t * 15.f);
                wy += judder;
            }

            // Vertex color: dim grid tint — more visible in decay
            BYTE dimness = F2B(0.05f + decayT * 0.08f);
            v[idx++] = { wx, wy, 0.f, 1.f, D3DCOLOR_XRGB(dimness, dimness, dimness * 2) };
        }
    }
    s_gridVB->Unlock();
}

static void DrawWarpGrid(float t)
{
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);
    if (decayT < 0.01f) return;  // not visible yet

    // Modulate the grid color via TEXTUREFACTOR — dim blue tint
    float a = decayT * 0.6f;
    g_pDevice->SetVertexShader(FVF_V2D);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,
        D3DCOLOR_XRGB(F2B(a * 0.6f), F2B(a * 0.7f), F2B(a)));

    g_pDevice->SetStreamSource(0, s_gridVB, sizeof(V2D));
    g_pDevice->SetIndices(s_gridIB, 0);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST,
        0, s_numVerts, 0, s_numIndices / 3);

    // Restore
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Static noise blocks
//  Random bright rectangles appear in decay/collapse
// ─────────────────────────────────────────────────────────────────────────────
static void DrawStaticBlocks(float t)
{
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);
    float collapseT = PhaseT(t, ARC_COLLAPSE, ARC_END);
    float intensity = decayT * 0.4f + collapseT * 0.8f;
    if (intensity < 0.01f) return;

    int count = Ftoi(intensity * 18.f);

    SetState2D(false);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    for (int i = 0; i < count; ++i)
    {
        float bx = RngF() * SW;
        float by = RngF() * SH;
        float bw = 4.f + RngF() * 40.f;
        float bh = 2.f + RngF() * 8.f;

        // Color: white noise, phosphor green, or signal blue
        DWORD col;
        unsigned pick = Rng() % 3;
        if (pick == 0)
        {
            BYTE b = F2B(0.5f + RngF() * 0.5f);
            col = D3DCOLOR_XRGB(b, b, b);
        }
        else if (pick == 1)
        {
            col = D3DCOLOR_XRGB(30, F2B(0.4f + RngF() * 0.6f), 30);
        }
        else
        {
            col = D3DCOLOR_XRGB(30, 80, F2B(0.5f + RngF() * 0.5f));
        }
        FillRect(bx, by, bx + bw, by + bh, col);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Phosphor glow — additive bloom around text content area
// ─────────────────────────────────────────────────────────────────────────────
static void DrawPhosphorGlow(float t)
{
    float onsetT = PhaseT(t, 0.f, ARC_ONSET);
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);

    // Gentle glow in clean phase, colour-shifts to green in decay (phosphor burn)
    float glowA = 0.04f + onsetT * 0.04f;
    float greenBias = decayT;

    SetState2D(true);  // additive

    float pulse = 0.85f + 0.15f * sinf(t * 1.4f);
    BYTE  ar = F2B(glowA * pulse * (1.f - greenBias * 0.6f));
    BYTE  ag = F2B(glowA * pulse * (1.f + greenBias * 1.5f));
    BYTE  ab = F2B(glowA * pulse * (1.f - greenBias * 0.4f));
    DWORD col = D3DCOLOR_ARGB(255, ar, ag, ab);

    // Content area glow band
    FillRectGrad(0.f, CONTENT_Y - 10.f, SW, BOT_BAR_Y,
        D3DCOLOR_ARGB(0, ar, ag, ab),
        col);
    FillRectGrad(0.f, BOT_BAR_Y, SW, SH,
        col,
        D3DCOLOR_ARGB(0, ar, ag, ab));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Vignette — closes in during collapse
// ─────────────────────────────────────────────────────────────────────────────
static void DrawVignette(float t)
{
    float decayT = PhaseT(t, ARC_DECAY, ARC_COLLAPSE);
    float collapseT = PhaseT(t, ARC_COLLAPSE, ARC_END);

    float vigA = 0.55f + decayT * 0.20f + collapseT * 0.25f;
    float vigR = 0.88f - decayT * 0.10f - collapseT * 0.35f;  // closes in

    const int VSEGS = 16;
    V2D fan[VSEGS + 2];
    fan[0] = { CX, CY, 0.f, 1.f, D3DCOLOR_ARGB(0, 0, 0, 0) };
    for (int i = 0; i <= VSEGS; ++i)
    {
        float a = (float)i / VSEGS * 6.2832f;
        BYTE  va = F2B(vigA);
        fan[i + 1] = {
            CX + cosf(a) * (SW * vigR),
            CY + sinf(a) * (SH * vigR),
            0.f, 1.f, D3DCOLOR_ARGB(va, 0, 0, 0)
        };
    }

    SetState2D(false);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, VSEGS, fan, sizeof(V2D));
}

// ─────────────────────────────────────────────────────────────────────────────
//  Signal collapse — white horizontal line contracts to nothing
//  Only active in the final collapse phase
// ─────────────────────────────────────────────────────────────────────────────
static void DrawSignalCollapse(float t)
{
    float collapseT = PhaseT(t, ARC_COLLAPSE, ARC_END);
    if (collapseT < 0.01f) return;

    // Black overlay that darkens everything as we approach end
    float blackA = collapseT * collapseT;
    SetState2D(false);
    DWORD blackCol = D3DCOLOR_ARGB(F2B(blackA * 0.9f), 0, 0, 0);
    FillRect(0.f, 0.f, SW, SH, blackCol);

    // White horizontal line — full width at collapse start, shrinks vertically
    float lineH = Lerp(12.f, 0.f, collapseT);
    float lineA = Clamp01(1.f - collapseT * 0.6f);
    float lineY = CY - lineH * 0.5f;

    if (lineH > 0.2f)
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        BYTE  la = F2B(lineA);
        DWORD lc = D3DCOLOR_XRGB(la, la, la);
        FillRect(0.f, lineY, SW, lineY + lineH, lc);

        // Bright core of the line
        float coreH = lineH * 0.3f;
        FillRect(0.f, lineY + lineH * 0.35f, SW, lineY + lineH * 0.35f + coreH,
            D3DCOLOR_XRGB(255, 255, 255));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Grid buffer creation
// ─────────────────────────────────────────────────────────────────────────────
static void CreateGridBuffers()
{
    s_numVerts = NVX * NVY;
    s_numIndices = GRID_X * GRID_Y * 6;

    g_pDevice->CreateVertexBuffer(
        s_numVerts * sizeof(V2D),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY,
        FVF_V2D, D3DPOOL_DEFAULT, &s_gridVB);

    g_pDevice->CreateIndexBuffer(
        s_numIndices * sizeof(WORD),
        0, D3DFMT_INDEX16, D3DPOOL_MANAGED, &s_gridIB);

    WORD* ib = nullptr;
    s_gridIB->Lock(0, 0, (BYTE**)&ib, 0);
    int ii = 0;
    for (int y = 0; y < GRID_Y; ++y)
        for (int x = 0; x < GRID_X; ++x)
        {
            WORD i0 = (WORD)(y * NVX + x);
            WORD i1 = i0 + 1, i2 = i0 + NVX, i3 = i2 + 1;
            ib[ii++] = i0; ib[ii++] = i2; ib[ii++] = i3;
            ib[ii++] = i0; ib[ii++] = i3; ib[ii++] = i1;
        }
    s_gridIB->Unlock();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Public interface
// ─────────────────────────────────────────────────────────────────────────────
static float s_prevTime = 0.f;
static float s_localTime = 0.f;  // scene-local seconds, resets to 0 on Init

void PostFXScene_Init()
{
    if (!g_pDevice) return;

    if (s_gridVB) { s_gridVB->Release(); s_gridVB = nullptr; }
    if (s_gridIB) { s_gridIB->Release(); s_gridIB = nullptr; }

    CreateGridBuffers();

    s_prevTime = 0.f;
    s_localTime = 0.f;
    s_tearCount = 0;
    s_tearTimer = 0.f;
    s_rng ^= GetTickCount();
}

void PostFXScene_Shutdown()
{
    if (s_gridVB) { s_gridVB->Release(); s_gridVB = nullptr; }
    if (s_gridIB) { s_gridIB->Release(); s_gridIB = nullptr; }

    // Full render state reset
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
}

void PostFXScene_Render(float demoTime)
{
    if (!g_pDevice || !s_gridVB || !s_gridIB) return;

    const float dt = (demoTime > s_prevTime && demoTime - s_prevTime < 0.1f)
        ? demoTime - s_prevTime : 0.016f;
    s_prevTime = demoTime;
    s_localTime += dt;

    const float t = s_localTime;  // all phase logic uses scene-local time

    // Update simulation
    UpdateTears(t, dt);
    UpdateWarpGrid(t);

    // ── Layer 1: clean BIOS layout ────────────────────────────────────────────
    DrawBiosLayout(t);

    // ── Layer 2: scanline overlay (always on, intensifies with time) ──────────
    DrawScanlines(t);

    // ── Layer 3: horizontal tears (onset → collapse) ──────────────────────────
    DrawTears(t);

    // ── Layer 4: warp grid overlay (decay → collapse) ─────────────────────────
    DrawWarpGrid(t);

    // ── Layer 5: chromatic aberration (onset → collapse) ──────────────────────
    DrawChromaticAberration(t);

    // ── Layer 6: static noise blocks (decay → collapse) ───────────────────────
    DrawStaticBlocks(t);

    // ── Layer 7: phosphor glow ────────────────────────────────────────────────
    DrawPhosphorGlow(t);

    // ── Layer 8: vignette ─────────────────────────────────────────────────────
    DrawVignette(t);

    // ── Layer 9: signal collapse line (collapse phase only) ───────────────────
    DrawSignalCollapse(t);

    // Restore alpha blend off for next scene
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}