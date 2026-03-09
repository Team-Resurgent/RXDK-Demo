// BiosScreen.cpp - Mock late-2000s BIOS splash screen
//
// Blocking call from main() - runs ~6 seconds then returns.
// Self-contained: owns its DDS loader and all rendering.
// Uses font.cpp DrawText for all text.
//
// Layout:
//   [xb.dds logo]  top-left 20px margin
//   Top bar:       full-width dark strip, version string right-aligned
//   ---------------------------------------------------------------
//   Content rows (monospaced BIOS style):
//     CPU detection
//     Memory test  (count animates up to full RAM size)
//     IDE channel detect
//     Boot device
//     Blank
//     Initializing...
//   ---------------------------------------------------------------
//   Bottom bar:    [A] Continue   [B] Exit to Dashboard
//
// Colors: classic Award/AMI BIOS palette
//   Background : very dark navy  #0A0A1A
//   Top bar    : dark steel blue #1A2A5A  text: bright white
//   Content    : white text on dark bg
//   Highlight  : bright cyan for values / detected items
//   Bottom bar : dark gray #1A1A2A  text: yellow prompt

#include "BiosScreen.h"
#include "font.h"

#include <xtl.h>
#include <xgraphics.h>
#include <string.h>
#include <stdlib.h>

extern LPDIRECT3DDEVICE8  g_pDevice;

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

static const float SW = 640.0f;
static const float SH = 480.0f;

static const DWORD COL_BG = D3DCOLOR_XRGB(10, 10, 26);   // near-black navy
static const DWORD COL_BAR_TOP = D3DCOLOR_XRGB(26, 42, 90);   // dark steel blue
static const DWORD COL_BAR_BOT = D3DCOLOR_XRGB(26, 26, 42);   // dark gray-blue
static const DWORD COL_WHITE = D3DCOLOR_XRGB(220, 220, 220);
static const DWORD COL_CYAN = D3DCOLOR_XRGB(80, 220, 255);   // detected values
static const DWORD COL_YELLOW = D3DCOLOR_XRGB(255, 220, 60);    // prompt text
static const DWORD COL_GRAY = D3DCOLOR_XRGB(130, 130, 150);   // dim label
static const DWORD COL_GREEN = D3DCOLOR_XRGB(80, 220, 100);   // OK status
static const DWORD COL_BORDER = D3DCOLOR_XRGB(50, 80, 160);   // separator lines

static const float TOP_BAR_H = 32.0f;
static const float BOT_BAR_H = 30.0f;
static const float BOT_BAR_Y = SH - BOT_BAR_H;
static const float CONTENT_Y = TOP_BAR_H + 18.0f;
static const float TEXT_SCALE = 1.5f;
static const float LINE_H = 18.0f;   // comfortable line spacing at scale 1.5
static const float GROUP_GAP = 10.0f;   // extra gap between logical groups

// Duration
static const DWORD BIOS_DURATION_MS = 6500;

// -----------------------------------------------------------------------------
// Ftoi (avoid __ftol2_sse)
// -----------------------------------------------------------------------------

static __declspec(noinline) int Ftoi(float f)
{
    int i;
    __asm {
        fld   f
        fistp i
    }
    return i;
}

// -----------------------------------------------------------------------------
// 2D vertex helpers
// -----------------------------------------------------------------------------

struct BV { float x, y, z, rhw; DWORD c; };

static void FillRect(float x0, float y0, float x1, float y1, DWORD col)
{
    BV v[4] =
    {
        { x0, y0, 0.f, 1.f, col },
        { x1, y0, 0.f, 1.f, col },
        { x0, y1, 0.f, 1.f, col },
        { x1, y1, 0.f, 1.f, col },
    };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BV));
}

static void FillRectGrad(float x0, float y0, float x1, float y1, DWORD cTop, DWORD cBot)
{
    BV v[4] =
    {
        { x0, y0, 0.f, 1.f, cTop },
        { x1, y0, 0.f, 1.f, cTop },
        { x0, y1, 0.f, 1.f, cBot },
        { x1, y1, 0.f, 1.f, cBot },
    };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BV));
}

// 1px horizontal line
static void HLine(float y, float x0, float x1, DWORD col)
{
    FillRect(x0, y, x1, y + 1.f, col);
}

// -----------------------------------------------------------------------------
// DDS loader (uncompressed A8R8G8B8 only, square power-of-two)
// -----------------------------------------------------------------------------

#pragma pack(push, 1)
struct BDDS_PF
{
    DWORD size, flags, fourCC, rgbBitCount;
    DWORD rMask, gMask, bMask, aMask;
};
struct BDDS_HDR
{
    DWORD size, flags, height, width;
    DWORD pitchOrLinearSize, depth, mipMapCount;
    DWORD reserved1[11];
    BDDS_PF ddspf;
    DWORD caps, caps2, caps3, caps4, reserved2;
};
#pragma pack(pop)

static LPDIRECT3DTEXTURE8 BiosLoadDDS(const char* path, int& outW, int& outH)
{
    outW = outH = 0;
    if (!g_pDevice || !path) return NULL;

    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;

    DWORD br = 0;
    DWORD magic = 0;
    ReadFile(hf, &magic, 4, &br, NULL);
    if (br != 4 || magic != 0x20534444) { CloseHandle(hf); return NULL; }

    BDDS_HDR hdr;
    ReadFile(hf, &hdr, sizeof(hdr), &br, NULL);
    if (br != sizeof(hdr) || hdr.size != 124) { CloseHandle(hf); return NULL; }

    // Accept only uncompressed 32-bit ARGB
    const DWORD DDPF_RGB = 0x40;
    const DWORD DDPF_RGBA = 0x41;
    if (hdr.ddspf.rgbBitCount != 32 ||
        !(hdr.ddspf.flags & DDPF_RGB))
    {
        CloseHandle(hf); return NULL;
    }

    int w = (int)hdr.width;
    int h = (int)hdr.height;
    if (w <= 0 || h <= 0 || (w & (w - 1)) != 0) { CloseHandle(hf); return NULL; }

    DWORD bytes = (DWORD)(w * h * 4);
    BYTE* px = (BYTE*)malloc(bytes);
    if (!px) { CloseHandle(hf); return NULL; }

    ReadFile(hf, px, bytes, &br, NULL);
    CloseHandle(hf);
    if (br != bytes) { free(px); return NULL; }

    LPDIRECT3DTEXTURE8 tex = NULL;
    if (FAILED(g_pDevice->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, 0, &tex)))
    {
        free(px); return NULL;
    }

    D3DLOCKED_RECT lr;
    if (FAILED(tex->LockRect(0, &lr, NULL, 0)))
    {
        tex->Release(); free(px); return NULL;
    }

    XGSwizzleRect(px, w * 4, NULL, lr.pBits, w, h, NULL, 4);
    tex->UnlockRect(0);
    free(px);

    outW = w; outH = h;
    return tex;
}

// -----------------------------------------------------------------------------
// Logo draw (textured quad, alpha blend)
// -----------------------------------------------------------------------------

struct BVT { float x, y, z, rhw; DWORD c; float u, v; };

static void DrawLogo(LPDIRECT3DTEXTURE8 tex, float cx, float cy,
    float dispW, float dispH, BYTE alpha)
{
    if (!tex) return;

    float l = cx - dispW * 0.5f;
    float r = cx + dispW * 0.5f;
    float t = cy - dispH * 0.5f;
    float b = cy + dispH * 0.5f;
    DWORD col = D3DCOLOR_ARGB(alpha, 255, 255, 255);

    BVT v[4] =
    {
        { l, t, 0.f, 1.f, col, 0.f, 0.f },
        { r, t, 0.f, 1.f, col, 1.f, 0.f },
        { l, b, 0.f, 1.f, col, 0.f, 1.f },
        { r, b, 0.f, 1.f, col, 1.f, 1.f },
    };

    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
    g_pDevice->SetTexture(0, tex);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(BVT));

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// -----------------------------------------------------------------------------
// Text helpers (wrap DrawText with BIOS render state setup)
// -----------------------------------------------------------------------------

// After DrawLogo (which sets TEX1 shader), restore the plain
// XYZRHW+DIFFUSE shader that font.cpp and FillRect both expect.
static void BiosResetShader()
{
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

// Measure text width using the same 6px-per-char advance as font.cpp
static float TW(const char* s, float scale)
{
    if (!s) return 0.f;
    int n = 0;
    while (s[n]) n++;
    return (float)n * 6.0f * scale;
}

// Right-align text within a given right edge
static void DrawTextR(float rightX, float y, const char* s, float sc, DWORD col)
{
    DrawText(rightX - TW(s, sc), y, s, sc, col);
}

// Draw a label+value pair:  "LABEL : " dim,  "VALUE" bright
static void DrawLabelValue(float x, float y, const char* label,
    const char* value, float sc,
    DWORD labelCol, DWORD valueCol)
{
    DrawText(x, y, label, sc, labelCol);
    float lx = x + TW(label, sc);
    DrawText(lx, y, value, sc, valueCol);
}

// -----------------------------------------------------------------------------
// Integer-to-string helpers (no sprintf, no CRT)
// -----------------------------------------------------------------------------

static void IntToStr(int v, char* buf, int bufLen)
{
    // Simple unsigned decimal
    if (bufLen <= 1) return;
    if (v == 0) { buf[0] = '0'; buf[1] = '\0'; return; }

    char tmp[16];
    int n = 0;
    unsigned u = (unsigned)v;
    while (u > 0 && n < 15) { tmp[n++] = '0' + (u % 10); u /= 10; }

    int out = 0;
    for (int i = n - 1; i >= 0 && out < bufLen - 1; --i)
        buf[out++] = tmp[i];
    buf[out] = '\0';
}

// Concatenate two strings into buf
static void StrCat2(char* buf, int bufLen, const char* a, const char* b)
{
    int i = 0;
    while (*a && i < bufLen - 1) buf[i++] = *a++;
    while (*b && i < bufLen - 1) buf[i++] = *b++;
    buf[i] = '\0';
}

// -----------------------------------------------------------------------------
// Main BIOS render frame
// -----------------------------------------------------------------------------

static void BiosRenderFrame(LPDIRECT3DTEXTURE8 logoTex,
    int logoTexW, int logoTexH,
    DWORD elapsedMs, bool has128MB)
{
    if (!g_pDevice) return;

    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET,
        D3DCOLOR_XRGB(10, 10, 26), 1.f, 0);
    g_pDevice->BeginScene();

    // -----------------------------------------------------------------
    // Top bar gradient
    // -----------------------------------------------------------------
    FillRectGrad(0.f, 0.f, SW, TOP_BAR_H,
        D3DCOLOR_XRGB(30, 55, 110),
        D3DCOLOR_XRGB(18, 35, 75));
    HLine(TOP_BAR_H, 0.f, SW, COL_BORDER);

    // -----------------------------------------------------------------
    // Bottom bar
    // -----------------------------------------------------------------
    HLine(BOT_BAR_Y - 1.f, 0.f, SW, COL_BORDER);
    FillRectGrad(0.f, BOT_BAR_Y, SW, SH,
        D3DCOLOR_XRGB(18, 18, 35),
        D3DCOLOR_XRGB(10, 10, 22));

    // Thin accent line just below top bar
    HLine(TOP_BAR_H + 1.f, 0.f, SW, D3DCOLOR_XRGB(60, 100, 200));

    // -----------------------------------------------------------------
    // Logo (top-left, fixed size display area ~120x48)
    // -----------------------------------------------------------------
    const float LOGO_DISP_W = 120.f;
    const float LOGO_DISP_H = 48.f;
    const float LOGO_CX = 20.f + LOGO_DISP_W * 0.5f;
    const float LOGO_CY = TOP_BAR_H * 0.5f;

    DrawLogo(logoTex, LOGO_CX, LOGO_CY, LOGO_DISP_W, LOGO_DISP_H, 255);

    // -----------------------------------------------------------------
    // Top-bar text
    // -----------------------------------------------------------------
    BiosResetShader();

    // Version string right-aligned in top bar
    const float TS = 1.3f;   // top bar text scale
    float barTextY = (TOP_BAR_H - 7.f * TS) * 0.5f;

    DrawTextR(SW - 10.f, barTextY, "XBIOS v2.0  (C) 2025", TS, COL_WHITE);

    // Divider after logo in top bar
    FillRect(LOGO_CX + LOGO_DISP_W * 0.5f + 8.f, 4.f,
        LOGO_CX + LOGO_DISP_W * 0.5f + 9.f, TOP_BAR_H - 4.f,
        COL_BORDER);

    // -----------------------------------------------------------------
    // Content area
    // -----------------------------------------------------------------
    const float CS = TEXT_SCALE;   // content text scale
    const float LM = 32.f;        // left margin
    // Value column: must clear longest label "MEMORY TEST :" (13 chars x 6 x 1.5 = 117px) + gap
    const float VM = 160.f;       // value column X - well clear of all labels
    float y = CONTENT_Y;

    // --- Group 1: CPU ---
    DrawText(LM, y, "CPU TYPE    :", CS, COL_GRAY);
    DrawText(VM, y, "Xbox PIII Coppermine  733MHz", CS, COL_WHITE);
    y += LINE_H;

    DrawText(LM, y, "CO-PROCESSOR:", CS, COL_GRAY);
    DrawText(VM, y, "Installed", CS, COL_GREEN);
    y += LINE_H + GROUP_GAP;

    // --- Group 2: Memory test (count animates up over first 2 seconds) ---
    {
        int totalKB = has128MB ? 131072 : 65536;

        int countKB = totalKB;
        if (elapsedMs < 2000)
        {
            countKB = (totalKB * (int)elapsedMs) / 2000;
            if (countKB < 0)         countKB = 0;
            if (countKB > totalKB)   countKB = totalKB;
        }

        char countStr[16];
        IntToStr(countKB, countStr, sizeof(countStr));
        char memLine[32];
        StrCat2(memLine, sizeof(memLine), countStr, "K");

        DrawText(LM, y, "MEMORY TEST :", CS, COL_GRAY);
        DrawText(VM, y, memLine, CS, COL_CYAN);

        if (elapsedMs >= 2000)
        {
            float okX = VM + TW(memLine, CS) + 8.f;
            DrawText(okX, y, "OK", CS, COL_GREEN);
        }
    }
    y += LINE_H + GROUP_GAP;

    // --- Group 3: Storage ---
    DrawText(LM, y, "IDE CH 0    :", CS, COL_GRAY);
    DrawText(VM, y, "HDD  WD800BB  80.0GB  UDMA5", CS, COL_WHITE);
    y += LINE_H;

    DrawText(LM, y, "IDE CH 1    :", CS, COL_GRAY);
    DrawText(VM, y, "DVD  Thomson  TGM600  [MASTER]", CS, COL_WHITE);
    y += LINE_H;

    DrawText(LM, y, "BOOT DEVICE :", CS, COL_GRAY);
    DrawText(VM, y, "HDD  [C:]", CS, COL_CYAN);
    y += LINE_H + GROUP_GAP;

    // --- Group 4: I/O ---
    DrawText(LM, y, "USB PORTS   :", CS, COL_GRAY);
    DrawText(VM, y, "4x  USB1.1  OK", CS, COL_GREEN);
    y += LINE_H;

    DrawText(LM, y, "AV OUTPUT   :", CS, COL_GRAY);
    DrawText(VM, y, "Composite / S-Video / HDTV", CS, COL_WHITE);
    y += LINE_H + GROUP_GAP * 2.f;

    // --- Initializing message (appears after memory test) ---
    if (elapsedMs >= 2200)
    {
        bool blink = ((elapsedMs / 500) & 1) == 0;

        DrawText(LM, y, "Initializing system...", CS, COL_GRAY);
        if (blink)
        {
            float curX = LM + TW("Initializing system...", CS);
            FillRect(curX, y, curX + CS * 4.f, y + 7.f * CS, COL_GRAY);
        }
    }

    // -----------------------------------------------------------------
    // Bottom bar prompt
    // -----------------------------------------------------------------
    float botY = BOT_BAR_Y + (BOT_BAR_H - 7.f * 1.3f) * 0.5f;
    float botTS = 1.3f;

    // Left side: controller hints
    DrawText(LM, botY, "[A] Continue", botTS, COL_YELLOW);

    float bBX = LM + TW("[A] Continue", botTS) + 8.f * botTS;
    FillRect(bBX, botY + 1.f, bBX + 1.f, botY + 7.f * botTS - 1.f, COL_BORDER);
    bBX += 6.f * botTS;

    DrawText(bBX, botY, "[B] Exit to Dashboard", botTS, COL_YELLOW);

    // Right side: elapsed / total time bar (progress bar aesthetic)
    {
        float barRX = SW - 12.f;
        float barW = 100.f;
        float barH = 7.f;
        float barX0 = barRX - barW;
        float barY0 = botY + (7.f * botTS - barH) * 0.5f;

        // Track bg
        FillRect(barX0, barY0, barRX, barY0 + barH, D3DCOLOR_XRGB(20, 20, 40));

        // Fill - capped at duration (all integer, no float->int cast)
        int barWi = Ftoi(barW);   // 100
        int fillW = (barWi * (int)elapsedMs) / (int)BIOS_DURATION_MS;
        if (fillW < 0)     fillW = 0;
        if (fillW > barWi) fillW = barWi;

        if (fillW > 0)
            FillRectGrad(barX0, barY0, barX0 + (float)fillW, barY0 + barH,
                D3DCOLOR_XRGB(60, 160, 255),
                D3DCOLOR_XRGB(30, 90, 180));

        // Border
        HLine(barY0, barX0, barRX, COL_BORDER);
        HLine(barY0 + barH, barX0, barRX, COL_BORDER);
        FillRect(barX0, barY0, barX0 + 1.f, barY0 + barH, COL_BORDER);
        FillRect(barRX - 1.f, barY0, barRX, barY0 + barH, COL_BORDER);
    }

    // -----------------------------------------------------------------
    // Restore clean state
    // -----------------------------------------------------------------
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetTexture(0, NULL);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// -----------------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------------

void BiosScreen_Run()
{
    if (!g_pDevice) return;

    // Load logo
    int logoW = 0, logoH = 0;
    LPDIRECT3DTEXTURE8 logoTex = BiosLoadDDS("D:\\tex\\xb.dds", logoW, logoH);
    // logoTex may be NULL if file missing — DrawLogo handles that gracefully

    // Detect RAM (same threshold as IntroScene)
    MEMORYSTATUS ms;
    ms.dwLength = sizeof(ms);
    GlobalMemoryStatus(&ms);
    bool has128MB = (ms.dwTotalPhys >= 100 * 1024 * 1024);

    DWORD startMs = GetTickCount();

    while (true)
    {
        DWORD elapsed = GetTickCount() - startMs;

        if (elapsed >= BIOS_DURATION_MS)
            break;

        BiosRenderFrame(logoTex, logoW, logoH, elapsed, has128MB);
    }

    // Clean up
    if (logoTex)
    {
        logoTex->Release();
        logoTex = NULL;
    }

    // Final black frame before handing back to main
    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);
    g_pDevice->BeginScene();
    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}