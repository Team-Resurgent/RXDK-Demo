// =============================================================================
// CrystalScene.cpp
// Crystalline Grotto -- GPU Benchmark Scene | RXDK / NV2A Fixed-Function
// =============================================================================
//
// OVERVIEW
//   A demoscene-style benchmark scene for original Xbox hardware. Renders a
//   cluster of glowing crystalline prisms inside a dark procedural cave
//   environment. Designed to push NV2A fill-rate and texture bandwidth while
//   remaining stable on real hardware.
//
// GEOMETRY
//   - 14 hexagonal prisms, each subdivided 3x = 21,504 triangles total
//   - Geometry stored in a single static vertex buffer (~1.97 MB)
//   - Cave background: indexed cylinder mesh, 192x96 quads = 36,864 triangles (~801KB)
//   - Fog puffs: 120 billboarded quads, enlarged radius, CPU-updated every frame
//   - Stalactite/stalagmite formations: 32 + 24 = 56 hex prism mini crystals
//     rooted flush on cave wall, 12 tris each = 672 tris per cave draw (2,016 drawn 3x)
//   - Hardware ceiling: ND=14 stable -- ND=15/16+ causes NV2A VB alloc hard lock
//
// RENDER PASSES (crystal cluster)
//   Pass  1 (opaque)  : DOT3 bump map base -- primary lit colour via TFACTOR
//   Pass  2 (additive): DOT3 cyan + violet light directions averaged
//   Pass  3 (additive): Cubemap reflection -- three tints merged into one draw
//   Pass  4 (additive): Rim DOT3 -- two rim light directions merged
//   Pass  5 (additive): Violet specular DOT3 sweep
//   Pass  6 (additive): Cyan glint cubemap
//   Pass  7 (additive): Hot pink DOT3 sweep
//   Pass  8 (additive): Violet cubemap sweep
//   Pass  9 (additive): DOT3 warm pink sweep
//   Pass 10 (additive): Cubemap teal glint
//   Pass 11 (additive): DOT3 deep blue rim
//   Pass 12 (additive): Cubemap white shimmer
//   Pass 13 (additive): DOT3 green-blue sweep
//   Pass 14 (additive): Cubemap deep violet pulse
//   Pass 15 (additive): DOT3 orange-red accent
//   Pass 16 (additive): Cubemap gold sweep
//   Total: 16 crystal passes x 21,504 tris = ~344,064 tri submissions/frame
//   Note: fullscreen quad passes removed -- caused cave blowout due to additive
//         accumulation over cave mesh. Load maintained via 16 crystal passes.
//
// CAVE BACKGROUND
//   - Textured indexed cylinder, additive blended after crystal cluster
//   - Drawn 3x with small tMs phase offsets (0 / +40 / +80) for subtle glow
//   - High-res mesh: 192x96 quads, drawn 3x = ~110,592 cave tri submissions/frame
//   - rock.dds: 256x256 A8R8G8B8 procedural fractal noise, very dark (max ~14)
//   - TFACTOR modulates glow intensity with slow sine pulse
//
// TEXTURES
//   D:\tex\crystal_n.dds    -- Normal map for DOT3 bump lighting
//   D:\tex\crystal_cube.dds -- Cubemap for environment reflection
//   D:\tex\rock.dds         -- Cave wall diffuse texture (procedural, dark)
//
// PERFORMANCE OVERLAY
//   Frame time bar (50ms scale), FPS counter, draw call count, tri count
//   Status flags: VB / NM / CB / CVB / CIB / RK / INIT
//   Thresholds: SOLID>=45fps  OK>=35fps  LIGHT>=25fps  HEAVY<25fps
//
// PIPELINE CONSTRAINTS
//   Fixed-function FVF only -- no vertex or pixel shaders
//   No float-to-int casts (__ftol2_sse avoided via Ftoi() wrapper)
//   Static vertex buffers -- no per-frame CPU geometry updates
//   All animation via TFACTOR, texture matrix, and LUT sine table
//
#include "CrystalScene.h"
#include <xtl.h>
#include <xgraphics.h>
#include <d3dx8.h>
#include <math.h>
#include <string.h>
#include "font.h"

extern LPDIRECT3DDEVICE8 g_pDevice;

#ifndef SAFE_RELEASE
#define SAFE_RELEASE(p) do { if ((p)) { (p)->Release(); (p) = NULL; } } while (0)
#endif

static const float SW = 640.f;
static const float SH = 480.f;
static const float PI = 3.14159265f;
static const float TAU = 6.28318530f;
static const DWORD SCENE_MS = 20000;

static bool  s_active = false;
static DWORD s_startTicks = 0;
static DWORD s_lastTick = 0;
static float s_fps = 0.f;
static float s_frameMs = 0.f;
static int   s_drawCalls = 0;

// Init diagnostics (shows on real hardware)
static bool  s_initOK = false;
static char  s_initErr[128] = { 0 };

static void SetInitError(const char* msg)
{
    s_initOK = false;
    if (!msg) msg = "unknown";
    strncpy(s_initErr, msg, sizeof(s_initErr) - 1);
    s_initErr[sizeof(s_initErr) - 1] = 0;
    OutputDebugStringA("CrystalScene INIT ERROR: ");
    OutputDebugStringA(s_initErr);
    OutputDebugStringA("\n");
}

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------
static __forceinline int   Ftoi(float f) { int i; __asm {fld f}__asm {fistp i}return i; }
static __forceinline BYTE  ClampB(int v) { return v < 0 ? 0 : v > 255 ? 255 : (BYTE)v; }
static __forceinline float Clamp01(float f) { return f < 0.f ? 0.f : f > 1.f ? 1.f : f; }

static const int LUT_N = 1024;
static float s_sin[LUT_N], s_cos[LUT_N];
static bool  s_lutReady = false;
static void BuildLUT()
{
    if (s_lutReady) return;
    for (int i = 0; i < LUT_N; ++i) {
        float a = (float)i * TAU / (float)LUT_N;
        s_sin[i] = sinf(a); s_cos[i] = cosf(a);
    }
    s_lutReady = true;
}
static __forceinline float LSin(float r)
{
    int i = Ftoi(r * LUT_N / TAU);
    return s_sin[((i % LUT_N) + LUT_N) & (LUT_N - 1)];
}
static __forceinline float LCos(float r)
{
    int i = Ftoi(r * LUT_N / TAU);
    return s_cos[((i % LUT_N) + LUT_N) & (LUT_N - 1)];
}

// -----------------------------------------------------------------------------
// Vertex shader (kept as-is; not required for fixed-function path)
// -----------------------------------------------------------------------------
static DWORD s_vsHandle = 0;

static const DWORD s_vsDecl[] =
{
    D3DVSD_STREAM(0),
    D3DVSD_REG(D3DVSDE_POSITION, D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_NORMAL,   D3DVSDT_FLOAT3),
    D3DVSD_REG(D3DVSDE_TEXCOORD0,D3DVSDT_FLOAT2),
    D3DVSD_END()
};

static const char s_vsSource[] =
"vs.1.1\n"
"dp4 r0.x, v0, c4\n"
"dp4 r0.y, v0, c5\n"
"dp4 r0.z, v0, c6\n"
"mov r0.w, c13.z\n"
"dp4 oPos.x, v0, c0\n"
"dp4 oPos.y, v0, c1\n"
"dp4 oPos.z, v0, c2\n"
"dp4 oPos.w, v0, c3\n"
"dp3 r1.x, v1, c4\n"
"dp3 r1.y, v1, c5\n"
"dp3 r1.z, v1, c6\n"
"dp3 r2.x, r1, r1\n"
"rsq r2.x, r2.x\n"
"mul r1, r1, r2.xxxx\n"
"dp3 r3.x, r1, c7\n"
"max r3.x, r3.x, c13.x\n"
"mul r4, c9, r3.xxxx\n"
"dp3 r3.y, r1, c8\n"
"max r3.y, r3.y, c13.x\n"
"mul r5, c10, r3.yyyy\n"
"add r4, r4, c11\n"
"add r4, r4, r5\n"
"min r4, r4, c13.zzzz\n"
"mov r4.w, c13.z\n"
"mov oD0, r4\n"
"sub r6, c12, r0\n"
"dp3 r2.x, r6, r6\n"
"rsq r2.x, r2.x\n"
"mul r6, r6, r2.xxxx\n"
"dp3 r2.x, r1, r6\n"
"add r2.x, r2.x, r2.x\n"
"mul r7, r1, r2.xxxx\n"
"sub r7, r7, r6\n"
"mov oT1, r7\n"
"mov oT0, v2\n";

static void CreateVS()
{
    LPXGBUFFER pCode = NULL, pErr = NULL;
    if (SUCCEEDED(XGAssembleShader("CrystalVS", s_vsSource,
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

// -----------------------------------------------------------------------------
// Vertex formats
// -----------------------------------------------------------------------------
struct CVtx { float x, y, z, nx, ny, nz, u, v; };
struct GVtx { float x, y, z; DWORD color; };
static const DWORD GVFVF = D3DFVF_XYZ | D3DFVF_DIFFUSE;
struct CaveVtx { float x, y, z, nx, ny, nz, u, v; };
static const DWORD CAVEFVF = D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1;
static void DisableFrom(int s); // forward decl for DrawCave

// -----------------------------------------------------------------------------
// V3 math
// -----------------------------------------------------------------------------
struct V3 { float x, y, z; };
static V3  V3Sub(V3 a, V3 b) { return { a.x - b.x,a.y - b.y,a.z - b.z }; }
static V3  V3Mid(V3 a, V3 b) { return { (a.x + b.x) * .5f,(a.y + b.y) * .5f,(a.z + b.z) * .5f }; }
static V3  V3Cross(V3 a, V3 b) { return { a.y * b.z - a.z * b.y,a.z * b.x - a.x * b.z,a.x * b.y - a.y * b.x }; }
static float V3Len(V3 v) { return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z); }
static V3  V3Norm(V3 v) { float l = V3Len(v); return l > 1e-5f ? V3{ v.x / l,v.y / l,v.z / l } : V3{ 0,1,0 }; }

static V3 XformV3(V3 p, float sc,
    float rx, float ry, float rz,
    float tx, float ty, float tz)
{
    float x = p.x * sc, y = p.y * sc, z = p.z * sc;
    float y2 = y * cosf(rx) - z * sinf(rx), z2 = y * sinf(rx) + z * cosf(rx); y = y2; z = z2;
    float x2 = x * cosf(ry) + z * sinf(ry); z2 = -x * sinf(ry) + z * cosf(ry); x = x2; z = z2;
    x2 = x * cosf(rz) - y * sinf(rz); y2 = x * sinf(rz) + y * cosf(rz); x = x2; y = y2;
    return { x + tx,y + ty,z + tz };
}

// -----------------------------------------------------------------------------
// Subdivision
// -----------------------------------------------------------------------------
struct Tri { V3 a, b, c; };
static int SubdivideTris(const Tri* src, int n, Tri* dst)
{
    int out = 0;
    for (int i = 0; i < n; ++i) {
        V3 ab = V3Mid(src[i].a, src[i].b), bc = V3Mid(src[i].b, src[i].c), ca = V3Mid(src[i].c, src[i].a);
        dst[out++] = { src[i].a,ab,ca }; dst[out++] = { ab,src[i].b,bc };
        dst[out++] = { ca,bc,src[i].c }; dst[out++] = { ab,bc,ca };
    }
    return out;
}

// -----------------------------------------------------------------------------
// Glow colors
// -----------------------------------------------------------------------------
static const DWORD GLOW_COLORS[] = {
    D3DCOLOR_XRGB(210,60,210),
    D3DCOLOR_XRGB(40,220,255),
    D3DCOLOR_XRGB(130,30,220),
    D3DCOLOR_XRGB(255,80,180),
    D3DCOLOR_XRGB(200,220,255),
    D3DCOLOR_XRGB(180,20,255),
    D3DCOLOR_XRGB(20,255,200),
};
static const int NGLOW = 7;

// -----------------------------------------------------------------------------
// Write triangle into CVtx
// -----------------------------------------------------------------------------
static CVtx* WriteTri(CVtx* dst, V3 a, V3 b, V3 c)
{
    V3 n = V3Norm(V3Cross(V3Sub(b, a), V3Sub(c, a)));
    V3 pts[3] = { a,b,c };
    for (int k = 0; k < 3; ++k) {
        dst->x = pts[k].x; dst->y = pts[k].y; dst->z = pts[k].z;
        dst->nx = n.x; dst->ny = n.y; dst->nz = n.z;
        float len = V3Len(pts[k]);
        float nx_ = len > 1e-5f ? pts[k].x / len : 0.f;
        float ny_ = len > 1e-5f ? pts[k].y / len : 0.f;
        float nz_ = len > 1e-5f ? pts[k].z / len : 1.f;
        dst->u = atan2f(nx_, nz_) / TAU + 0.5f;
        dst->v = acosf(Clamp01(ny_)) / PI;
        dst++;
    }
    return dst;
}

// -----------------------------------------------------------------------------
// Build one prism
// -----------------------------------------------------------------------------
static int BuildPrism(CVtx* dst, GVtx* gdst, int* gout,
    float shH, float capH, float rad, float sc,
    float rx, float ry, float rz,
    float tx, float ty, float tz, DWORD glowCol)
{
    const int SIDES = 6;
    V3 ring[6], top[6];
    for (int i = 0; i < SIDES; ++i) {
        float a = PI / 6.f + i * TAU / SIDES;
        ring[i] = XformV3({ cosf(a) * rad,0.f,sinf(a) * rad }, sc, rx, ry, rz, tx, ty, tz);
        top[i] = XformV3({ cosf(a) * rad,shH,sinf(a) * rad }, sc, rx, ry, rz, tx, ty, tz);
    }
    V3 apex = XformV3({ 0.f,shH + capH,0.f }, sc, rx, ry, rz, tx, ty, tz);
    V3 base = XformV3({ 0.f,0.f,0.f }, sc, rx, ry, rz, tx, ty, tz);

    static Tri bA[24], bufA[1536], bufB[1536];
    int bn = 0;
    for (int i = 0; i < SIDES; ++i) {
        int j = (i + 1) % SIDES;
        bA[bn++] = { ring[i],top[i],ring[j] }; bA[bn++] = { ring[j],top[i],top[j] };
        bA[bn++] = { top[i],apex,top[j] };     bA[bn++] = { base,ring[j],ring[i] };
    }
    Tri* src = bufA, * tmp = bufB;
    for (int i = 0; i < bn; ++i) src[i] = bA[i];
    int n = bn;
    for (int p = 0; p < 3; ++p) { n = SubdivideTris(src, n, tmp); Tri* sw = src; src = tmp; tmp = sw; }

    CVtx* p = dst;
    for (int i = 0; i < n; ++i) {
        p = WriteTri(p, src[i].a, src[i].b, src[i].c);
        (void)gdst; (void)gout; (void)glowCol; // glow removed
    }
    return n;
}

// -----------------------------------------------------------------------------
// Prism definitions (PATCH 2: MINIMAL GEOMETRY TEST)
// -----------------------------------------------------------------------------
// Reduce to a single prism so we can rule out VB size / heap fragmentation / build overflow.
struct CDef { float shH, capH, rad, sc, rx, ry, rz, tx, ty, tz; };
static const int ND = 14;
static const CDef s_def[ND] = {
    {1.8f,0.70f,0.35f,1.f, 0.10f, 0.30f, 0.00f, 0.00f,-0.90f, 0.00f},
    {1.5f,0.60f,0.28f,1.f,-0.15f, 0.80f, 0.10f,-0.10f,-0.85f, 0.15f},
    {1.2f,0.50f,0.24f,1.f, 0.20f,-0.40f, 0.05f, 0.15f,-0.80f,-0.10f},
    {0.9f,0.40f,0.18f,1.f, 1.10f, 0.00f, 0.00f, 0.50f,-0.60f, 0.20f},
    {0.8f,0.35f,0.16f,1.f,-0.90f, 0.50f, 0.20f,-0.45f,-0.50f, 0.10f},
    {1.0f,0.40f,0.20f,1.f, 0.80f, 1.20f,-0.30f, 0.20f,-0.70f, 0.50f},
    {0.7f,0.30f,0.15f,1.f,-0.70f,-0.80f, 0.40f,-0.30f,-0.40f,-0.50f},
    {0.85f,0.35f,0.17f,1.f,1.30f, 0.30f, 0.50f, 0.00f,-0.80f,-0.60f},
    {0.6f,0.28f,0.14f,1.f, 0.60f,-1.10f,-0.20f,-0.60f,-0.50f, 0.30f},
    {0.75f,0.32f,0.15f,1.f, 1.50f,-0.30f, 0.60f, 0.80f,-0.35f, 0.10f},
    {0.70f,0.30f,0.14f,1.f,-1.40f, 0.60f,-0.40f,-0.75f,-0.30f,-0.20f},
    {0.65f,0.28f,0.13f,1.f, 0.50f, 1.60f, 0.30f, 0.10f,-0.45f, 0.80f},
    {0.60f,0.26f,0.12f,1.f,-0.60f,-1.50f,-0.50f,-0.15f,-0.35f,-0.80f},
    {0.40f,0.20f,0.09f,1.f, 1.80f, 0.40f, 0.20f, 0.70f,-0.30f, 0.00f},
};

// -----------------------------------------------------------------------------
// Geometry state
// -----------------------------------------------------------------------------
static LPDIRECT3DVERTEXBUFFER8 s_crystalVB = NULL;
static LPDIRECT3DVERTEXBUFFER8 s_glowVB = NULL;
static LPDIRECT3DVERTEXBUFFER8 s_fogVB = NULL;
static int s_crystalTris = 0;
static int s_glowTris = 0;
static int s_fogTris = 0;

// Fog puffs
static const int FOG_PUFFS = 120;
static const int FOG_VERTS = FOG_PUFFS * 12;

// Forward declarations for textures used in DrawCave
static LPDIRECT3DTEXTURE8     s_normalMap = NULL;
static LPDIRECT3DCUBETEXTURE8 s_cubeMap = NULL;

// Cave mesh
static const int CAVE_SEGS = 192;
static const int CAVE_RINGS = 96;
static LPDIRECT3DVERTEXBUFFER8 s_caveVB = NULL;
static LPDIRECT3DINDEXBUFFER8  s_caveIB = NULL;
static LPDIRECT3DTEXTURE8      s_caveTex = NULL;
static bool                    s_rockDDSOK = false;
static int s_caveVerts = 0;
static int s_cavePrims = 0;


// =============================================================================
// Cave mesh -- textured lit cylinder, camera sits inside
// =============================================================================
static void BuildCave()
{
    SAFE_RELEASE(s_caveVB); SAFE_RELEASE(s_caveIB); SAFE_RELEASE(s_caveTex);
    const int   SEGS = CAVE_SEGS, RINGS = CAVE_RINGS;
    const float RADIUS = 6.0f, DEPTH = 14.0f, ZNEAR = -2.0f;

    s_caveVerts = (SEGS + 1) * (RINGS + 1);
    s_cavePrims = SEGS * RINGS * 2;

    HRESULT hrV = g_pDevice->CreateVertexBuffer(
        s_caveVerts * sizeof(CaveVtx), D3DUSAGE_WRITEONLY, 0,
        D3DPOOL_MANAGED, &s_caveVB);
    if (FAILED(hrV) || !s_caveVB)
        hrV = g_pDevice->CreateVertexBuffer(
            s_caveVerts * sizeof(CaveVtx), D3DUSAGE_WRITEONLY, 0,
            D3DPOOL_DEFAULT, &s_caveVB);
    if (FAILED(hrV) || !s_caveVB) return;

    HRESULT hrI = g_pDevice->CreateIndexBuffer(
        s_cavePrims * 3 * sizeof(WORD), D3DUSAGE_WRITEONLY,
        D3DFMT_INDEX16, D3DPOOL_MANAGED, &s_caveIB);
    if (FAILED(hrI) || !s_caveIB)
        hrI = g_pDevice->CreateIndexBuffer(
            s_cavePrims * 3 * sizeof(WORD), D3DUSAGE_WRITEONLY,
            D3DFMT_INDEX16, D3DPOOL_DEFAULT, &s_caveIB);
    if (FAILED(hrI) || !s_caveIB) { SAFE_RELEASE(s_caveVB); return; }

    CaveVtx* vb = NULL; s_caveVB->Lock(0, 0, (BYTE**)&vb, 0);
    for (int ring = 0; ring <= RINGS; ++ring) {
        float frac = (float)ring / (float)RINGS;
        float z = ZNEAR + frac * DEPTH;
        for (int seg = 0; seg <= SEGS; ++seg) {
            float a = (float)seg * TAU / (float)SEGS;
            unsigned int h = (unsigned int)(seg * 1664525 + ring * 22695477 + 1013904223);
            h ^= (h >> 16); h *= 0x45d9f3b; h ^= (h >> 16);
            float jag = RADIUS + ((h >> 24) & 0x0F) * 0.04f;
            float cx = cosf(a) * jag, cy = sinf(a) * jag;
            float nl = sqrtf(cx * cx + cy * cy);
            CaveVtx& v = vb[ring * (SEGS + 1) + seg];
            v.x = cx; v.y = cy; v.z = z;
            v.nx = -(cx / nl); v.ny = -(cy / nl); v.nz = 0.f;
            v.u = (float)seg / (float)SEGS * 6.f;
            v.v = frac * 4.f;
        }
    }
    s_caveVB->Unlock();

    WORD* ib = NULL; s_caveIB->Lock(0, 0, (BYTE**)&ib, 0);
    int idx = 0;
    for (int ring = 0; ring < RINGS; ++ring)
        for (int seg = 0; seg < SEGS; ++seg) {
            WORD tl = (WORD)(ring * (SEGS + 1) + seg), tr = tl + 1;
            WORD bl = (WORD)((ring + 1) * (SEGS + 1) + seg), br = bl + 1;
            ib[idx++] = tl; ib[idx++] = bl; ib[idx++] = tr;
            ib[idx++] = tr; ib[idx++] = bl; ib[idx++] = br;
        }
    s_caveIB->Unlock();

    // Load rock texture from DVD
    s_rockDDSOK = SUCCEEDED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\rock.dds", &s_caveTex)) && s_caveTex != NULL;
    if (!s_rockDDSOK) {
        // Bright checkerboard -- pink/green so we can see if geometry works
        g_pDevice->CreateTexture(8, 8, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_caveTex);
        if (s_caveTex) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(s_caveTex->LockRect(0, &lr, NULL, 0)) && lr.pBits) {
                DWORD* px = (DWORD*)lr.pBits;
                for (int y = 0; y < 8; ++y) for (int x = 0; x < 8; ++x)
                    px[y * 8 + x] = ((x + y) & 1) ? D3DCOLOR_ARGB(255, 255, 100, 255) : D3DCOLOR_ARGB(255, 100, 255, 100);
                s_caveTex->UnlockRect(0);
            }
        }
    }
}

static void DrawCave(DWORD tMs, float alpha)
{
    if (!s_caveVB || !s_caveIB || !s_caveTex || s_cavePrims <= 0) return;
    float t = (float)tMs * 0.001f;

    D3DXVECTOR3 eye(0.f, 0.3f, -5.f), at(0.f, 0.f, 0.f), up(0.f, 1.f, 0.f);
    D3DXMATRIX view, proj, mWorld;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, PI / 3.2f, SW / SH, 0.1f, 50.f);
    D3DXMatrixRotationY(&mWorld, t * 0.03f);
    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);
    g_pDevice->SetTransform(D3DTS_WORLD, &mWorld);

    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE); // cave is bg -- no Z test
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);  // additive: texture adds onto scene
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE); // no culling -- see both sides
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    // Crystal glow colour pulsing on cave walls -- mimics the cluster as a light source
    // Slow pulse with warm violet tint, bright enough to illuminate the cave dimly
    int gph = (tMs / 12u) & (LUT_N - 1);
    float glow = 0.18f + s_sin[gph] * 0.07f;  // 0.11 - 0.25 range -- very dim cave glow
    BYTE gR = ClampB(Ftoi(glow * 60.f * alpha)); // very subtle violet tint
    BYTE gG = ClampB(Ftoi(glow * 30.f * alpha));
    BYTE gB = ClampB(Ftoi(glow * 80.f * alpha));
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(gR, gG, gB));

    // Raw texture -- no stage multiplication darkening it
    // TFACTOR tint via ADD in stage 1 so glow adds brightness not multiplies
    g_pDevice->SetTexture(0, s_caveTex);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    // Stage 1: ADD glow colour on top of texture -- pure additive tint
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_ADD);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    DisableFrom(2);
    g_pDevice->SetVertexShader(CAVEFVF);
    g_pDevice->SetStreamSource(0, s_caveVB, sizeof(CaveVtx));
    g_pDevice->SetIndices(s_caveIB, 0);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s_caveVerts, 0, s_cavePrims);

    // Stalactites/stalagmites -- bright solid triangles pointing inward from cave wall
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE);
    DisableFrom(1);

    struct CV { float x, y, z; DWORD color; };
    const float ZN = -2.f, DP = 14.f, RD = 6.f;

    // Mini crystal formations -- hex prisms with pointed tips, poking out of cave wall
    // Each crystal: 6 side faces + 6 tip faces = 12 tris, pure geometry no texture
    auto DrawMiniCrystal = [&](float wx, float wy, float zp,
        float nx2, float ny2,
        float shaftLen, float capLen, float rad,
        DWORD cBase, DWORD cMid, DWORD cTip)
        {
            // Perpendicular axes for hex ring
            // nx2,ny2 = inward axis; build two perp vectors in XY plane
            float bx = -ny2, by = nx2;        // tangent in XY
            // 6 base verts on cave wall surface
            CV base[6], mid[6];
            for (int k = 0; k < 6; ++k) {
                float a2 = (float)k / 6.f * TAU;
                // Tangential spread only -- base ring flush on wall, no radial displacement
                float tx = bx * cosf(a2) * rad;
                float ty = by * cosf(a2) * rad;
                float tz = sinf(a2) * rad;  // Z spread for depth
                base[k].x = wx + tx;                    base[k].y = wy + ty;                    base[k].z = zp + tz; base[k].color = cBase;
                mid[k].x = wx + tx * 0.6f + nx2 * shaftLen; mid[k].y = wy + ty * 0.6f + ny2 * shaftLen; mid[k].z = zp + tz * 0.6f; mid[k].color = cMid;
            }
            CV tip; tip.x = wx + nx2 * (shaftLen + capLen); tip.y = wy + ny2 * (shaftLen + capLen); tip.z = zp; tip.color = cTip;

            // 6 shaft side quads (2 tris each = 12 tris)
            for (int k = 0; k < 6; ++k) {
                int n = (k + 1) % 6;
                CV q[6] = { base[k], base[n], mid[k], base[n], mid[n], mid[k] };
                g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(CV));
            }
            // 6 cap tris to tip
            for (int k = 0; k < 6; ++k) {
                int n = (k + 1) % 6;
                CV t3[3] = { mid[k], mid[n], tip };
                g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t3, sizeof(CV));
            }
        };

    // 32 stalactites -- violet/blue crystals
    for (int i = 0; i < 32; ++i) {
        float ang = (float)i * 0.71f;
        float zp = ZN + 0.8f + (float)i * (DP / 32.f);
        float wx = cosf(ang) * RD;
        float wy = sinf(ang) * RD;
        float nx2 = -cosf(ang), ny2 = -sinf(ang);
        float sLen = 0.25f + (float)(i % 4) * 0.18f + (float)(i % 3) * 0.10f;
        float cLen = 0.20f + (float)(i % 3) * 0.12f;
        float rad = 0.06f + (float)(i % 4) * 0.025f;
        int iph = (i * 97 + (tMs / 25u)) & (LUT_N - 1);
        float br = 0.70f + s_sin[iph] * 0.30f;
        DWORD cBase = D3DCOLOR_XRGB(ClampB(Ftoi(br * 80.f * alpha)), ClampB(Ftoi(br * 30.f * alpha)), ClampB(Ftoi(br * 140.f * alpha)));
        DWORD cMid = D3DCOLOR_XRGB(ClampB(Ftoi(br * 180.f * alpha)), ClampB(Ftoi(br * 80.f * alpha)), ClampB(Ftoi(br * 255.f * alpha)));
        DWORD cTip = D3DCOLOR_XRGB(ClampB(Ftoi(br * 220.f * alpha)), ClampB(Ftoi(br * 180.f * alpha)), ClampB(Ftoi(br * 255.f * alpha)));
        DrawMiniCrystal(wx, wy, zp, nx2, ny2, sLen, cLen, rad, cBase, cMid, cTip);
    }
    // 24 stalagmites -- pink/magenta crystals for contrast
    for (int i = 0; i < 24; ++i) {
        float ang = (float)i * 0.83f + PI * 0.5f;
        float zp = ZN + 0.8f + (float)i * (DP / 24.f);
        float wx = cosf(ang) * RD;
        float wy = sinf(ang) * RD;
        float nx2 = -cosf(ang), ny2 = -sinf(ang);
        float sLen = 0.22f + (float)(i % 3) * 0.16f + (float)(i % 5) * 0.09f;
        float cLen = 0.18f + (float)(i % 4) * 0.10f;
        float rad = 0.05f + (float)(i % 3) * 0.025f;
        int iph = (i * 61 + (tMs / 30u)) & (LUT_N - 1);
        float br = 0.65f + s_sin[iph] * 0.35f;
        DWORD cBase = D3DCOLOR_XRGB(ClampB(Ftoi(br * 140.f * alpha)), ClampB(Ftoi(br * 20.f * alpha)), ClampB(Ftoi(br * 100.f * alpha)));
        DWORD cMid = D3DCOLOR_XRGB(ClampB(Ftoi(br * 255.f * alpha)), ClampB(Ftoi(br * 80.f * alpha)), ClampB(Ftoi(br * 200.f * alpha)));
        DWORD cTip = D3DCOLOR_XRGB(ClampB(Ftoi(br * 255.f * alpha)), ClampB(Ftoi(br * 200.f * alpha)), ClampB(Ftoi(br * 255.f * alpha)));
        DrawMiniCrystal(wx, wy, zp, nx2, ny2, sLen, cLen, rad, cBase, cMid, cTip);
    }

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->LightEnable(1, FALSE);
    g_pDevice->LightEnable(2, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    DisableFrom(0);
    D3DXMATRIX id; D3DXMatrixIdentity(&id);
    g_pDevice->SetTransform(D3DTS_WORLD, &id);
}

// =============================================================================
// Billboarded fog puffs
struct FogPuff { float x, y, z, radius; BYTE alpha; };
static FogPuff s_puffs[FOG_PUFFS];

static void BuildFogDisc()
{
    SAFE_RELEASE(s_fogVB); s_fogTris = 0;
    if (FAILED(g_pDevice->CreateVertexBuffer(FOG_VERTS * sizeof(GVtx),
        D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s_fogVB))) return;
    int pi = 0;
    for (int p = 0; p < FOG_PUFFS; ++p) {
        const CDef& c = s_def[pi % ND]; pi++;
        float ang = (float)p * 2.3998f;
        float dist = c.rad * (1.0f + (float)(p % 3) * 0.6f);
        s_puffs[p].x = c.tx + cosf(ang) * dist;
        s_puffs[p].y = c.ty + 0.05f + (float)(p % 4) * 0.06f;
        s_puffs[p].z = c.tz + sinf(ang) * dist;
        s_puffs[p].radius = 0.28f + c.rad * 1.80f + (float)(p % 4) * 0.09f;
        s_puffs[p].alpha = (BYTE)(110 + (p % 5) * 14);
    }
}

static void UpdateFogVB(const D3DXMATRIX& view, DWORD tMs)
{
    if (!s_fogVB) return;
    float rx = view._11, ry = view._21, rz = view._31;
    float ux = view._12, uy = view._22, uz = view._32;
    GVtx* vb = NULL;
    if (FAILED(s_fogVB->Lock(0, 0, (BYTE**)&vb, 0))) return;
    GVtx* dst = vb; s_fogTris = 0;
    const BYTE fR = 40, fG = 15, fB = 80;
    for (int p = 0; p < FOG_PUFFS; ++p) {
        const FogPuff& pf = s_puffs[p];
        int ph = ((tMs / 8u) + p * 37) & (LUT_N - 1);
        float pulse = 0.80f + s_sin[ph] * 0.20f;
        BYTE a = (BYTE)Ftoi((float)pf.alpha * pulse);
        DWORD cCore = D3DCOLOR_ARGB(a, fR, fG, fB), cEdge = D3DCOLOR_ARGB(0, fR, fG, fB);
        float r = pf.radius, cx = pf.x, cy = pf.y, cz = pf.z;
        float rpx = rx * r, rpy = ry * r, rpz = rz * r, upx = ux * r, upy = uy * r, upz = uz * r;
        float ox = cx, oy = cy, oz = cz;
        float tx = cx + upx, ty_ = cy + upy, tz = cz + upz;
        float bx = cx - upx, by = cy - upy, bz = cz - upz;
        float lx = cx - rpx, ly = cy - rpy, lz = cz - rpz;
        float wrx = cx + rpx, wry = cy + rpy, wrz = cz + rpz;
        dst->x = ox; dst->y = oy; dst->z = oz; dst->color = cCore; dst++;
        dst->x = tx; dst->y = ty_; dst->z = tz; dst->color = cEdge; dst++;
        dst->x = lx; dst->y = ly; dst->z = lz; dst->color = cEdge; dst++;
        dst->x = ox; dst->y = oy; dst->z = oz; dst->color = cCore; dst++;
        dst->x = wrx; dst->y = wry; dst->z = wrz; dst->color = cEdge; dst++;
        dst->x = tx; dst->y = ty_; dst->z = tz; dst->color = cEdge; dst++;
        dst->x = ox; dst->y = oy; dst->z = oz; dst->color = cCore; dst++;
        dst->x = bx; dst->y = by; dst->z = bz; dst->color = cEdge; dst++;
        dst->x = wrx; dst->y = wry; dst->z = wrz; dst->color = cEdge; dst++;
        dst->x = ox; dst->y = oy; dst->z = oz; dst->color = cCore; dst++;
        dst->x = lx; dst->y = ly; dst->z = lz; dst->color = cEdge; dst++;
        dst->x = bx; dst->y = by; dst->z = bz; dst->color = cEdge; dst++;
        s_fogTris += 4;
    }
    s_fogVB->Unlock();
}

// =============================================================================
// Robust geometry build: checks hr + lock, and avoids NULL deref hard-locks.
static bool BuildGeometry()
{
    SAFE_RELEASE(s_crystalVB);
    SAFE_RELEASE(s_glowVB);

    s_crystalTris = 0;
    s_glowTris = 0;

    if (!g_pDevice) { SetInitError("BuildGeometry: no device"); return false; }

    // Exact sizing:
    // base tris per prism = 24
    // 3 subdivision passes => *4^3 = *64
    // => 1536 tris per prism
    const int TRIS_PER_PRISM = 24 * 64;   // 1536
    const int TOTAL_TRIS = ND * TRIS_PER_PRISM;
    const int TOTAL_VERTS = TOTAL_TRIS * 3;
    const int VB_BYTES = TOTAL_VERTS * (int)sizeof(CVtx);

    HRESULT hr = g_pDevice->CreateVertexBuffer(
        VB_BYTES, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &s_crystalVB);

    // Fallback pool (some real-hardware scenarios behave better with DEFAULT)
    if (FAILED(hr) || !s_crystalVB) {
        hr = g_pDevice->CreateVertexBuffer(
            VB_BYTES, D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &s_crystalVB);
    }

    if (FAILED(hr) || !s_crystalVB) {
        SetInitError("BuildGeometry: CreateVertexBuffer failed");
        return false;
    }

    CVtx* cvb = NULL;
    hr = s_crystalVB->Lock(0, 0, (BYTE**)&cvb, 0);
    if (FAILED(hr) || !cvb) {
        SetInitError("BuildGeometry: VB Lock failed");
        SAFE_RELEASE(s_crystalVB);
        return false;
    }

    CVtx* cdst = cvb;

    for (int i = 0; i < ND; ++i) {
        const CDef& c = s_def[i];
        DWORD gcol = GLOW_COLORS[i % NGLOW];
        int n = BuildPrism(cdst, NULL, NULL,
            c.shH, c.capH, c.rad, c.sc,
            c.rx, c.ry, c.rz, c.tx, c.ty, c.tz, gcol);
        s_crystalTris += n;
        cdst += n * 3;
    }

    s_crystalVB->Unlock();

    // Non-fatal sanity check
    if (s_crystalTris != TOTAL_TRIS) {
        OutputDebugStringA("CrystalScene: WARNING tri count mismatch\n");
    }

    return true;
}

// -----------------------------------------------------------------------------
// DOT3 light direction for normal map pass
// -----------------------------------------------------------------------------
static void SetDOT3TFactor(DWORD tMs)
{
    float t = (float)tMs * 0.001f;
    float la = t * 0.22f;
    float dlx = -cosf(la), dly = -0.6f - 0.3f * sinf(t * 0.17f), dlz = -sinf(la);
    float len = sqrtf(dlx * dlx + dly * dly + dlz * dlz); if (len < 1e-4f) len = 1.f;
    dlx /= len; dly /= len; dlz /= len;
    BYTE r = ClampB(Ftoi((dlx + 1.f) * 127.5f));
    BYTE g = ClampB(Ftoi((-dly + 1.f) * 127.5f));
    BYTE b = ClampB(Ftoi((dlz + 1.f) * 127.5f));
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_ARGB(255, r, g, b));
}

// -----------------------------------------------------------------------------
// Textures (declared above for forward visibility in DrawCave)
// -----------------------------------------------------------------------------

static void GenTextures()
{
    SAFE_RELEASE(s_normalMap);
    SAFE_RELEASE(s_cubeMap);

    // NOTE: if D:\ is not a DVD in your run mode, these can fail.
    if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\crystal_n.dds", &s_normalMap)) || !s_normalMap)
    {
        g_pDevice->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_normalMap);
        if (s_normalMap) {
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(s_normalMap->LockRect(0, &lr, NULL, 0)) && lr.pBits) {
                DWORD* p = (DWORD*)lr.pBits;
                for (int i = 0; i < 16; ++i) p[i] = D3DCOLOR_ARGB(255, 128, 128, 255);
                s_normalMap->UnlockRect(0);
            }
        }
    }

    if (FAILED(D3DXCreateCubeTextureFromFileA(g_pDevice, "D:\\tex\\crystal_cube.dds", &s_cubeMap)) || !s_cubeMap)
    {
        g_pDevice->CreateCubeTexture(4, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_cubeMap);
        if (s_cubeMap) {
            for (int face = 0; face < 6; ++face) {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(s_cubeMap->LockRect((D3DCUBEMAP_FACES)face, 0, &lr, NULL, 0)) && lr.pBits) {
                    DWORD* p = (DWORD*)lr.pBits;
                    DWORD cols[6] = { 0xFFD23CD2,0xFF28DCFF,0xFF821EDB,0xFFFF50B4,0xFFC8DCFF,0xFF050010 };
                    for (int i = 0; i < 16; ++i) p[i] = cols[face];
                    s_cubeMap->UnlockRect((D3DCUBEMAP_FACES)face, 0);
                }
            }
        }
    }
}

// -----------------------------------------------------------------------------
// State helpers
// -----------------------------------------------------------------------------
static void SetLinear(int s)
{
    g_pDevice->SetTextureStageState(s, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(s, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(s, D3DTSS_MIPFILTER, D3DTEXF_NONE);
}
static void DisableFrom(int s)
{
    for (int i = s; i < 4; ++i) {
        g_pDevice->SetTexture(i, NULL);
        g_pDevice->SetTextureStageState(i, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(i, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(i, D3DTSS_TEXCOORDINDEX, i);
        g_pDevice->SetTextureStageState(i, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
}

// -----------------------------------------------------------------------------
// Background
// -----------------------------------------------------------------------------
static void DrawBackground(DWORD tMs, float alpha)
{
    // Black clear first so cave composites cleanly
    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);
    DrawCave(tMs, alpha);
}

// -----------------------------------------------------------------------------
// Render cluster
// -----------------------------------------------------------------------------

static void RenderCluster(DWORD tMs, float alpha, const D3DXMATRIX& world)
{
    if (!s_crystalVB || s_crystalTris <= 0) return; // never attempt draw on missing geometry

    float t = (float)tMs * 0.001f;

    D3DXVECTOR3 eye(0.f, 0.3f, -5.f), at(0.f, 0.f, 0.f), up(0.f, 1.f, 0.f);
    D3DXMATRIX view, proj;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, PI / 3.2f, SW / SH, 0.1f, 50.f);
    g_pDevice->SetTransform(D3DTS_WORLD, &world);
    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);

    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

    g_pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1);
    g_pDevice->SetStreamSource(0, s_crystalVB, sizeof(CVtx));
    SetLinear(0); SetLinear(1);

    // -------------------------------------------------------------------------
    // PASS 1 (opaque): DOT3 base -- primary lit colour, blended with TFACTOR
    // -------------------------------------------------------------------------
    SetDOT3TFactor(tMs);

    int phK = (tMs / 7u) & (LUT_N - 1);
    float kp = 0.85f + s_sin[phK] * 0.15f;
    BYTE kR = ClampB(Ftoi(kp * 140.f * alpha));
    BYTE kG = ClampB(Ftoi(kp * 40.f * alpha));
    BYTE kB = ClampB(Ftoi(kp * 140.f * alpha));

    g_pDevice->SetTexture(0, s_normalMap);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_WRAP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_WRAP);
    g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(kR, kG, kB));
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE2X);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    DisableFrom(2);
    g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;

    // Additive passes
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    // -------------------------------------------------------------------------
    // PASS 2 (additive): DOT3 cyan + violet combined -- two light dirs merged
    // Stage 0: DOT3 with cyan light dir
    // Stage 1: ADD with second DOT3 result -- not possible in single pass so
    //          use averaged TFACTOR of both dirs for a single cheap approximation
    // -------------------------------------------------------------------------
    {
        float la = t * 0.22f + PI;
        float flx = cosf(la), fly = -0.5f, flz = sinf(la);
        float fl = sqrtf(flx * flx + fly * fly + flz * flz); flx /= fl; fly /= fl; flz /= fl;
        float la2 = t * 0.22f + PI * 0.5f;
        float sx = cosf(la2), sy = 0.7f, sz = sinf(la2);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        // Average the two light directions for a blended DOT3
        BYTE fR = ClampB(Ftoi(((flx + 1.f) * .5f + (sx + 1.f) * .5f) * 0.5f * 255.f));
        BYTE fG = ClampB(Ftoi(((-fly + 1.f) * .5f + (-sy + 1.f) * .5f) * 0.5f * 180.f));
        BYTE fB = ClampB(Ftoi(((flz + 1.f) * .5f + (sz + 1.f) * .5f) * 0.5f * 255.f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(fR, fG, fB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }

    // PASS 5 (additive): violet specular DOT3
    {
        float la = t * 0.13f + PI * 1.25f, sx = cosf(la), sy = -0.5f, sz = sinf(la);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        BYTE sR = ClampB(Ftoi((sx + 1.f) * 100.f * alpha));
        BYTE sG = ClampB(Ftoi((-sy + 1.f) * 30.f * alpha));
        BYTE sB = ClampB(Ftoi((sz + 1.f) * 127.5f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(sR, sG, sB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }

    // PASS 6 (additive): cyan glint cubemap
    {
        int ph = (tMs / 4u) & (LUT_N - 1); float rp = 0.12f + s_sin[ph] * 0.10f;
        BYTE cR = ClampB(Ftoi(rp * 40.f * alpha));
        BYTE cG = ClampB(Ftoi(rp * 220.f * alpha));
        BYTE cB = ClampB(Ftoi(rp * 255.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    // PASS 7 (additive): hot pink DOT3 sweep
    {
        float la = t * 0.29f + PI * 0.33f, sx = cosf(la), sy = 0.6f, sz = sinf(la);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        BYTE sR = ClampB(Ftoi((sx + 1.f) * 127.5f * alpha));
        BYTE sG = ClampB(Ftoi((-sy + 1.f) * 40.f * alpha));
        BYTE sB = ClampB(Ftoi((sz + 1.f) * 110.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(sR, sG, sB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }

    // PASS 8 (additive): violet cubemap sweep
    {
        int ph = (tMs / 17u) & (LUT_N - 1); float rp = 0.20f + s_sin[ph] * 0.15f;
        BYTE vR = ClampB(Ftoi(rp * 130.f * alpha));
        BYTE vG = ClampB(Ftoi(rp * 30.f * alpha));
        BYTE vB = ClampB(Ftoi(rp * 220.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(vR, vG, vB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    // -------------------------------------------------------------------------
    // PASS 3 (additive): cubemap reflection -- all three cubemap passes merged
    //   stage 0: cubemap sample modulated by blended TFACTOR (white+violet+pink)
    // -------------------------------------------------------------------------
    {
        int ph = (tMs / 6u) & (LUT_N - 1); float rp = 0.35f + s_sin[ph] * 0.10f;
        int ph2 = (tMs / 13u) & (LUT_N - 1); float rp2 = 0.25f + s_sin[ph2] * 0.10f;
        int ph3 = (tMs / 9u) & (LUT_N - 1); float rp3 = 0.20f + s_sin[ph3] * 0.12f;
        // Blend all three cubemap tints into one TFACTOR
        BYTE cR = ClampB(Ftoi((rp * 255.f + rp2 * 130.f + rp3 * 255.f) / 3.f * alpha));
        BYTE cG = ClampB(Ftoi((rp * 255.f + rp2 * 30.f + rp3 * 80.f) / 3.f * alpha));
        BYTE cB = ClampB(Ftoi((rp * 255.f + rp2 * 220.f + rp3 * 180.f) / 3.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    // -------------------------------------------------------------------------
    // PASS 4 (additive): rim DOT3 -- cyan + violet rim merged into one
    // -------------------------------------------------------------------------
    {
        float la = t * 0.22f + PI * 1.5f, rx = cosf(la), ry = -0.2f, rz = sinf(la);
        float rl = sqrtf(rx * rx + ry * ry + rz * rz); rx /= rl; ry /= rl; rz /= rl;
        float la2 = t * 0.31f + PI * 0.75f, rx2 = cosf(la2), ry2 = 0.3f, rz2 = sinf(la2);
        float rl2 = sqrtf(rx2 * rx2 + ry2 * ry2 + rz2 * rz2); rx2 /= rl2; ry2 /= rl2; rz2 /= rl2;
        // Average rim directions
        BYTE rR = ClampB(Ftoi(((rx + 1.f) * .5f + (rx2 + 1.f) * .5f) * .5f * 180.f));
        BYTE rG = ClampB(Ftoi(((-ry + 1.f) * .5f + (-ry2 + 1.f) * .5f) * .5f * 180.f));
        BYTE rB = ClampB(Ftoi(((rz + 1.f) * .5f + (rz2 + 1.f) * .5f) * .5f * 255.f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rR, rG, rB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }

    // PASS 9 (additive): DOT3 warm pink sweep
    {
        float la = t * 0.17f + PI * 0.6f, sx = cosf(la), sy = -0.8f, sz = sinf(la);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        BYTE sR = ClampB(Ftoi((sx + 1.f) * 127.5f * alpha));
        BYTE sG = ClampB(Ftoi((-sy + 1.f) * 50.f * alpha));
        BYTE sB = ClampB(Ftoi((sz + 1.f) * 90.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(sR, sG, sB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }
    // PASS 10 (additive): cubemap teal glint
    {
        int ph = (tMs / 11u) & (LUT_N - 1); float rp = 0.18f + s_sin[ph] * 0.12f;
        BYTE cR = ClampB(Ftoi(rp * 20.f * alpha));
        BYTE cG = ClampB(Ftoi(rp * 200.f * alpha));
        BYTE cB = ClampB(Ftoi(rp * 180.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    // PASS 11 (additive): DOT3 deep blue rim
    {
        float la = t * 0.41f + PI * 1.1f, rx = cosf(la), ry = 0.5f, rz = sinf(la);
        float rl = sqrtf(rx * rx + ry * ry + rz * rz); rx /= rl; ry /= rl; rz /= rl;
        BYTE rR = ClampB(Ftoi((rx + 1.f) * 40.f * alpha));
        BYTE rG = ClampB(Ftoi((-ry + 1.f) * 80.f * alpha));
        BYTE rB = ClampB(Ftoi((rz + 1.f) * 127.5f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rR, rG, rB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }
    // PASS 12 (additive): cubemap white shimmer
    {
        int ph = (tMs / 5u) & (LUT_N - 1); float rp = 0.15f + s_sin[ph] * 0.10f;
        BYTE cR = ClampB(Ftoi(rp * 200.f * alpha));
        BYTE cG = ClampB(Ftoi(rp * 200.f * alpha));
        BYTE cB = ClampB(Ftoi(rp * 200.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    // PASS 13 (additive): DOT3 green-blue sweep
    {
        float la = t * 0.23f + PI * 1.8f, sx = cosf(la), sy = 0.3f, sz = sinf(la);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        BYTE sR = ClampB(Ftoi((sx + 1.f) * 30.f * alpha));
        BYTE sG = ClampB(Ftoi((-sy + 1.f) * 127.5f * alpha));
        BYTE sB = ClampB(Ftoi((sz + 1.f) * 127.5f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(sR, sG, sB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }
    // PASS 14 (additive): cubemap deep violet pulse
    {
        int ph = (tMs / 19u) & (LUT_N - 1); float rp = 0.22f + s_sin[ph] * 0.18f;
        BYTE cR = ClampB(Ftoi(rp * 160.f * alpha));
        BYTE cG = ClampB(Ftoi(rp * 20.f * alpha));
        BYTE cB = ClampB(Ftoi(rp * 255.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }
    // PASS 15 (additive): DOT3 orange-red accent
    {
        float la = t * 0.37f + PI * 0.9f, sx = cosf(la), sy = -0.4f, sz = sinf(la);
        float sl = sqrtf(sx * sx + sy * sy + sz * sz); sx /= sl; sy /= sl; sz /= sl;
        BYTE sR = ClampB(Ftoi((sx + 1.f) * 127.5f * alpha));
        BYTE sG = ClampB(Ftoi((-sy + 1.f) * 60.f * alpha));
        BYTE sB = ClampB(Ftoi((sz + 1.f) * 30.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(sR, sG, sB));
        g_pDevice->SetTexture(0, s_normalMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
    }
    // PASS 16 (additive): cubemap final gold sweep
    {
        int ph = (tMs / 23u) & (LUT_N - 1); float rp = 0.20f + s_sin[ph] * 0.15f;
        BYTE cR = ClampB(Ftoi(rp * 255.f * alpha));
        BYTE cG = ClampB(Ftoi(rp * 160.f * alpha));
        BYTE cB = ClampB(Ftoi(rp * 30.f * alpha));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(cR, cG, cB));
        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);
        DisableFrom(1);
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_crystalTris); ++s_drawCalls;
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    // Fog puffs
    UpdateFogVB(view, tMs);
    if (s_fogVB && s_fogTris > 0) {
        D3DXMATRIX fogWorld; D3DXMatrixIdentity(&fogWorld);
        g_pDevice->SetTransform(D3DTS_WORLD, &fogWorld);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pDevice->SetTexture(0, NULL);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
        DisableFrom(1);
        g_pDevice->SetVertexShader(GVFVF);
        g_pDevice->SetStreamSource(0, s_fogVB, sizeof(GVtx));
        g_pDevice->DrawPrimitive(D3DPT_TRIANGLELIST, 0, s_fogTris); ++s_drawCalls;
    }

    // Restore
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    DisableFrom(0);
    D3DXMATRIX id; D3DXMatrixIdentity(&id);
    g_pDevice->SetTransform(D3DTS_WORLD, &id);
    g_pDevice->SetTransform(D3DTS_VIEW, &id);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &id);
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------
void CrystalScene_Init()
{
    s_active = true;
    s_startTicks = GetTickCount();
    s_lastTick = 0;
    s_initOK = false;
    s_initErr[0] = 0;

    BuildLUT();

    if (!g_pDevice) { SetInitError("Init: no device"); return; }

    if (!BuildGeometry()) return;

    BuildCave();
    BuildFogDisc();

    GenTextures();

    // Vertex shader optional in this fixed-function render path.
    CreateVS();

    s_initOK = true;
}

void CrystalScene_Shutdown()
{
    s_active = false;
    DeleteVS();
    SAFE_RELEASE(s_crystalVB);
    SAFE_RELEASE(s_glowVB);
    SAFE_RELEASE(s_fogVB);
    SAFE_RELEASE(s_caveVB);
    SAFE_RELEASE(s_caveIB);
    SAFE_RELEASE(s_caveTex);
    SAFE_RELEASE(s_normalMap);
    SAFE_RELEASE(s_cubeMap);
}

bool CrystalScene_IsFinished()
{
    // Don�t let init failure trap scene forever
    return (GetTickCount() - s_startTicks) >= SCENE_MS;
}

void CrystalScene_Render(float)
{
    if (!g_pDevice) return;

    // Frame timing
    DWORD fNow = GetTickCount();
    if (s_lastTick != 0) {
        float dt = (float)(fNow - s_lastTick);
        if (dt > 0.f) { s_frameMs = s_frameMs * 0.85f + dt * 0.15f; s_fps = s_fps * 0.85f + (1000.f / dt) * 0.15f; }
    }
    else { s_frameMs = 16.f; s_fps = 60.f; }
    s_lastTick = fNow;

    s_drawCalls = 0;

    DWORD tMs = GetTickCount() - s_startTicks;
    float t = (float)tMs * 0.001f;
    float tFull = (float)SCENE_MS * 0.001f;

    float alpha = 1.f;
    if (t < 1.5f) alpha = t / 1.5f;
    else if (t > tFull - 1.5f) alpha = (tFull - t) / 1.5f;
    alpha = Clamp01(alpha);

    float ry = t * (TAU / 30.f);
    float rx = 0.15f * LSin(t * 0.4f);
    D3DXMATRIX mRX, mRY, world;
    D3DXMatrixRotationX(&mRX, rx);
    D3DXMatrixRotationY(&mRY, ry);
    world = mRX * mRY;

    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_XRGB(0, 0, 0), 1.f, 0);

    if (s_initOK) {
        RenderCluster(tMs, alpha, world);
    }

    // Cave drawn 3x -- small tMs offsets give subtle blur/glow without heavy smear
    DrawCave(tMs, alpha);
    DrawCave(tMs + 40, alpha * 0.4f);
    DrawCave(tMs + 80, alpha * 0.2f);

    // Overlay
    {
        g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
        g_pDevice->SetTexture(0, NULL);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);

        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

        char buf[128];
        const float sx = 8.f, sc = 2.f;
        DWORD col = D3DCOLOR_XRGB(0, 255, 80);
        int fps = Ftoi(s_fps), fms = Ftoi(s_frameMs);

        // --- Frame time bar at top ---
        // Scale: 0ms=green, 16ms=yellow, 33ms+=red (33ms = 30fps target)
        // Clamp fill to 33ms range so bar is meaningful across 60-30fps
        {
            const float BX = sx, BY = 8.f, BH = 14.f, BW = 420.f;
            const float TARGET_MS = 50.0f;   // 20fps = full bar, 33ms = 66% yellow
            float fill = s_frameMs / TARGET_MS; if (fill > 1.f) fill = 1.f;
            // green at low load, yellow at mid, red at high
            int bR = fill < 0.5f ? Ftoi(fill * 2.f * 255.f) : 255;
            int bG = fill < 0.5f ? 255 : Ftoi((1.f - (fill - 0.5f) * 2.f) * 255.f);
            if (bG < 0) bG = 0;
            DWORD bCol = D3DCOLOR_XRGB(bR, bG, 0);
            struct FBV { float x, y, z, rhw; DWORD c; };
            // Track
            FBV tr[4] = { {BX,BY,0,1,0xFF1A1A1A},{BX + BW,BY,0,1,0xFF1A1A1A},{BX,BY + BH,0,1,0xFF1A1A1A},{BX + BW,BY + BH,0,1,0xFF1A1A1A} };
            g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tr, sizeof(FBV));
            // Fill
            float fw = fill * BW;
            if (fw > 1.f) {
                FBV br[4] = { {BX,BY,0,1,bCol},{BX + fw,BY,0,1,bCol},{BX,BY + BH,0,1,bCol},{BX + fw,BY + BH,0,1,bCol} };
                g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, br, sizeof(FBV));
            }
            // Tick mark at 16ms (60fps)
            float tick = (16.667f / TARGET_MS) * BW;
            FBV tk[4] = { {BX + tick - 1,BY,0,1,0xFFFFFFFF},{BX + tick + 1,BY,0,1,0xFFFFFFFF},{BX + tick - 1,BY + BH,0,1,0xFFFFFFFF},{BX + tick + 1,BY + BH,0,1,0xFFFFFFFF} };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, tk, sizeof(FBV));
            g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
            g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
            // Label: ms, fps, load tag -- all on one line under bar
            const char* tag = fps >= 45 ? "SOLID" : fps >= 35 ? "OK" : fps >= 25 ? "LIGHT" : "HEAVY";
            wsprintfA(buf, "%d ms  %d fps  %s", fms, fps, tag);
            DrawText(BX, BY + BH + 4.f, buf, sc, bCol);
        }

        // --- Stats below bar ---
        float sy = 50.f;
        int totalTris = s_crystalTris + s_cavePrims * 3 + s_fogTris;
        int trisK = totalTris / 1000, trisR = totalTris - trisK * 1000;
        wsprintfA(buf, "TRIS: %d.%03d  DRAWS: %d", trisK, trisR, s_drawCalls);
        DrawText(sx, sy, buf, sc, col); sy += 18.f;
        wsprintfA(buf, "VB:%s NM:%s CB:%s CVB:%s CIB:%s %s INIT:%s",
            s_crystalVB ? "OK" : "NO", s_normalMap ? "OK" : "NO",
            s_cubeMap ? "OK" : "NO",
            s_caveVB ? "OK" : "NO", s_caveIB ? "OK" : "NO",
            s_rockDDSOK ? "RK" : "FB", s_initOK ? "OK" : "FAIL");
        DrawText(sx, sy, buf, sc, col); sy += 18.f;
        if (!s_initOK && s_initErr[0])
            DrawText(sx, sy, s_initErr, sc, D3DCOLOR_XRGB(255, 80, 80));
    }
}