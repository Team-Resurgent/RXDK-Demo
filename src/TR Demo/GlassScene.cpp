// GlassScene.cpp - Stained glass cathedral window  (RXDK / NV2A)
//
// Visual target: irregular mosaic stained glass with warm outer border,
// cool inner field, Team Resurgent script inlaid as thick outlined lettering.
//
// Technique:
//   - Voronoi-style irregular polygon panels via jittered seed points +
//     triangle-fan ear clipping (convex cells guaranteed by construction)
//   - Zone-based palette: warm amber/orange/red outer, cool blue/teal/cyan
//     inner, purple/violet transition band
//   - Rectangular brick border ring around full screen edge
//   - vs.1.1 vertex shader: sinusoidal Z-displacement (glass breathes)
//   - Fixed-function DOT3 bump: large-facet cathedral rolled-glass normal map
//   - Per-panel additive glow pass (center-bright fan, edge transparent)
//   - Light shaft sweeping L->R additively
//   - Logo: dark outline pass (scale up, black) + bright fill pass on top
//   - Backlight envelope: 0-3s ramp, 3-22s peak+pulse, 22-28s dim
//
// Assets: D:\tex\tr.dds (DXT1), D:\tex\glass_n.dds (generated A8R8G8B8)
// No heap alloc. All geometry in static VBs. RXDK-safe ftoi inline asm.

#include "GlassScene.h"
#include <xtl.h>
#include <d3dx8.h>
#include <xgraphics.h>
#include <math.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

static const float SW = 640.0f;
static const float SH = 480.0f;
static const float CX = 320.0f;
static const float CY = 240.0f;
static const float PI2 = 6.28318530717959f;
static const float PI = 3.14159265358979f;
static const DWORD SCENE_MS = 28000;

// ------------------------------------------------------------
// RXDK-safe helpers
// ------------------------------------------------------------
static __forceinline int Ftoi(float f)
{
    int i;
    __asm { fld   f  }
    __asm { fistp i  }
    return i;
}
static __forceinline BYTE ClampB(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}
static __forceinline float Clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}
static __forceinline float Fabs(float f) { return f < 0.f ? -f : f; }
static __forceinline float Fmin(float a, float b) { return a < b ? a : b; }
static __forceinline float Fmax(float a, float b) { return a > b ? a : b; }

// ------------------------------------------------------------
// Trig LUT
// ------------------------------------------------------------
static const int LUT_N = 1024;
static float     s_sin[LUT_N];
static float     s_cos[LUT_N];
static bool      s_lutReady = false;

static void BuildLUT()
{
    if (s_lutReady) return;
    for (int i = 0; i < LUT_N; ++i)
    {
        float a = (float)i * PI2 / (float)LUT_N;
        s_sin[i] = sinf(a);
        s_cos[i] = cosf(a);
    }
    s_lutReady = true;
}
static __forceinline float LSin(float r)
{
    int idx = Ftoi(r * (float)LUT_N / PI2);
    return s_sin[((idx % LUT_N) + LUT_N) & (LUT_N - 1)];
}
static __forceinline float LCos(float r)
{
    int idx = Ftoi(r * (float)LUT_N / PI2);
    return s_cos[((idx % LUT_N) + LUT_N) & (LUT_N - 1)];
}

// ------------------------------------------------------------
// Simple deterministic pseudo-random (no stdlib rand)
// ------------------------------------------------------------
static unsigned int s_rng = 0x12345678u;
static __forceinline float Randf()
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return (float)(s_rng & 0x7FFFu) / (float)0x8000u;  // 0..1
}
static __forceinline float RandfSym() { return Randf() * 2.0f - 1.0f; } // -1..1

// ------------------------------------------------------------
// Scene state
// ------------------------------------------------------------
static bool  s_active = false;
static DWORD s_startTicks = 0;

// ------------------------------------------------------------
// Textures
// ------------------------------------------------------------
static LPDIRECT3DTEXTURE8 s_texLogo = NULL;
static LPDIRECT3DTEXTURE8 s_texNormal = NULL;
static int                s_loadStep = 0;

static void StepLoad()
{
    switch (s_loadStep)
    {
    case 0:
        if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\tr.dds", &s_texLogo)))
            D3DXCreateTextureFromFileA(g_pDevice, "tex\\tr.dds", &s_texLogo);
        s_loadStep = 1;
        break;
    case 1:
        if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\glass_n.dds", &s_texNormal)))
            if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "tex\\glass_n.dds", &s_texNormal)))
                if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\chrome.dds", &s_texNormal)))
                    D3DXCreateTextureFromFileA(g_pDevice, "tex\\chrome.dds", &s_texNormal);
        s_loadStep = 2;
        break;
    default:
        s_active = true;
        break;
    }
}
static void ReleaseTextures()
{
    if (s_texLogo) { s_texLogo->Release();   s_texLogo = NULL; }
    if (s_texNormal) { s_texNormal->Release(); s_texNormal = NULL; }
}

// ------------------------------------------------------------
// Vertex shader vs.1.1  — sinusoidal Z displacement
// c0-c3: WVP  c4: x=time*0.5 y=amplitude z=freq w=0
// c5:    x=0.5 y=2.0 z=0 w=1
// ------------------------------------------------------------
static DWORD s_vsHandle = 0;

static const DWORD s_vsDecl[] =
{
    D3DVSD_STREAM(0),
    D3DVSD_REG(D3DVSDE_POSITION,  D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_DIFFUSE,   D3DVSDT_D3DCOLOR),
    D3DVSD_REG(D3DVSDE_TEXCOORD0, D3DVSDT_FLOAT2),
    D3DVSD_END()
};

// vs.1.1 — Backlit stained glass
//
// c0-c3 : WVP matrix
// c4    : x=time*0.5  y=Z-amp  z=freq  w=0
// c5    : x=0.5  y=2.0  z=0  w=1
// c6    : x=lightX(screen px)  y=lightY(screen px)  z=1/radius  w=globalBright
// c7    : x=lightR  y=lightG  z=lightB  w=ambient (0..1)
// c8    : x=SW(640)  y=SH(480)  z=SW*0.5  w=SH*0.5
//
// Vertex positions ARE screen pixels (the ortho matrix converts them).
// We compute distance in screen pixel space directly from v0.xy.
static const char s_vsSource[] =
"vs.1.1\n"

// ---- Z displacement (breathing) ----
"dp3 r0.x, v0, c4.zzzw\n"
"add r0.x, r0.x, c4.x\n"
"frc r0.x, r0.x\n"
"mad r0.x, r0.x, c5.y, -c5.x\n"
"mul r1.x, r0.x, r0.x\n"
"mad r0.x, -r1.x, r0.x, r0.x\n"
"mul r0.x, r0.x, c4.y\n"
"mov r1, v0\n"
"add r1.z, v0.z, r0.x\n"

// ---- Transform to clip space ----
"dp4 oPos.x, r1, c0\n"
"dp4 oPos.y, r1, c1\n"
"dp4 oPos.z, r1, c2\n"
"dp4 oPos.w, r1, c3\n"

// ---- Backlight: distance in screen pixel space (v0.xy = screen px) ----
// r2.xy = vertex - lightPos
"sub r2.x, v0.x, c6.x\n"
"sub r2.y, v0.y, c6.y\n"
// r3.x = (dx * invR)^2 + (dy * invR)^2  = (dist/radius)^2
"mul r2.x, r2.x, c6.z\n"       // dx / radius
"mul r2.y, r2.y, c6.z\n"       // dy / radius
"mul r3.x, r2.x, r2.x\n"
"mul r3.y, r2.y, r2.y\n"
"add r3.x, r3.x, r3.y\n"       // (dist/radius)^2
// falloff = saturate(1 - dist^2/r^2)
"sub r3.x, c5.w, r3.x\n"       // 1 - norm
"max r3.x, r3.x, c5.z\n"       // clamp >= 0
"min r3.x, r3.x, c5.w\n"       // clamp <= 1
// square it for tighter hotspot
"mul r3.x, r3.x, r3.x\n"

// ---- lit = ambient + falloff * lightColor ----
// r4.xyz = lightColor * falloff
"mul r4.xyz, c7.xyzz, r3.xxxx\n"
// r4.xyz += ambient
"add r4.xyz, r4.xyz, c7.wwww\n"
"min r4.xyz, r4.xyz, c5.wwww\n" // clamp to 1
"mov r4.w,   c5.w\n"            // alpha = 1

// ---- Modulate panel base color by lighting ----
"mul oD0, v2, r4\n"

// ---- Texcoords ----
"mov oT0, v3\n"
"mov oT1, v3\n";

static void CreateVS()
{
    LPXGBUFFER pCode = NULL, pErr = NULL;
    if (SUCCEEDED(XGAssembleShader("GlassVS", s_vsSource,
        (UINT)strlen(s_vsSource), 0, NULL, &pCode, &pErr, NULL, NULL, NULL, NULL)))
    {
        g_pDevice->CreateVertexShader(s_vsDecl,
            (const DWORD*)pCode->GetBufferPointer(), &s_vsHandle, 0);
        pCode->Release();
    }
    if (pErr) pErr->Release();
}
static void DeleteVS()
{
    if (s_vsHandle) { g_pDevice->DeleteVertexShader(s_vsHandle); s_vsHandle = 0; }
}
// Light animation — slow sweep + gentle orbital drift
// Returns screen-space XY of the virtual backlight source
static void GetLightPos(float tSec, float* lx, float* ly)
{
    // Primary: slow lemniscate (figure-8) path — feels like sun tracking across sky
    // Full cycle every 14 seconds
    float t = tSec * (PI2 / 14.0f);
    float s = LSin(t);
    float c = LCos(t);
    float d = 1.0f + s * s;          // lemniscate denominator
    float ox = CX + (c / d) * 260.f;  // x swings wide
    float oy = CY + (s * c / d) * 120.f; // y traces the figure-8 crossover

    // Secondary: very slow drift layered on top — different period so it never repeats
    float driftX = LCos(tSec * 0.11f) * 55.f;
    float driftY = LSin(tSec * 0.07f) * 35.f;

    *lx = ox + driftX;
    *ly = oy + driftY;
}

// Returns a 0..1 intensity modifier — slow cloud-like brightness variation
static float GetLightIntensity(float tSec)
{
    // Three overlapping sine waves at different periods — produces organic variation
    float a = 0.70f + 0.15f * LSin(tSec * 0.23f);
    float b = 1.00f + 0.12f * LSin(tSec * 0.41f + 1.3f);
    float c = 1.00f + 0.08f * LCos(tSec * 0.67f + 0.7f);
    return Clamp01(a * b * c);
}

static void SetVSConstants(float tSec, float backlight)
{
    D3DXMATRIX world, view, proj, wvp;
    g_pDevice->GetTransform(D3DTS_WORLD, &world);
    g_pDevice->GetTransform(D3DTS_VIEW, &view);
    g_pDevice->GetTransform(D3DTS_PROJECTION, &proj);
    wvp = world * view * proj;
    g_pDevice->SetVertexShaderConstant(0, &wvp, 4);

    // c4: Z displacement params
    float c4[4] = { tSec * 0.5f, 0.014f * backlight, 2.8f, 0.0f };
    float c5[4] = { 0.5f, 2.0f, 0.0f, 1.0f };
    g_pDevice->SetVertexShaderConstant(4, c4, 1);
    g_pDevice->SetVertexShaderConstant(5, c5, 1);

    // c6: light position + 1/radius
    // Tight radius = obvious hotspot that panels clearly enter/exit
    float lx, ly;
    GetLightPos(tSec, &lx, &ly);
    float radius = 180.f;            // tight — clear bright circle
    float invR = 1.0f / radius;
    float c6[4] = { lx, ly, invR, backlight };
    g_pDevice->SetVertexShaderConstant(6, c6, 1);

    // c7: very warm intense light + low ambient for high contrast
    // Lit panels: full warm white (strong red/green shift, blue cut)
    // Unlit panels: 15% ambient — noticeably darker, clearly different
    float amb = 0.15f * backlight;  // dark outside the hotspot
    float lInt = 1.2f * backlight;  // bright inside — can exceed 1, clamped in VS
    float c7[4] = { lInt * 1.0f, lInt * 0.75f, lInt * 0.35f, amb };
    g_pDevice->SetVertexShaderConstant(7, c7, 1);

    float c8[4] = { SW, SH, CX, CY };
    g_pDevice->SetVertexShaderConstant(8, c8, 1);
}

// ------------------------------------------------------------
// Palette — zone-based like the reference image
//   Zone 0 (outer border bricks): warm — amber, orange, red
//   Zone 1 (outer mosaic):        warm/mixed — orange, amber, rose
//   Zone 2 (transition band):     purple, violet, magenta
//   Zone 3 (inner field):         cool — cobalt, teal, cyan, blue
//   Zone 4 (center):              cool bright — cyan, sky, blue-white
// ------------------------------------------------------------
static const DWORD PAL_WARM[] =
{
    D3DCOLOR_ARGB(255, 255, 130,   0),  // amber
    D3DCOLOR_ARGB(255, 255,  80,   0),  // deep orange
    D3DCOLOR_ARGB(255, 220,  30,  20),  // red-orange
    D3DCOLOR_ARGB(255, 255, 170,  20),  // golden amber
    D3DCOLOR_ARGB(255, 200,  50,  10),  // brick red
    D3DCOLOR_ARGB(255, 255, 100,  40),  // coral
};
static const int PAL_WARM_N = 6;

static const DWORD PAL_TRANS[] =
{
    D3DCOLOR_ARGB(255, 160,  20, 220),  // violet
    D3DCOLOR_ARGB(255, 200,  40, 200),  // magenta
    D3DCOLOR_ARGB(255, 120,  10, 200),  // deep violet
    D3DCOLOR_ARGB(255, 180,  60, 255),  // purple-pink
    D3DCOLOR_ARGB(255, 140,   0, 180),  // plum
};
static const int PAL_TRANS_N = 5;

static const DWORD PAL_COOL[] =
{
    D3DCOLOR_ARGB(255,  20,  80, 220),  // cobalt
    D3DCOLOR_ARGB(255,   0, 160, 210),  // teal
    D3DCOLOR_ARGB(255,   0, 200, 230),  // cyan
    D3DCOLOR_ARGB(255,  40, 100, 255),  // royal blue
    D3DCOLOR_ARGB(255,   0, 130, 180),  // steel teal
    D3DCOLOR_ARGB(255,  80, 180, 255),  // sky blue
    D3DCOLOR_ARGB(255,  60,  60, 200),  // indigo
};
static const int PAL_COOL_N = 7;

// Pick color based on distance from center (normalized 0=center 1=edge)
static DWORD ZoneColor(float normDist, int seed)
{
    // inner 35%: cool
    if (normDist < 0.35f)
        return PAL_COOL[((unsigned)seed * 7 + 3) % PAL_COOL_N];
    // 35-55%: cool with some transition
    if (normDist < 0.55f)
    {
        int s2 = (seed * 13 + 7) % (PAL_COOL_N + PAL_TRANS_N);
        if (s2 < PAL_COOL_N) return PAL_COOL[s2];
        return PAL_TRANS[s2 - PAL_COOL_N];
    }
    // 55-70%: transition band
    if (normDist < 0.70f)
        return PAL_TRANS[((unsigned)seed * 11 + 5) % PAL_TRANS_N];
    // outer 30%: warm
    return PAL_WARM[((unsigned)seed * 17 + 2) % PAL_WARM_N];
}

// ------------------------------------------------------------
// Geometry
// ------------------------------------------------------------
struct GlassVtx { float x, y, z; DWORD color; float u, v; };
struct LeadVtx { float x, y, z, rhw; DWORD color; };

static const int MAX_PANEL_VERTS = 12000;
static const int MAX_GLOW_VERTS = 8000;
static const int MAX_LEAD_VERTS = 8000;

static LPDIRECT3DVERTEXBUFFER8 s_panelVB = NULL;
static int                     s_panelVerts = 0;
static LPDIRECT3DVERTEXBUFFER8 s_glowVB = NULL;
static int                     s_glowVerts = 0;
static LPDIRECT3DVERTEXBUFFER8 s_leadVB = NULL;
static int                     s_leadVerts = 0;

// Shadow copy in system RAM — base colors + positions for CPU lighting
// We rewrite s_panelVB every frame with lit colors derived from this.
static GlassVtx s_panelBase[MAX_PANEL_VERTS];  // base (unlit) panel geometry

// Lead came emitter state
static LeadVtx* s_lv = NULL;
static int      s_lvi = 0;
static int      s_leadCap = 0;

static void EmitLine(float x0, float y0, float x1, float y1, float w, DWORD col)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.5f) return;
    float nx = -dy / len * w, ny = dx / len * w;
    if (s_lvi + 6 > s_leadCap) return;
    s_lv[s_lvi++] = { x0 - nx,y0 - ny,0.f,1.f,col };
    s_lv[s_lvi++] = { x0 + nx,y0 + ny,0.f,1.f,col };
    s_lv[s_lvi++] = { x1 - nx,y1 - ny,0.f,1.f,col };
    s_lv[s_lvi++] = { x1 - nx,y1 - ny,0.f,1.f,col };
    s_lv[s_lvi++] = { x0 + nx,y0 + ny,0.f,1.f,col };
    s_lv[s_lvi++] = { x1 + nx,y1 + ny,0.f,1.f,col };
}

// Emit a convex polygon as triangle fan into panel VB.
// pts[] = {x0,y0, x1,y1, ...} npts = point count
// Also emits center-bright glow fan.
static GlassVtx* s_pvb = NULL;
static int       s_pvi = 0;
static GlassVtx* s_gvb = NULL;
static int       s_gvi = 0;

static void EmitPolygon(float* pts, int npts, DWORD color,
    float cx, float cy)  // cx,cy = centroid
{
    if (npts < 3) return;
    const float UV_SCALE = 5.0f / SW;

    // Panel pass: triangle fan from centroid
    if (s_pvi + (npts - 1) * 3 + 3 > MAX_PANEL_VERTS) return;

    DWORD c = D3DCOLOR_ARGB(255,
        (color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);

    for (int i = 0; i < npts; ++i)
    {
        int j = (i + 1) % npts;
        float ax = pts[i * 2], ay = pts[i * 2 + 1];
        float bx = pts[j * 2], by = pts[j * 2 + 1];
        s_pvb[s_pvi++] = { cx, cy, 0.f, c, cx * UV_SCALE, cy * UV_SCALE };
        s_pvb[s_pvi++] = { ax, ay, 0.f, c, ax * UV_SCALE, ay * UV_SCALE };
        s_pvb[s_pvi++] = { bx, by, 0.f, c, bx * UV_SCALE, by * UV_SCALE };
    }

    // Glow pass: center bright, edges black/transparent
    if (s_gvi + npts * 3 + 3 > MAX_GLOW_VERTS) return;
    BYTE gr = ClampB(Ftoi(((color >> 16) & 0xFF) * 1.5f));
    BYTE gg = ClampB(Ftoi(((color >> 8) & 0xFF) * 1.5f));
    BYTE gb = ClampB(Ftoi(((color) & 0xFF) * 1.5f));
    DWORD cC = D3DCOLOR_ARGB(200, gr, gg, gb);
    DWORD cE = D3DCOLOR_ARGB(0, 0, 0, 0);

    for (int i = 0; i < npts; ++i)
    {
        int j = (i + 1) % npts;
        float ax = pts[i * 2], ay = pts[i * 2 + 1];
        float bx = pts[j * 2], by = pts[j * 2 + 1];
        s_gvb[s_gvi++] = { cx, cy, 0.f, cC, 0,0 };
        s_gvb[s_gvi++] = { ax, ay, 0.f, cE, 0,0 };
        s_gvb[s_gvi++] = { bx, by, 0.f, cE, 0,0 };
    }

    // Lead came along each edge
    const float CAME_W = 4.5f;
    const DWORD CAME_C = D3DCOLOR_ARGB(255, 0, 0, 0);
    for (int i = 0; i < npts; ++i)
    {
        int j = (i + 1) % npts;
        EmitLine(pts[i * 2], pts[i * 2 + 1], pts[j * 2], pts[j * 2 + 1], CAME_W, CAME_C);
    }
}

// Clip a convex polygon against a half-plane ax+by <= c
// Returns new point count (in-place)
static int ClipPoly(float* pts, int n, float a, float b, float c)
{
    static float tmp[64];
    int tn = 0;
    for (int i = 0; i < n; ++i)
    {
        int j = (i + 1) % n;
        float di = a * pts[i * 2] + b * pts[i * 2 + 1] - c;
        float dj = a * pts[j * 2] + b * pts[j * 2 + 1] - c;
        if (di <= 0.f) { tmp[tn * 2] = pts[i * 2]; tmp[tn * 2 + 1] = pts[i * 2 + 1]; ++tn; }
        if ((di < 0.f) != (dj < 0.f))
        {
            float t = di / (di - dj);
            tmp[tn * 2] = pts[i * 2] + t * (pts[j * 2] - pts[i * 2]);
            tmp[tn * 2 + 1] = pts[i * 2 + 1] + t * (pts[j * 2 + 1] - pts[i * 2 + 1]);
            ++tn;
        }
    }
    if (tn > 0) memcpy(pts, tmp, tn * 2 * sizeof(float));
    return tn;
}

// Generate the full Voronoi mosaic using Fortune-style relaxation
// (Lloyd iteration approximation — cheap, no heap)
static void BuildMosaic()
{
    // Seed points — jittered grid, two density zones
    // Inner circle (~r<200): ~60 seeds for larger pieces
    // Outer ring (r>200):    ~80 seeds for smaller border pieces
    static const int MAX_SEEDS = 160;
    static float sx[MAX_SEEDS], sy[MAX_SEEDS];
    int nseed = 0;

    s_rng = 0xDEADBEEFu;  // deterministic

    // Inner field: 7x5 jittered grid clipped to ellipse r<205
    for (int gy = 0; gy < 7 && nseed < MAX_SEEDS; ++gy)
        for (int gx = 0; gx < 9 && nseed < MAX_SEEDS; ++gx)
        {
            float bx = 80.f + gx * (480.f / 8.f);
            float by = 60.f + gy * (360.f / 6.f);
            float jx = RandfSym() * 28.f;
            float jy = RandfSym() * 22.f;
            float px = bx + jx, py = by + jy;
            float dx = (px - CX) / 210.f, dy = (py - CY) / 175.f;
            if (dx * dx + dy * dy < 1.0f)
            {
                sx[nseed] = px; sy[nseed] = py; ++nseed;
            }
        }
    int nInner = nseed;

    // Outer ring: denser angular grid between r=190 and screen edge
    int outerSegs = 56;
    for (int i = 0; i < outerSegs && nseed < MAX_SEEDS; ++i)
    {
        float a = PI2 * (float)i / (float)outerSegs + RandfSym() * 0.08f;
        float r = 205.f + Randf() * 130.f;
        float px = CX + LCos(a) * r + RandfSym() * 18.f;
        float py = CY + LSin(a) * r + RandfSym() * 14.f;
        px = Fmax(8.f, Fmin(SW - 8.f, px));
        py = Fmax(8.f, Fmin(SH - 8.f, py));
        sx[nseed] = px; sy[nseed] = py; ++nseed;
    }

    // One Lloyd relaxation pass (centroid of approximate Voronoi cell)
    // Approximate: for each seed, shift toward weighted average of neighbors
    for (int iter = 0; iter < 2; ++iter)
    {
        for (int i = 0; i < nseed; ++i)
        {
            float ax = 0, ay = 0; int cnt = 0;
            for (int j = 0; j < nseed; ++j)
            {
                if (i == j) continue;
                float dx = sx[j] - sx[i], dy = sy[j] - sy[i];
                float d2 = dx * dx + dy * dy;
                if (d2 < 90.f * 90.f) { ax += sx[j]; ay += sy[j]; ++cnt; }
            }
            if (cnt > 0) {
                sx[i] = sx[i] * 0.7f + (ax / (float)cnt) * 0.3f;
                sy[i] = sy[i] * 0.7f + (ay / (float)cnt) * 0.3f;
            }
        }
    }

    // For each seed, construct its Voronoi cell by clipping a large quad
    // against the perpendicular bisectors of all nearby neighbors.
    static float poly[32];  // max 16 vertices * 2 floats
    const float CAME_W = 4.5f;
    const DWORD CAME_C = D3DCOLOR_ARGB(255, 0, 0, 0);

    for (int i = 0; i < nseed; ++i)
    {
        // Start with screen-sized quad
        poly[0] = 0;   poly[1] = 0;
        poly[2] = SW;  poly[3] = 0;
        poly[4] = SW;  poly[5] = SH;
        poly[6] = 0;   poly[7] = SH;
        int np = 4;

        // Clip against screen border with inset
        np = ClipPoly(poly, np, 1, 0, SW - 4.f);
        np = ClipPoly(poly, np, -1, 0, -4.f);
        np = ClipPoly(poly, np, 0, 1, SH - 4.f);
        np = ClipPoly(poly, np, 0, -1, -4.f);

        // Clip against perpendicular bisectors of nearby seeds
        for (int j = 0; j < nseed && np >= 3; ++j)
        {
            if (i == j) continue;
            float dx = sx[j] - sx[i], dy = sy[j] - sy[i];
            float d2 = dx * dx + dy * dy;
            if (d2 > 160.f * 160.f) continue;  // skip far seeds
            // Bisector: dx*(x-mx)+dy*(y-my)<=0  where mx,my = midpoint
            float mx = (sx[i] + sx[j]) * 0.5f, my = (sy[i] + sy[j]) * 0.5f;
            float c_ = dx * mx + dy * my;
            np = ClipPoly(poly, np, dx, dy, c_);
        }

        if (np < 3) continue;

        // Compute centroid
        float pcx = 0, pcy = 0;
        for (int k = 0; k < np; ++k) { pcx += poly[k * 2]; pcy += poly[k * 2 + 1]; }
        pcx /= (float)np; pcy /= (float)np;

        // Zone color based on distance from center
        float ddx = (pcx - CX) / SW, ddy = (pcy - CY) / SH;
        float normD = Clamp01(sqrtf(ddx * ddx * 4.f + ddy * ddy * 4.f));
        DWORD col = ZoneColor(normD, i);

        EmitPolygon(poly, np, col, pcx, pcy);
    }
}

// Rectangular brick border around the screen perimeter
static void BuildBrickBorder()
{
    const float MARGIN = 28.f;   // how far inset the bricks go
    const float CAME_W = 4.5f;
    const DWORD CAME_C = D3DCOLOR_ARGB(255, 0, 0, 0);
    const float BRICKH = 32.f;
    const float BRICKW = 44.f;   // average width
    int seed = 200;

    // Top strip
    {
        float y0 = 4.f, y1 = MARGIN + 4.f;
        float x = 4.f;
        while (x < SW - 4.f)
        {
            float w = BRICKW + RandfSym() * 12.f;
            if (x + w > SW - 4.f) w = SW - 4.f - x;
            float pts[8] = { x,y0, x + w,y0, x + w,y1, x,y1 };
            float pcx = x + w * 0.5f, pcy = (y0 + y1) * 0.5f;
            float dx = (pcx - CX) / SW, dy = (pcy - CY) / SH;
            DWORD col = PAL_WARM[(seed * 17 + 3) % PAL_WARM_N]; ++seed;
            EmitPolygon(pts, 4, col, pcx, pcy);
            x += w;
        }
    }
    // Bottom strip
    {
        float y0 = SH - MARGIN - 4.f, y1 = SH - 4.f;
        float x = 4.f;
        while (x < SW - 4.f)
        {
            float w = BRICKW + RandfSym() * 12.f;
            if (x + w > SW - 4.f) w = SW - 4.f - x;
            float pts[8] = { x,y0, x + w,y0, x + w,y1, x,y1 };
            float pcx = x + w * 0.5f, pcy = (y0 + y1) * 0.5f;
            DWORD col = PAL_WARM[(seed * 13 + 7) % PAL_WARM_N]; ++seed;
            EmitPolygon(pts, 4, col, pcx, pcy);
            x += w;
        }
    }
    // Left strip
    {
        float x0 = 4.f, x1 = MARGIN + 4.f;
        float y = MARGIN + 4.f;
        while (y < SH - MARGIN - 4.f)
        {
            float h = BRICKH + RandfSym() * 10.f;
            if (y + h > SH - MARGIN - 4.f) h = SH - MARGIN - 4.f - y;
            float pts[8] = { x0,y, x1,y, x1,y + h, x0,y + h };
            float pcx = (x0 + x1) * 0.5f, pcy = y + h * 0.5f;
            DWORD col = PAL_WARM[(seed * 11 + 2) % PAL_WARM_N]; ++seed;
            EmitPolygon(pts, 4, col, pcx, pcy);
            y += h;
        }
    }
    // Right strip
    {
        float x0 = SW - MARGIN - 4.f, x1 = SW - 4.f;
        float y = MARGIN + 4.f;
        while (y < SH - MARGIN - 4.f)
        {
            float h = BRICKH + RandfSym() * 10.f;
            if (y + h > SH - MARGIN - 4.f) h = SH - MARGIN - 4.f - y;
            float pts[8] = { x0,y, x1,y, x1,y + h, x0,y + h };
            float pcx = (x0 + x1) * 0.5f, pcy = y + h * 0.5f;
            DWORD col = PAL_WARM[(seed * 7 + 5) % PAL_WARM_N]; ++seed;
            EmitPolygon(pts, 4, col, pcx, pcy);
            y += h;
        }
    }
}

static void BuildWindowGeometry()
{
    // Panel VB: DYNAMIC so we can lock/update every frame for CPU lighting
    g_pDevice->CreateVertexBuffer(MAX_PANEL_VERTS * sizeof(GlassVtx),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s_panelVB);
    g_pDevice->CreateVertexBuffer(MAX_GLOW_VERTS * sizeof(GlassVtx),
        D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &s_glowVB);
    g_pDevice->CreateVertexBuffer(MAX_LEAD_VERTS * sizeof(LeadVtx),
        D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &s_leadVB);

    // Build into shadow copy first
    s_pvb = s_panelBase;
    s_glowVB->Lock(0, 0, (BYTE**)&s_gvb, 0);
    s_leadVB->Lock(0, 0, (BYTE**)&s_lv, 0);
    s_pvi = 0; s_gvi = 0; s_lvi = 0;
    s_leadCap = MAX_LEAD_VERTS;

    BuildBrickBorder();
    BuildMosaic();

    // Upload initial panel data to GPU VB
    GlassVtx* gpuVerts = NULL;
    s_panelVB->Lock(0, s_pvi * sizeof(GlassVtx), (BYTE**)&gpuVerts, 0);
    if (gpuVerts) memcpy(gpuVerts, s_panelBase, s_pvi * sizeof(GlassVtx));
    s_panelVB->Unlock();

    s_glowVB->Unlock(); s_leadVB->Unlock();
    s_panelVerts = s_pvi; s_glowVerts = s_gvi; s_leadVerts = s_lvi;
    s_pvb = NULL; s_gvb = NULL; s_lv = NULL;
}

static void ReleaseGeometry()
{
    if (s_panelVB) { s_panelVB->Release(); s_panelVB = NULL; }
    if (s_glowVB) { s_glowVB->Release(); s_glowVB = NULL; }
    if (s_leadVB) { s_leadVB->Release(); s_leadVB = NULL; }
    s_panelVerts = s_glowVerts = s_leadVerts = 0;
}

// ------------------------------------------------------------
// Animation helpers
// ------------------------------------------------------------
static float Backlight(float tSec)
{
    float base;
    if (tSec < 3.f)  base = tSec / 3.f;
    else if (tSec < 22.f) base = 1.f;
    else if (tSec < 28.f) base = 1.f - (tSec - 22.f) / 6.f;
    else                  base = 0.f;
    return Clamp01(base + 0.05f * LSin(tSec * 0.9f));
}
// (Light is now computed per-vertex in the VS — no 2D shaft overlay)

// ------------------------------------------------------------
// Draw helpers
// ------------------------------------------------------------
static void SetBlend2D()
{
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
}

// CPU lighting — runs every frame, rewrites panel VB with lit colors.
// This is guaranteed to work regardless of VS state.
// Light pos in screen pixels, radius in pixels.
static void UpdatePanelLighting(float tSec, float backlight)
{
    if (!s_panelVB || s_panelVerts == 0) return;

    float lx, ly;
    GetLightPos(tSec, &lx, &ly);
    float intensity = GetLightIntensity(tSec);

    const float RADIUS = 175.f;
    const float INV_R2 = 1.0f / (RADIUS * RADIUS);
    const float AMBIENT = 0.18f * backlight;
    const float LIGHT_R = 1.10f * backlight * intensity;
    const float LIGHT_G = 0.75f * backlight * intensity;
    const float LIGHT_B = 0.32f * backlight * intensity;

    GlassVtx* dst = NULL;
    if (FAILED(s_panelVB->Lock(0, s_panelVerts * sizeof(GlassVtx),
        (BYTE**)&dst, 0))) return;

    for (int i = 0; i < s_panelVerts; ++i)
    {
        const GlassVtx& src = s_panelBase[i];

        // Distance falloff in screen pixel space — guaranteed correct
        float dx = src.x - lx;
        float dy = src.y - ly;
        float norm = (dx * dx + dy * dy) * INV_R2;  // 0 at light, 1 at radius edge
        float fall = Clamp01(1.0f - norm);
        fall = fall * fall;  // quadratic — tighter hotspot

        // Base panel color
        float br = (float)((src.color >> 16) & 0xFF) * (1.f / 255.f);
        float bg = (float)((src.color >> 8) & 0xFF) * (1.f / 255.f);
        float bb = (float)((src.color) & 0xFF) * (1.f / 255.f);

        // Lit = base * (ambient + falloff * lightColor)
        float lr = br * (AMBIENT + fall * LIGHT_R);
        float lg = bg * (AMBIENT + fall * LIGHT_G);
        float lb = bb * (AMBIENT + fall * LIGHT_B);

        // Clamp and pack
        BYTE cr = ClampB(Ftoi(lr * 255.f));
        BYTE cg = ClampB(Ftoi(lg * 255.f));
        BYTE cb = ClampB(Ftoi(lb * 255.f));

        dst[i] = src;
        dst[i].color = D3DCOLOR_ARGB(255, cr, cg, cb);
    }

    s_panelVB->Unlock();
}

static void DrawBackground()
{
    struct V2D { float x, y, z, rhw; DWORD c; };
    DWORD blk = D3DCOLOR_ARGB(255, 0, 0, 0);
    V2D q[4] = { {0,0,0,1,blk},{SW,0,0,1,blk},{0,SH,0,1,blk},{SW,SH,0,1,blk} };
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    SetBlend2D();
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
}

static void DrawGlass(float tSec, float backlight)
{
    if (!s_panelVB || s_panelVerts == 0) return;

    // CPU lighting: lock dynamic VB, rewrite vertex colors every frame
    UpdatePanelLighting(tSec, backlight);

    D3DXMATRIX id, proj;
    D3DXMatrixIdentity(&id);
    D3DXMatrixIdentity(&proj);
    proj._11 = 2.f / SW;  proj._22 = -2.f / SH;
    proj._41 = -1.f;     proj._42 = 1.f;
    proj._33 = 0.5f;    proj._43 = 0.5f;
    g_pDevice->SetTransform(D3DTS_WORLD, &id);
    g_pDevice->SetTransform(D3DTS_VIEW, &id);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);
    g_pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    SetBlend2D();

    if (s_texNormal)
    {
        // DOT3 bump mapping — NV2A native, no pixel shader needed.
        //
        // Stage 0: DOT3(normalMap, lightDir) → greyscale surface variation
        //   lightDir encoded in TEXTUREFACTOR as signed vector (127=0, 255=+1, 0=-1)
        //   The direction tracks the animated light position so surface facets
        //   catch and release highlights as the light sweeps across.
        //
        // Stage 1: MODULATE(dot3Result, diffuse) → colored backlit glass
        //   diffuse already has the per-vertex CPU lighting baked in (warm falloff).
        //   Multiplying by DOT3 greyscale adds surface micro-detail on top —
        //   bubbles, streaks, and facets in the glass catch the light independently.

        // Compute light direction from current light position to screen center
        // (light is behind the glass, shining toward viewer)
        float lx, ly;
        GetLightPos(tSec, &lx, &ly);
        float dlx = lx - CX;
        float dly = ly - CY;
        float len = sqrtf(dlx * dlx + dly * dly + 200.f * 200.f); // z=200 keeps it from going fully sideways
        float nx = dlx / len;
        float ny = dly / len;
        float nz = 200.f / len;

        // Pack into 0-255: 127 = 0.0, 255 = +1.0, 0 = -1.0
        BYTE ldr = ClampB(Ftoi((nx + 1.0f) * 127.5f));
        BYTE ldg = ClampB(Ftoi((-ny + 1.0f) * 127.5f)); // Y flipped (screen vs texture space)
        BYTE ldb = ClampB(Ftoi((nz + 1.0f) * 127.5f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR,
            D3DCOLOR_ARGB(255, ldr, ldg, ldb));

        // Stage 0: DOT3 — normal map surface vs animated light direction
        g_pDevice->SetTexture(0, s_texNormal);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);  // light dir, NOT diffuse
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
        g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);

        // Stage 1: modulate DOT3 greyscale by CPU-lit panel color
        g_pDevice->SetTexture(1, NULL);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);   // dot3 result
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_DIFFUSE);   // lit panel color
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }
    else
    {
        g_pDevice->SetTexture(0, NULL);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    }

    g_pDevice->SetStreamSource(0, s_panelVB, sizeof(GlassVtx));
    g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_panelVerts / 3);

    if (s_glowVB && s_glowVerts > 0)
    {
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        g_pDevice->SetTexture(0, NULL);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetStreamSource(0, s_glowVB, sizeof(GlassVtx));
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_glowVerts / 3);
    }

    g_pDevice->SetTransform(D3DTS_PROJECTION, &id);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
}

static void DrawLeadCame()
{
    if (!s_leadVB || s_leadVerts == 0) return;
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    SetBlend2D();
    g_pDevice->SetStreamSource(0, s_leadVB, sizeof(LeadVtx));
    g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_leadVerts / 3);
}

// Light shaft removed — lighting is per-vertex in VS via c6/c7

static void DrawLogo(float tSec, float backlight)
{
    if (!s_texLogo) return;

    int phL = Ftoi(tSec * 1000.f / 11.f) & (LUT_N - 1);
    float br = 0.97f + 0.03f * s_sin[phL];

    float lx, ly;
    GetLightPos(tSec, &lx, &ly);
    float lightDist = Fabs(lx - CX) / (SW * 0.5f);
    float boost = 1.f + (1.f - Clamp01(lightDist)) * 0.4f;
    float alpha = Clamp01(backlight * boost);

    // Logo size — large enough to read clearly like the reference
    float lW = 420.f * br, lH = 420.f * br;
    float lX = (SW - lW) * 0.5f, lY = (SH - lH) * 0.5f;

    SetBlend2D();

    struct TV { float x, y, z, rhw; DWORD c; float u, v; };

    // Pass 1: Dark outline — draw slightly larger in near-black
    // Gives the leaded-glass-letter illusion from the reference
    {
        float oW = lW + 14.f, oH = lH + 14.f;
        float oX = (SW - oW) * 0.5f, oY = (SH - oH) * 0.5f;
        BYTE  oa = ClampB(Ftoi(alpha * 255.f));
        DWORD oc = D3DCOLOR_ARGB(oa, 8, 0, 12);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHAFUNC, D3DCMP_GREATER);
        g_pDevice->SetRenderState(D3DRS_ALPHAREF, 20);
        g_pDevice->SetTexture(0, s_texLogo);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
        g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
        TV oq[4] = { {oX,oY,0,1,oc,0,0},{oX + oW,oY,0,1,oc,1,0},
                  {oX,oY + oH,0,1,oc,0,1},{oX + oW,oY + oH,0,1,oc,1,1} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, oq, sizeof(TV));
    }

    // Pass 2: Hot-pink fill — the logo color itself
    {
        BYTE  la = ClampB(Ftoi(alpha * 255.f));
        DWORD lc = D3DCOLOR_ARGB(la, 255, 60, 200);  // hot magenta-pink
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        TV q[4] = { {lX,lY,0,1,lc,0,0},{lX + lW,lY,0,1,lc,1,0},
                 {lX,lY + lH,0,1,lc,0,1},{lX + lW,lY + lH,0,1,lc,1,1} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV));
    }

    // Pass 3: Additive inner glow — makes letters luminous like backlit glass
    {
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        BYTE  ga = ClampB(Ftoi(alpha * 140.f));
        DWORD gc = D3DCOLOR_ARGB(ga, 255, 120, 255);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
        TV q[4] = { {lX,lY,0,1,gc,0,0},{lX + lW,lY,0,1,gc,1,0},
                 {lX,lY + lH,0,1,gc,0,1},{lX + lW,lY + lH,0,1,gc,1,1} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(TV));
    }

    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
}

static void UndoState()
{
    g_pDevice->SetPixelShader(0);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    for (int i = 0; i < 4; ++i)
    {
        g_pDevice->SetTexture(i, NULL);
        g_pDevice->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    D3DXMATRIX id; D3DXMatrixIdentity(&id);
    g_pDevice->SetTransform(D3DTS_WORLD, &id);
    g_pDevice->SetTransform(D3DTS_VIEW, &id);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &id);
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
void GlassScene_Init()
{
    s_active = false;
    s_startTicks = GetTickCount();
    s_loadStep = 0;
    BuildLUT();
    BuildWindowGeometry();
    CreateVS();
}

void GlassScene_Shutdown()
{
    s_active = false;
    UndoState();
    ReleaseGeometry();
    DeleteVS();
    ReleaseTextures();
}

bool GlassScene_IsFinished()
{
    if (!s_startTicks) return false;
    return (GetTickCount() - s_startTicks) >= SCENE_MS;
}

void GlassScene_Render(float /*demoTime*/)
{
    if (!g_pDevice) return;
    if (s_loadStep <= 1) StepLoad();

    DWORD tMs = GetTickCount() - s_startTicks;
    float tSec = (float)tMs * 0.001f;
    float bl = Backlight(tSec);

    DrawBackground();
    DrawGlass(tSec, bl);
    DrawLeadCame();
    DrawLogo(tSec, bl);
    UndoState();
}