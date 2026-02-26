// CubeEnvScene.cpp - Real-time environment mapped reflective cube (DX8 / NV2A)
//
// Technique:
//   - 256x256 D3DCUBEMAP render target, one face updated per frame (staggered)
//   - Dot-product face cull: exactly 3 back faces skipped every frame
//   - Rotating cube with per-vertex tangent-space DOT3 light encoding
//     (ChromeScene proven technique - world-space light transform per face)
//   - Animated UV ripple matrix on normal map stage (liquid chrome surface feel)
//   - 4 render passes per frame:
//       Pass 1: DOT3 bump x diffuse           (opaque, writes Z)
//       Pass 2: Cube map reflection x mask     (additive, no Z write)
//       Pass 3: Specular highlight             (additive, green-white TFACTOR)
//       Pass 4: Rim light DOT3                 (additive, teal-blue edge catch)
//
// Cube face environments (each face a unique scene rendered into 256x256 RT):
//   Face 0 (+Z): Deep space nebula  - Xbox green, textured clouds, starfield
//   Face 1 (-Z): Plasma colour wave - shifting RGB bands + horizontal sweep
//   Face 2 (+X): Matrix digital rain - falling green block glyphs
//   Face 3 (-X): Lava / fire        - rising heat columns, orange-red blobs
//   Face 4 (+Y): Electric energy grid - blue-white lines, crackle nodes
//   Face 5 (-Y): Void               - black void, slow purple/blue particles
//
// Background:
//   - Dark Xbox-green gradient base
//   - 28 textured nebula clouds (green/teal/blue/purple) via cloud_256.dds
//     using GalaxyScene sprite technique (SRCALPHA x ONE, MODULATE colour+alpha)
//   - 220 normalised-position stars scaled to viewport at draw time
//   - Green-teal lens glow pulse
//
// Assets:
//   D:\tex\cloud_256.dds   (A8R8G8B8 swizzled) - nebula cloud sprite
//   D:\tex\chrome.dds      (DXT5)               - tangent-space normal map
//   D:\tex\chrome_diff.dds (DXT1)               - diffuse albedo tint
//   D:\tex\chrome_bmp.dds  (DXT1)               - bump/specular mask

#include "CubeEnvScene.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;
static const int   CUBE_FACE_SZ = 256;
static const DWORD SCENE_DURATION_MS = 25000;

// ------------------------------------------------------------
// RXDK-safe float->int  (inline asm, no __ftol2_sse)
// ------------------------------------------------------------
static __forceinline int Ftoi(float f)
{
    int i;
    __asm
    {
        fld  f
        fistp i
    }
    return i;
}

static __forceinline BYTE ClampByte(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (BYTE)v;
}

static __forceinline DWORD MakeARGB(BYTE a, BYTE r, BYTE g, BYTE b)
{
    return D3DCOLOR_ARGB(a, r, g, b);
}

// ------------------------------------------------------------
// Trig LUT
// ------------------------------------------------------------
static const int LUT_N = 1024;
static float s_sin[LUT_N];
static float s_cos[LUT_N];
static bool  s_lutReady = false;

static void BuildLUT()
{
    if (s_lutReady) return;
    for (int i = 0; i < LUT_N; ++i)
    {
        float a = (float)i * (2.0f * 3.14159265358979323846f) / (float)LUT_N;
        s_sin[i] = sinf(a);
        s_cos[i] = cosf(a);
    }
    s_lutReady = true;
}

// ------------------------------------------------------------
// Scene state
// ------------------------------------------------------------
static bool  s_active = false;
static DWORD s_startTicks = 0;

// ------------------------------------------------------------
// DDS A8R8G8B8 swizzled loader (ported from GalaxyScene)
// ------------------------------------------------------------
#pragma pack(push,1)
struct CEDds_PixFmt { DWORD size, flags, fourCC, rgbBitCount, rMask, gMask, bMask, aMask; };
struct CEDds_Header {
    DWORD size, flags, height, width, pitchOrLinearSize, depth, mipMapCount, res[11];
    CEDds_PixFmt ddspf; DWORD caps, caps2, caps3, caps4, res2;
};
#pragma pack(pop)

static LPDIRECT3DTEXTURE8 LoadCloudDDS(const char* path)
{
    if (!g_pDevice || !path) return NULL;
    HANDLE hf = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ,
        NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return NULL;

    DWORD rd = 0, magic = 0;
    if (!ReadFile(hf, &magic, 4, &rd, NULL) || magic != 0x20534444u)
    {
        CloseHandle(hf); return NULL;
    }

    CEDds_Header hdr;
    if (!ReadFile(hf, &hdr, sizeof(hdr), &rd, NULL) || rd != sizeof(hdr))
    {
        CloseHandle(hf); return NULL;
    }

    if (hdr.size != 124 || hdr.ddspf.size != 32 ||
        hdr.ddspf.rgbBitCount != 32 ||
        !(hdr.ddspf.flags & 0x41u) ||           // RGB + ALPHA flags
        hdr.ddspf.rMask != 0x00FF0000u ||
        hdr.ddspf.gMask != 0x0000FF00u ||
        hdr.ddspf.bMask != 0x000000FFu ||
        hdr.ddspf.aMask != 0xFF000000u)
    {
        CloseHandle(hf); return NULL;
    }

    int w = (int)hdr.width, h = (int)hdr.height;
    DWORD bytes = (DWORD)(w * h * 4);
    BYTE* px = (BYTE*)malloc(bytes);
    if (!px) { CloseHandle(hf); return NULL; }

    if (!ReadFile(hf, px, bytes, &rd, NULL) || rd != bytes)
    {
        free(px); CloseHandle(hf); return NULL;
    }
    CloseHandle(hf);

    LPDIRECT3DTEXTURE8 tex = NULL;
    if (FAILED(g_pDevice->CreateTexture((UINT)w, (UINT)h, 1, 0,
        D3DFMT_A8R8G8B8, 0, &tex))) {
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
    return tex;
}

// ------------------------------------------------------------
// Cloud texture + nebula cloud data
// ------------------------------------------------------------
static LPDIRECT3DTEXTURE8 s_texCloud = NULL;

struct NebCloud
{
    float nx, ny;       // normalised 0..1 position
    float size;         // radius in pixels at 640x480
    DWORD baseColor;    // ARGB tint — green / blue / purple
    int   phaseIdx;     // LUT offset for pulse
};

static const int CLOUD_N = 28;
static NebCloud  s_clouds[CLOUD_N];
static bool      s_cloudsBuilt = false;

static void BuildClouds()
{
    if (s_cloudsBuilt) return;

    // Preset clouds — green / teal / blue / purple nebulae
    // Spread across the screen, varied sizes, varied colours
    struct CloudSeed { float nx, ny, sz; BYTE a, r, g, b; int ph; };
    static const CloudSeed seeds[CLOUD_N] =
    {
        // Green nebulae
        { 0.10f, 0.15f,  90.f,  28,  10, 200,  60,   0 },
        { 0.55f, 0.08f,  75.f,  22,  15, 180,  80,  80 },
        { 0.82f, 0.22f,  85.f,  26,   8, 220,  50, 160 },
        { 0.25f, 0.55f,  70.f,  20,  12, 160,  90, 240 },
        { 0.68f, 0.60f,  95.f,  30,   6, 200,  70, 320 },
        { 0.40f, 0.80f,  80.f,  24,  10, 190,  55, 400 },
        { 0.90f, 0.75f,  65.f,  18,  14, 175,  85, 480 },

        // Teal / cyan nebulae
        { 0.15f, 0.40f,  85.f,  24,  10, 140, 180,  40 },
        { 0.72f, 0.35f,  75.f,  20,   8, 120, 200,  120 },
        { 0.35f, 0.22f,  90.f,  28,  12, 100, 220, 200 },
        { 0.60f, 0.85f,  70.f,  22,   6, 130, 190, 280 },
        { 0.88f, 0.50f,  80.f,  26,  10, 110, 210, 360 },
        { 0.05f, 0.70f,  65.f,  18,  14, 150, 170, 440 },
        { 0.48f, 0.45f,  95.f,  30,   8, 120, 200, 520 },

        // Blue nebulae
        { 0.20f, 0.90f,  80.f,  22,  20,  60, 200,  60 },
        { 0.75f, 0.10f,  75.f,  18,  25,  40, 180, 140 },
        { 0.42f, 0.65f,  90.f,  26,  15,  50, 200, 220 },
        { 0.92f, 0.88f,  70.f,  20,  20,  70, 190, 300 },
        { 0.08f, 0.35f,  85.f,  24,  10,  45, 210, 380 },
        { 0.58f, 0.30f,  65.f,  16,  18,  55, 195, 460 },
        { 0.30f, 0.12f,  80.f,  22,  22,  60, 185, 540 },

        // Purple / violet nebulae
        { 0.65f, 0.72f,  90.f,  24, 140,  20, 200,  20 },
        { 0.18f, 0.78f,  75.f,  20, 160,  10, 180, 100 },
        { 0.80f, 0.42f,  85.f,  28, 120,  25, 210, 180 },
        { 0.38f, 0.92f,  70.f,  18, 150,  15, 190, 260 },
        { 0.52f, 0.18f,  80.f,  22, 130,  20, 200, 340 },
        { 0.95f, 0.62f,  65.f,  16, 170,   8, 175, 420 },
        { 0.12f, 0.55f,  90.f,  26, 110,  30, 220, 500 },
    };

    for (int i = 0; i < CLOUD_N; ++i)
    {
        s_clouds[i].nx = seeds[i].nx;
        s_clouds[i].ny = seeds[i].ny;
        s_clouds[i].size = seeds[i].sz;
        s_clouds[i].baseColor = D3DCOLOR_ARGB(seeds[i].a, seeds[i].r,
            seeds[i].g, seeds[i].b);
        s_clouds[i].phaseIdx = seeds[i].ph & 1023;
    }
    s_cloudsBuilt = true;
}

// Pulse a cloud colour (same technique as GalaxyScene TwinkleColor)
static __forceinline DWORD PulseCloud(DWORD baseARGB, int add)
{
    BYTE a = (BYTE)(baseARGB >> 24);
    BYTE r = (BYTE)(baseARGB >> 16);
    BYTE g = (BYTE)(baseARGB >> 8);
    BYTE b = (BYTE)(baseARGB);
    int ia = (int)a + add;
    int ir = (int)r + (add >> 1);
    int ig = (int)g + (add >> 1);
    int ib = (int)b + (add >> 1);
    if (ia > 255) ia = 255;  if (ir > 255) ir = 255;
    if (ig > 255) ig = 255;  if (ib > 255) ib = 255;
    return D3DCOLOR_ARGB((BYTE)ia, (BYTE)ir, (BYTE)ig, (BYTE)ib);
}

// Draw textured nebula clouds scaled to any viewport size
// Uses GalaxyScene SetupSpriteStates technique:
//   SRC_ALPHA x ONE additive, MODULATE tex x diffuse on both colour+alpha
struct CloudVtx { float x, y, z, rhw; DWORD c; float u, v; };
#define FVF_CLOUD (D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static void DrawNebulaClouds(DWORD tMs, float vpW, float vpH)
{
    if (!s_texCloud) return;

    int base = (int)((tMs / 35u) & 1023u);

    // Texture stage — modulate tex alpha and colour by diffuse (GalaxyScene pattern)
    g_pDevice->SetTexture(0, s_texCloud);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_NONE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    g_pDevice->SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetVertexShader(FVF_CLOUD);

    float scaleX = vpW / SCREEN_W;
    float scaleY = vpH / SCREEN_H;

    for (int i = 0; i < CLOUD_N; ++i)
    {
        const NebCloud& nc = s_clouds[i];
        int ph = (base + nc.phaseIdx) & 1023;
        int add = (s_sin[ph] > 0.0f) ? Ftoi(s_sin[ph] * 18.0f) : 0;
        DWORD col = PulseCloud(nc.baseColor, add);

        float cx = nc.nx * vpW;
        float cy = nc.ny * vpH;
        float size = nc.size * scaleX;  // scale radius with viewport

        CloudVtx q[6] =
        {
            { cx - size, cy - size, 0,1, col, 0,0 },
            { cx + size, cy - size, 0,1, col, 1,0 },
            { cx + size, cy + size, 0,1, col, 1,1 },
            { cx - size, cy - size, 0,1, col, 0,0 },
            { cx + size, cy + size, 0,1, col, 1,1 },
            { cx - size, cy + size, 0,1, col, 0,1 },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 2, q, sizeof(CloudVtx));
    }

    // Clean up tex stage
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
}

// ------------------------------------------------------------
// Chrome textures (reused from ChromeScene — stepped load)
// ------------------------------------------------------------
static LPDIRECT3DTEXTURE8 s_texNormal = NULL;
static LPDIRECT3DTEXTURE8 s_texDiff = NULL;
static LPDIRECT3DTEXTURE8 s_texMask = NULL;
static int                s_loadStep = 0;

static void StepLoad()
{
    switch (s_loadStep)
    {
    case 0:
        s_texCloud = LoadCloudDDS("D:\\tex\\cloud_256.dds");
        if (!s_texCloud) s_texCloud = LoadCloudDDS("tex\\cloud_256.dds");
        s_loadStep = 1;
        break;
    case 1:
        if (FAILED(D3DXCreateTextureFromFileA(g_pDevice,
            "D:\\tex\\chrome.dds", &s_texNormal)))
            D3DXCreateTextureFromFileA(g_pDevice,
                "tex\\chrome.dds", &s_texNormal);
        s_loadStep = 2;
        break;
    case 2:
        if (FAILED(D3DXCreateTextureFromFileA(g_pDevice,
            "D:\\tex\\chrome_diff.dds", &s_texDiff)))
            D3DXCreateTextureFromFileA(g_pDevice,
                "tex\\chrome_diff.dds", &s_texDiff);
        s_loadStep = 3;
        break;
    case 3:
        if (FAILED(D3DXCreateTextureFromFileA(g_pDevice,
            "D:\\tex\\chrome_bmp.dds", &s_texMask)))
            D3DXCreateTextureFromFileA(g_pDevice,
                "tex\\chrome_bmp.dds", &s_texMask);
        s_loadStep = 4;
        break;
    case 4:
        s_active = true;
        s_loadStep = 5;
        break;
    default:
        break;
    }
}

static void ReleaseTextures()
{
    if (s_texCloud) { s_texCloud->Release();  s_texCloud = NULL; }
    if (s_texNormal) { s_texNormal->Release(); s_texNormal = NULL; }
    if (s_texDiff) { s_texDiff->Release();   s_texDiff = NULL; }
    if (s_texMask) { s_texMask->Release();   s_texMask = NULL; }
}

// ------------------------------------------------------------
// Cube map render target
// ------------------------------------------------------------
static LPDIRECT3DCUBETEXTURE8 s_cubeMap = NULL;
static LPDIRECT3DSURFACE8     s_cubeDepth = NULL;
static int                    s_nextFace = 0;
static bool                   s_cubeReady = false;

static void CreateCubeRT()
{
    if (s_cubeMap) return;

    // Create cubemap and load each face from a pre-baked DDS file.
    // No render target, no SetRenderTarget - safe on real NV2A hardware.
    if (FAILED(g_pDevice->CreateCubeTexture(
        CUBE_FACE_SZ, 1, 0,
        D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &s_cubeMap)))
    {
        s_cubeMap = NULL;
        return;
    }

    static const char* facePaths[6] = {
        "D:\\tex\\cube_face0.dds",
        "D:\\tex\\cube_face1.dds",
        "D:\\tex\\cube_face2.dds",
        "D:\\tex\\cube_face3.dds",
        "D:\\tex\\cube_face4.dds",
        "D:\\tex\\cube_face5.dds",
    };

    for (int f = 0; f < 6; ++f)
    {
        LPDIRECT3DTEXTURE8 tmp = NULL;
        if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, facePaths[f], &tmp)))
            continue;

        LPDIRECT3DSURFACE8 src = NULL;
        LPDIRECT3DSURFACE8 dst = NULL;
        tmp->GetSurfaceLevel(0, &src);
        s_cubeMap->GetCubeMapSurface((D3DCUBEMAP_FACES)f, 0, &dst);
        D3DXLoadSurfaceFromSurface(dst, NULL, NULL, src, NULL, NULL, D3DX_DEFAULT, 0);
        if (src) src->Release();
        if (dst) dst->Release();
        if (tmp) tmp->Release();
    }

    s_cubeDepth = NULL;
    s_cubeReady = true;
}

static void ReleaseCubeRT()
{
    if (s_cubeDepth) { s_cubeDepth->Release(); s_cubeDepth = NULL; }
    if (s_cubeMap) { s_cubeMap->Release();   s_cubeMap = NULL; }
    s_cubeReady = false;
}

// ------------------------------------------------------------
// Cube geometry — 6 faces, per-face flat normals + tangent frame
// ------------------------------------------------------------
struct CubeVtx
{
    float x, y, z;
    float nx, ny, nz;
    DWORD color;    // tangent-space light, updated per-frame
    float u, v;
};
#define FVF_CUBE (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1)

static LPDIRECT3DVERTEXBUFFER8 s_cubeVB = NULL;
static LPDIRECT3DINDEXBUFFER8  s_cubeIB = NULL;
static const int   CUBE_VERTS = 24;
static const int   CUBE_TRIS = 12;
static const float CS = 1.5f;

// Per-face: 4 positions + normal + tangent + computed binormal
struct FaceDef
{
    float px[4], py[4], pz[4];
    float nx, ny, nz;
    float tx, ty, tz;
    float bx, by, bz;   // computed in BuildFaceDefs
};
static FaceDef s_faces[6];

static void BuildFaceDefs()
{
    // +Z front
    s_faces[0].px[0] = -CS; s_faces[0].py[0] = -CS; s_faces[0].pz[0] = +CS;
    s_faces[0].px[1] = +CS; s_faces[0].py[1] = -CS; s_faces[0].pz[1] = +CS;
    s_faces[0].px[2] = -CS; s_faces[0].py[2] = +CS; s_faces[0].pz[2] = +CS;
    s_faces[0].px[3] = +CS; s_faces[0].py[3] = +CS; s_faces[0].pz[3] = +CS;
    s_faces[0].nx = 0; s_faces[0].ny = 0; s_faces[0].nz = 1;
    s_faces[0].tx = 1; s_faces[0].ty = 0; s_faces[0].tz = 0;

    // -Z back
    s_faces[1].px[0] = +CS; s_faces[1].py[0] = -CS; s_faces[1].pz[0] = -CS;
    s_faces[1].px[1] = -CS; s_faces[1].py[1] = -CS; s_faces[1].pz[1] = -CS;
    s_faces[1].px[2] = +CS; s_faces[1].py[2] = +CS; s_faces[1].pz[2] = -CS;
    s_faces[1].px[3] = -CS; s_faces[1].py[3] = +CS; s_faces[1].pz[3] = -CS;
    s_faces[1].nx = 0; s_faces[1].ny = 0; s_faces[1].nz = -1;
    s_faces[1].tx = -1; s_faces[1].ty = 0; s_faces[1].tz = 0;

    // +X right
    s_faces[2].px[0] = +CS; s_faces[2].py[0] = -CS; s_faces[2].pz[0] = +CS;
    s_faces[2].px[1] = +CS; s_faces[2].py[1] = -CS; s_faces[2].pz[1] = -CS;
    s_faces[2].px[2] = +CS; s_faces[2].py[2] = +CS; s_faces[2].pz[2] = +CS;
    s_faces[2].px[3] = +CS; s_faces[2].py[3] = +CS; s_faces[2].pz[3] = -CS;
    s_faces[2].nx = 1; s_faces[2].ny = 0; s_faces[2].nz = 0;
    s_faces[2].tx = 0; s_faces[2].ty = 0; s_faces[2].tz = -1;

    // -X left
    s_faces[3].px[0] = -CS; s_faces[3].py[0] = -CS; s_faces[3].pz[0] = -CS;
    s_faces[3].px[1] = -CS; s_faces[3].py[1] = -CS; s_faces[3].pz[1] = +CS;
    s_faces[3].px[2] = -CS; s_faces[3].py[2] = +CS; s_faces[3].pz[2] = -CS;
    s_faces[3].px[3] = -CS; s_faces[3].py[3] = +CS; s_faces[3].pz[3] = +CS;
    s_faces[3].nx = -1; s_faces[3].ny = 0; s_faces[3].nz = 0;
    s_faces[3].tx = 0; s_faces[3].ty = 0; s_faces[3].tz = +1;

    // +Y top
    s_faces[4].px[0] = -CS; s_faces[4].py[0] = +CS; s_faces[4].pz[0] = +CS;
    s_faces[4].px[1] = +CS; s_faces[4].py[1] = +CS; s_faces[4].pz[1] = +CS;
    s_faces[4].px[2] = -CS; s_faces[4].py[2] = +CS; s_faces[4].pz[2] = -CS;
    s_faces[4].px[3] = +CS; s_faces[4].py[3] = +CS; s_faces[4].pz[3] = -CS;
    s_faces[4].nx = 0; s_faces[4].ny = 1; s_faces[4].nz = 0;
    s_faces[4].tx = 1; s_faces[4].ty = 0; s_faces[4].tz = 0;

    // -Y bottom
    s_faces[5].px[0] = -CS; s_faces[5].py[0] = -CS; s_faces[5].pz[0] = -CS;
    s_faces[5].px[1] = +CS; s_faces[5].py[1] = -CS; s_faces[5].pz[1] = -CS;
    s_faces[5].px[2] = -CS; s_faces[5].py[2] = -CS; s_faces[5].pz[2] = +CS;
    s_faces[5].px[3] = +CS; s_faces[5].py[3] = -CS; s_faces[5].pz[3] = +CS;
    s_faces[5].nx = 0; s_faces[5].ny = -1; s_faces[5].nz = 0;
    s_faces[5].tx = 1; s_faces[5].ty = 0; s_faces[5].tz = 0;

    // Compute binormal = normal x tangent
    for (int f = 0; f < 6; ++f)
    {
        FaceDef& fd = s_faces[f];
        fd.bx = fd.ny * fd.tz - fd.nz * fd.ty;
        fd.by = fd.nz * fd.tx - fd.nx * fd.tz;
        fd.bz = fd.nx * fd.ty - fd.ny * fd.tx;
    }
}

static const float s_faceUV[4][2] = { {0,1},{1,1},{0,0},{1,0} };

static void BuildCubeGeometry()
{
    if (s_cubeVB) return;
    BuildFaceDefs();

    g_pDevice->CreateVertexBuffer(
        CUBE_VERTS * sizeof(CubeVtx), 0, FVF_CUBE,
        D3DPOOL_MANAGED, &s_cubeVB);
    g_pDevice->CreateIndexBuffer(
        CUBE_TRIS * 3 * sizeof(WORD), 0, D3DFMT_INDEX16,
        D3DPOOL_MANAGED, &s_cubeIB);

    CubeVtx* vb = NULL;
    s_cubeVB->Lock(0, 0, (BYTE**)&vb, 0);
    for (int f = 0; f < 6; ++f)
    {
        const FaceDef& fd = s_faces[f];
        for (int i = 0; i < 4; ++i)
        {
            CubeVtx& v = vb[f * 4 + i];
            v.x = fd.px[i]; v.y = fd.py[i]; v.z = fd.pz[i];
            v.nx = fd.nx;   v.ny = fd.ny;   v.nz = fd.nz;
            v.color = 0xFFFFFFFF;
            v.u = s_faceUV[i][0];
            v.v = s_faceUV[i][1];
        }
    }
    s_cubeVB->Unlock();

    WORD* ib = NULL;
    s_cubeIB->Lock(0, 0, (BYTE**)&ib, 0);
    for (int f = 0; f < 6; ++f)
    {
        WORD b = (WORD)(f * 4);
        ib[f * 6 + 0] = b + 0; ib[f * 6 + 1] = b + 2; ib[f * 6 + 2] = b + 3;
        ib[f * 6 + 3] = b + 0; ib[f * 6 + 4] = b + 3; ib[f * 6 + 5] = b + 1;
    }
    s_cubeIB->Unlock();
}

static void ReleaseCubeGeometry()
{
    if (s_cubeVB) { s_cubeVB->Release(); s_cubeVB = NULL; }
    if (s_cubeIB) { s_cubeIB->Release(); s_cubeIB = NULL; }
}

// ------------------------------------------------------------
// Per-vertex tangent-space DOT3 light encoding
// Mirrors ChromeScene exactly:
//   transform light to object space via world matrix rows,
//   project onto per-face tangent/binormal/normal,
//   encode (v*0.5+0.5)*255 into vertex ARGB diffuse.
//   Two lights: warm green-white primary + cool blue fill.
// ------------------------------------------------------------
static void UpdateDOT3(const D3DXMATRIX& world, DWORD tMs)
{
    if (!s_cubeVB) return;

    // Primary light position — warm, orbits horizontally
    int phA = (int)((tMs / 8u) & 1023u);
    int phAe = (int)((tMs / 13u) & 1023u);
    float plx = s_cos[phA] * 2.2f;
    float ply = 0.8f + s_sin[phAe] * 0.4f;
    float plz = s_sin[phA] * 2.2f;

    // Fill light position — cool blue, opposite hemisphere
    int phB = (int)((tMs / 11u) & 1023u);
    float flx = s_cos[(phB + 512) & 1023] * 2.0f;
    float fly = -0.5f - s_sin[(phB + 256) & 1023] * 0.3f;
    float flz = s_sin[(phB + 512) & 1023] * 2.0f;

    // Transform to object space via transposed world rotation rows
    // (same as ChromeScene verts[vi] approach but precomputed per-face)
    float pox = plx * world._11 + ply * world._12 + plz * world._13;
    float poy = plx * world._21 + ply * world._22 + plz * world._23;
    float poz = plx * world._31 + ply * world._32 + plz * world._33;

    float fox = flx * world._11 + fly * world._12 + flz * world._13;
    float foy = flx * world._21 + fly * world._22 + flz * world._23;
    float foz = flx * world._31 + fly * world._32 + flz * world._33;

    float pl = sqrtf(pox * pox + poy * poy + poz * poz);
    if (pl < 0.0001f) pl = 1.0f;
    pox /= pl; poy /= pl; poz /= pl;

    float fl = sqrtf(fox * fox + foy * foy + foz * foz);
    if (fl < 0.0001f) fl = 1.0f;
    fox /= fl; foy /= fl; foz /= fl;

    CubeVtx* vb = NULL;
    s_cubeVB->Lock(0, 0, (BYTE**)&vb, 0);

    for (int f = 0; f < 6; ++f)
    {
        const FaceDef& fd = s_faces[f];

        // Primary in tangent space
        float pt = pox * fd.tx + poy * fd.ty + poz * fd.tz;
        float pb = pox * fd.bx + poy * fd.by + poz * fd.bz;
        float pn = pox * fd.nx + poy * fd.ny + poz * fd.nz;

        // Fill in tangent space, 25% weight
        const float FW = 0.25f;
        float ft = (fox * fd.tx + foy * fd.ty + foz * fd.tz) * FW;
        float fb = (fox * fd.bx + foy * fd.by + foz * fd.bz) * FW;
        float fn = (fox * fd.nx + foy * fd.ny + foz * fd.nz) * FW;

        // Combine primary 65% + fill — green tint
        const float S = 0.65f;
        float combT = pt * S + ft * 0.6f;
        float combB = pb * S + fb * 0.6f;
        float combN = pn * S + fn * 0.9f;

        BYTE cr = ClampByte(Ftoi((combT * 0.5f + 0.5f) * 255.0f));
        BYTE cg = ClampByte(Ftoi((combB * 0.5f + 0.5f) * 255.0f));
        BYTE cb = ClampByte(Ftoi((combN * 0.5f + 0.5f) * 255.0f));

        for (int i = 0; i < 4; ++i)
            vb[f * 4 + i].color = MakeARGB(255, cr, cg, cb);
    }

    s_cubeVB->Unlock();
}

// ------------------------------------------------------------
// State helpers
// ------------------------------------------------------------
static void SetLinear(int stage)
{
    g_pDevice->SetTextureStageState(stage, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(stage, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(stage, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
}

static void UndoState()
{
    D3DXMATRIX id;
    D3DXMatrixIdentity(&id);
    for (int si = 0; si < 4; ++si)
    {
        g_pDevice->SetTexture(si, NULL);
        g_pDevice->SetTextureStageState(si, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_TEXCOORDINDEX, si);
        g_pDevice->SetTextureStageState(si, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
        g_pDevice->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0 + si), &id);
    }
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
}

// ------------------------------------------------------------
// Face cull — dot product of world-space face normal vs camera
// ------------------------------------------------------------
static const float s_faceNX[6] = { 0, 0, 1,-1, 0, 0 };
static const float s_faceNY[6] = { 0, 0, 0, 0, 1,-1 };
static const float s_faceNZ[6] = { 1,-1, 0, 0, 0, 0 };

static bool IsFaceVisible(int face, const D3DXMATRIX& world,
    float camX, float camY, float camZ)
{
    float fnx = s_faceNX[face];
    float fny = s_faceNY[face];
    float fnz = s_faceNZ[face];

    // Rotate face normal to world space
    float wnx = fnx * world._11 + fny * world._21 + fnz * world._31;
    float wny = fnx * world._12 + fny * world._22 + fnz * world._32;
    float wnz = fnx * world._13 + fny * world._23 + fnz * world._33;

    return (wnx * camX + wny * camY + wnz * camZ) > 0.0f;
}

// ------------------------------------------------------------
// 2D drawing
// ------------------------------------------------------------
struct V2D { float x, y, z, rhw; DWORD color; };
#define FVF_V2D (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void Begin2D(bool additive)
{
    g_pDevice->SetVertexShader(FVF_V2D);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetTexture(1, NULL);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    if (additive)
    {
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }
    else
    {
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
}

// ------------------------------------------------------------
// Starfield — positions stored normalised 0..1, scaled at draw time
// ------------------------------------------------------------
static const int ENV_STAR_N = 220;
struct EnvStar { float nx, ny; int phaseIdx; int bright; };
static EnvStar  s_envStars[ENV_STAR_N];
static bool     s_envStarsBuilt = false;
static unsigned s_rng = 0xBEEF1234u;

static unsigned RngU32()
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return s_rng;
}

static void BuildEnvStars()
{
    if (s_envStarsBuilt) return;
    s_rng ^= GetTickCount();
    for (int i = 0; i < ENV_STAR_N; ++i)
    {
        s_envStars[i].nx = (float)(RngU32() & 0xFFFFu) * (1.0f / 65535.0f);
        s_envStars[i].ny = (float)(RngU32() & 0xFFFFu) * (1.0f / 65535.0f);
        s_envStars[i].phaseIdx = (int)(RngU32() & 1023u);
        s_envStars[i].bright = 70 + (int)(RngU32() % 185u);
    }
    s_envStarsBuilt = true;
}

static void DrawStars(DWORD tMs, float vpW, float vpH)
{
    int base = (int)((tMs / 8u) & 1023u);
    Begin2D(true);

    for (int i = 0; i < ENV_STAR_N; ++i)
    {
        int ph = (base + s_envStars[i].phaseIdx) & 1023;
        int tw = (s_sin[ph] > 0.0f) ? Ftoi(s_sin[ph] * 64.0f) : 0;
        int br = s_envStars[i].bright + tw;
        BYTE b = ClampByte(br);
        DWORD c = MakeARGB(b, ClampByte(br - 20), ClampByte(br - 10), b);

        float sx = s_envStars[i].nx * vpW;
        float sy = s_envStars[i].ny * vpH;

        V2D q[4] =
        {
            { sx,      sy,      0.0f, 1.0f, c },
            { sx + 1.5f, sy,      0.0f, 1.0f, c },
            { sx,      sy + 1.5f, 0.0f, 1.0f, c },
            { sx + 1.5f, sy + 1.5f, 0.0f, 1.0f, c },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }
}

// ------------------------------------------------------------
// Xbox-green nebula gradient
// ------------------------------------------------------------
static void DrawNebula(DWORD tMs, float vpW, float vpH)
{
    int ph0 = (int)((tMs / 14u) & 1023u);
    int ph1 = (int)((tMs / 17u) & 1023u);
    int ph2 = (int)((tMs / 11u) & 1023u);

    BYTE topR = ClampByte(4 + Ftoi(3.0f * s_sin[ph0]));
    BYTE topG = ClampByte(14 + Ftoi(8.0f * s_sin[(ph0 + 150) & 1023]));
    BYTE topB = ClampByte(20 + Ftoi(10.0f * s_sin[ph1]));

    BYTE botR = ClampByte(6 + Ftoi(4.0f * s_sin[ph2]));
    BYTE botG = ClampByte(28 + Ftoi(14.0f * s_sin[(ph2 + 200) & 1023]));
    BYTE botB = ClampByte(18 + Ftoi(8.0f * s_sin[(ph1 + 300) & 1023]));

    DWORD cTop = MakeARGB(255, topR, topG, topB);
    DWORD cBot = MakeARGB(255, botR, botG, botB);

    Begin2D(false);

    V2D q[4] =
    {
        { 0.0f, 0.0f, 0.0f, 1.0f, cTop },
        {  vpW, 0.0f, 0.0f, 1.0f, cTop },
        { 0.0f,  vpH, 0.0f, 1.0f, cBot },
        {  vpW,  vpH, 0.0f, 1.0f, cBot },
    };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
}

// ------------------------------------------------------------
// Coloured nebula blobs — Xbox green accent
// ------------------------------------------------------------
static void DrawBlobs(DWORD tMs, float vpW, float vpH)
{
    struct BlobDef { float nx, ny; int r8; int cr, cg, cb; int phOff; };
    static const BlobDef blobs[] =
    {
        { 0.20f, 0.25f, 55, 20, 180,  80,   0 },
        { 0.75f, 0.30f, 45, 10, 120, 180, 171 },
        { 0.50f, 0.70f, 60, 30, 160,  60, 342 },
        { 0.80f, 0.75f, 40, 10,  80, 160, 512 },
        { 0.30f, 0.80f, 50, 20, 200, 100, 683 },
    };
    static const int BLOB_N = 5;
    static const int SEG = 16;

    int base = (int)((tMs / 20u) & 1023u);
    Begin2D(true);

    for (int b = 0; b < BLOB_N; ++b)
    {
        const BlobDef& bd = blobs[b];
        int ph = (base + bd.phOff) & 1023;
        int dph = (base * 2 + bd.phOff * 3) & 1023;

        float cx = bd.nx * vpW + s_sin[dph] * vpW * 0.06f;
        float cy = bd.ny * vpH + s_cos[(dph + 256) & 1023] * vpH * 0.05f;
        float r = (float)bd.r8 * (vpW / 256.0f)
            + s_sin[(ph + 128) & 1023] * vpW * 0.03f;

        int a = 22 + Ftoi(s_sin[ph] * 14.0f);
        DWORD col = MakeARGB(ClampByte(a), ClampByte(bd.cr), ClampByte(bd.cg), ClampByte(bd.cb));
        DWORD edgeCol = MakeARGB(0, ClampByte(bd.cr), ClampByte(bd.cg), ClampByte(bd.cb));

        static V2D fan[SEG + 2];
        fan[0] = { cx, cy, 0.0f, 1.0f, col };
        for (int s = 0; s <= SEG; ++s)
        {
            int ai = (s * 1024 / SEG) & 1023;
            fan[s + 1] = { cx + s_cos[ai] * r, cy + s_sin[ai] * r, 0.0f, 1.0f, edgeCol };
        }
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEG, fan, sizeof(V2D));
    }
}

// ------------------------------------------------------------
// Lens glow — green-teal pulse centred on viewport
// ------------------------------------------------------------
static void DrawLensGlow(DWORD tMs, float vpW, float vpH)
{
    int phP = (int)((tMs / 6u) & 1023u);
    float pulse = 0.65f + s_sin[phP] * 0.25f;

    float cx = vpW * 0.5f;
    float cy = vpH * 0.5f;
    float R0 = vpW * 0.12f + vpW * 0.03f * pulse;
    float R1 = vpW * 0.30f + vpW * 0.05f * pulse;

    DWORD c0 = MakeARGB(0, 0, 0, 0);
    DWORD c1 = MakeARGB(55, 40, 180, 80);
    DWORD c2 = MakeARGB(40, 20, 120, 180);

    Begin2D(true);

    const int SEG = 40;
    for (int i = 0; i < SEG; ++i)
    {
        int ai0 = (i * 1024 / SEG) & 1023;
        int ai1 = ((i + 1) * 1024 / SEG) & 1023;

        V2D t0[3] =
        {
            { cx, cy, 0,1,c1 },
            { cx + s_cos[ai0] * R1, cy + s_sin[ai0] * R1, 0,1,c0 },
            { cx + s_cos[ai1] * R1, cy + s_sin[ai1] * R1, 0,1,c0 },
        };
        V2D t1[3] =
        {
            { cx, cy, 0,1,c2 },
            { cx + s_cos[ai0] * R0, cy + s_sin[ai0] * R0, 0,1,c0 },
            { cx + s_cos[ai1] * R0, cy + s_sin[ai1] * R0, 0,1,c0 },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t0, sizeof(V2D));
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, t1, sizeof(V2D));
    }
}

// ============================================================
// Per-face environment renderers
// Face 0 (+Z): Deep space nebula
// Face 1 (-Z): Plasma colour wave
// Face 2 (+X): Matrix digital rain
// Face 3 (-X): Lava / fire
// Face 4 (+Y): Electric energy grid
// Face 5 (-Y): Void particle drift
// ============================================================

// Face 0: Deep space nebula (existing, unchanged)


// ------------------------------------------------------------
// 24s pacing helpers (no refactor; just stronger faces)
// ------------------------------------------------------------
static __forceinline float Clamp01f(float x)
{
    if (x < 0.0f) return 0.0f;
    if (x > 1.0f) return 1.0f;
    return x;
}

static __forceinline DWORD LocalSceneMs(DWORD tMs)
{
    // Scene duration is ~25s; drive an internal arc via modulo so it loops cleanly.
    return (SCENE_DURATION_MS > 0) ? (tMs % SCENE_DURATION_MS) : tMs;
}

// Intensity curve across 25s: establish -> intensify -> peak -> release
static float SceneIntensity(DWORD tMs)
{
    DWORD lt = LocalSceneMs(tMs);

    if (lt < 8000u)
        return 0.70f;

    if (lt < 16000u)
    {
        float k = (float)(lt - 8000u) * (1.0f / 8000.0f);
        return 0.70f + 0.30f * k;
    }

    if (lt < 22000u)
    {
        float k = (float)(lt - 16000u) * (1.0f / 6000.0f);
        return 1.00f + 0.35f * k;
    }

    // release to ~1.10 by end
    {
        float k = (float)(lt - 22000u) * (1.0f / 3000.0f);
        float v = 1.35f - 0.25f * k;
        if (v < 1.10f) v = 1.10f;
        return v;
    }
}

// Short global pulse around ~20s (ties all faces together)
static float ScenePulse(DWORD tMs)
{
    DWORD lt = LocalSceneMs(tMs);
    int d = (int)lt - 20000;
    if (d < 0) d = -d;
    // 0..1 triangular pulse over 1000ms window
    float p = 1.0f - (float)d * (1.0f / 500.0f);
    return Clamp01f(p);
}

// Additive radial glow disc (cheap "hero" element that survives cubemap blur)
static void DrawRadialGlow(float W, float H, float cx, float cy, float r, DWORD centerCol, DWORD edgeCol)
{
    (void)H;
    Begin2D(true);

    const int SEG = 24;
    static V2D fan[SEG + 2];
    fan[0] = { cx, cy, 0,1,centerCol };
    for (int s = 0; s <= SEG; ++s)
    {
        int ai = (s * 1024 / SEG) & 1023;
        fan[s + 1] = { cx + s_cos[ai] * r, cy + s_sin[ai] * r, 0,1,edgeCol };
    }
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEG, fan, sizeof(V2D));
}

// Additive thick band (big readable sweep)
static void DrawBandH(float W, float y0, float y1, DWORD c)
{
    Begin2D(true);
    V2D q[4] = { {0,y0,0,1,c},{W,y0,0,1,c},{0,y1,0,1,c},{W,y1,0,1,c} };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
}


static void DrawFace0_Nebula(DWORD tMs)
{
    float W = (float)CUBE_FACE_SZ;
    float H = (float)CUBE_FACE_SZ;

    DrawNebula(tMs, W, H);
    DrawNebulaClouds(tMs, W, H);
    DrawStars(tMs, W, H);
    DrawLensGlow(tMs, W, H);

    // ---- Impact layers (large readable shapes + timed ignition) ----
    float inten = SceneIntensity(tMs);
    float pulse = ScenePulse(tMs);

    float cx = W * 0.52f;
    float cy = H * 0.42f;

    // slow core breathe
    int ph = (int)((tMs / 14u) & 1023u);
    float breathe = 0.75f + 0.25f * s_sin[ph];

    // supernova window ~18-21s
    DWORD lt = LocalSceneMs(tMs);
    float ign = 0.0f;
    if (lt > 17500u && lt < 21000u)
    {
        int d = (int)lt - 19250;
        if (d < 0) d = -d;
        ign = Clamp01f(1.0f - (float)d * (1.0f / 1750.0f));
    }

    float r0 = W * (0.14f + 0.05f * breathe);
    float r1 = W * (0.30f + 0.08f * breathe);

    // scale up during ignition + global pulse
    float boost = inten + 0.45f * ign + 0.25f * pulse;

    int a0 = 18 + Ftoi(95.0f * boost);
    int a1 = 0;

    // teal/white hot core with purple halo
    DWORD cCore = MakeARGB(ClampByte(a0), 130, 255, 220);
    DWORD cEdge = MakeARGB(ClampByte(a1), 0, 0, 0);
    DrawRadialGlow(W, H, cx, cy, r1, cCore, cEdge);

    int aH = 12 + Ftoi(70.0f * boost);
    DWORD cHalo = MakeARGB(ClampByte(aH), 160, 90, 255);
    DrawRadialGlow(W, H, cx, cy, r1 * 1.25f, cHalo, cEdge);

    // ignition flash ring (filled disc is fine at 256²; reads in reflection)
    if (ign > 0.05f)
    {
        int af = 10 + Ftoi(140.0f * ign);
        DWORD cFlash = MakeARGB(ClampByte(af), 255, 255, 255);
        DrawRadialGlow(W, H, cx, cy, r0 * (1.1f + 0.7f * ign), cFlash, cEdge);
    }
}


// ------------------------------------------------------------
// Face 1: Plasma colour wave
// Vertical bands of shifting hue across the face
// ------------------------------------------------------------

static void DrawFace1_Plasma(DWORD tMs)
{
    const float W = (float)CUBE_FACE_SZ;
    const float H = (float)CUBE_FACE_SZ;
    const int   COLS = 32;
    const float CW = W / (float)COLS;

    // Black background
    Begin2D(false);
    DWORD black = MakeARGB(255, 0, 0, 0);
    V2D bg[4] = { {0,0,0,1,black},{W,0,0,1,black},{0,H,0,1,black},{W,H,0,1,black} };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(V2D));

    Begin2D(true);

    float inten = SceneIntensity(tMs);
    float pulse = ScenePulse(tMs);

    int base = (int)((tMs / 5u) & 1023u);
    int base2 = (int)((tMs / 11u) & 1023u);

    // main vertical bands (kept)
    for (int col = 0; col < COLS; ++col)
    {
        int ph = (base + col * (1024 / COLS)) & 1023;
        int ph2 = (base2 + col * (1024 / COLS) + 200) & 1023;
        int ph3 = (base + col * (1024 / COLS) + 400) & 1023;

        BYTE r = ClampByte(70 + Ftoi(s_sin[ph] * 90.0f));
        BYTE g = ClampByte(160 + Ftoi(s_sin[ph2] * 80.0f));
        BYTE b = ClampByte(50 + Ftoi(s_sin[ph3] * 80.0f));

        int ai = 120 + Ftoi(110.0f * inten);
        BYTE a = ClampByte(ai);

        float x0 = (float)col * CW;
        float x1 = x0 + CW + 1.0f;

        DWORD c = MakeARGB(a, r, g, b);
        V2D q[4] =
        {
            { x0, 0.0f, 0,1,c },
            { x1, 0.0f, 0,1,c },
            { x0,    H, 0,1,c },
            { x1,    H, 0,1,c },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }

    // Horizontal wave sweep on top (kept, boosted)
    int wph = (int)((tMs / 3u) & 1023u);
    for (int row = 0; row < 16; ++row)
    {
        int rph = (wph + row * 64) & 1023;
        float y = (float)row * (H / 16.0f);
        float dy = H / 16.0f + 1.0f;

        BYTE ra = ClampByte(90 + Ftoi(s_sin[rph] * 110.0f));
        BYTE ga = ClampByte(200 + Ftoi(s_sin[(rph + 341) & 1023] * 55.0f));
        BYTE ba = ClampByte(70 + Ftoi(s_sin[(rph + 682) & 1023] * 90.0f));

        int aaI = 24 + Ftoi((30.0f + 35.0f * inten) * (0.5f + 0.5f * s_sin[rph]));
        BYTE aa = ClampByte(aaI);

        DWORD rc = MakeARGB(aa, ra, ga, ba);
        V2D rq[4] = { {0,y,0,1,rc},{W,y,0,1,rc},{0,y + dy,0,1,rc},{W,y + dy,0,1,rc} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, rq, sizeof(V2D));
    }

    // ---- Hero sweeps (big, readable) ----
    DWORD lt = LocalSceneMs(tMs);

    // periodic thick ribbon
    int phR = (int)((tMs / 7u) & 1023u);
    float yMid = H * (0.50f + 0.18f * s_sin[phR]);
    float bandH = 10.0f + 10.0f * inten;
    int aB = 18 + Ftoi(90.0f * inten);
    DWORD cBand = MakeARGB(ClampByte(aB), 60, 220, 255);
    DrawBandH(W, yMid - bandH, yMid + bandH, cBand);

    // peak sweep around ~19-21s
    float peak = 0.0f;
    if (lt > 18000u && lt < 21500u)
    {
        int d = (int)lt - 19750;
        if (d < 0) d = -d;
        peak = Clamp01f(1.0f - (float)d * (1.0f / 1750.0f));
    }

    if (peak > 0.02f)
    {
        int aS = 25 + Ftoi(170.0f * peak);
        DWORD cSweep = MakeARGB(ClampByte(aS), 255, 255, 255);
        float y0 = H * 0.18f;
        float y1 = H * 0.82f;
        // brighten with global pulse as well
        if (pulse > 0.0f)
            aS = 25 + Ftoi(170.0f * (peak + 0.5f * pulse));
        cSweep = MakeARGB(ClampByte(aS), 255, 255, 255);
        DrawBandH(W, y0, y1, cSweep);
    }
}


// ------------------------------------------------------------
// Face 2: Matrix digital rain (2D, no font — block glyphs)
// Uses coloured quads to simulate falling green characters
// ------------------------------------------------------------
static const int RAIN_COLS = 16;
static const int RAIN_ROWS = 16;

struct RainDrop { int head; int tailLen; int speed; int phase; };
static RainDrop s_rain[RAIN_COLS];
static bool     s_rainBuilt = false;

static void BuildRain()
{
    if (s_rainBuilt) return;
    for (int c = 0; c < RAIN_COLS; ++c)
    {
        unsigned sd = (unsigned)(c * 1337u + 0xDEAD);
        sd = sd * 1664525u + 1013904223u;
        s_rain[c].tailLen = 3 + (int)(sd & 7u);
        sd = sd * 1664525u + 1013904223u;
        s_rain[c].speed = 1 + (int)(sd & 3u);
        sd = sd * 1664525u + 1013904223u;
        s_rain[c].phase = (int)(sd % (unsigned)(RAIN_ROWS + s_rain[c].tailLen + 4));
        s_rain[c].head = 0;
    }
    s_rainBuilt = true;
}


static void DrawFace2_MatrixRain(DWORD tMs)
{
    const float W = (float)CUBE_FACE_SZ;
    const float H = (float)CUBE_FACE_SZ;
    const float CW = W / (float)RAIN_COLS;
    const float RH = H / (float)RAIN_ROWS;

    // Deep black background
    Begin2D(false);
    DWORD black = MakeARGB(255, 0, 4, 0);
    V2D bg[4] = { {0,0,0,1,black},{W,0,0,1,black},{0,H,0,1,black},{W,H,0,1,black} };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(V2D));

    Begin2D(true);

    float inten = SceneIntensity(tMs);
    float pulse = ScenePulse(tMs);

    unsigned frameStep = tMs / 60u;

    for (int col = 0; col < RAIN_COLS; ++col)
    {
        const RainDrop& rd = s_rain[col];
        int wrap = RAIN_ROWS + rd.tailLen + 4;
        int head = (int)((frameStep / (unsigned)rd.speed + (unsigned)rd.phase) % (unsigned)wrap);

        for (int row = 0; row < RAIN_ROWS; ++row)
        {
            int dist = (head >= row) ? (head - row) : (head + wrap - row);
            if (dist > rd.tailLen) continue;

            float x = (float)col * CW + 1.0f;
            float y = (float)row * RH + 1.0f;
            float cw = CW - 2.0f;
            float rh = RH - 2.0f;

            int a, r, g, b;
            if (dist == 0)
            {
                // Head — bright white-green
                a = 255; r = 200; g = 255; b = 200;
            }
            else if (dist == 1)
            {
                a = 220; r = 30; g = 245; b = 60;
            }
            else if (dist <= 3)
            {
                a = 170; r = 15; g = 210; b = 40;
            }
            else
            {
                // Tail fade
                int fade = 130 - (dist - 3) * 22;
                a = fade < 20 ? 20 : fade;
                r = 8; g = 160; b = 25;
            }

            // boost overall punch slightly over time
            a = a + Ftoi(40.0f * (inten - 0.7f));
            DWORD c = MakeARGB(ClampByte(a), ClampByte(r), ClampByte(g), ClampByte(b));

            V2D q[4] =
            {
                { x,      y,      0,1,c },
                { x + cw, y,      0,1,c },
                { x,      y + rh, 0,1,c },
                { x + cw, y + rh, 0,1,c },
            };
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));

            // Bright horizontal slit — simulates glyph detail
            if (dist <= 1)
            {
                float sy = y + rh * 0.35f;
                float sh = rh * 0.18f;
                DWORD sc = MakeARGB(ClampByte(a - 40), 255, 255, 255);
                V2D sq[4] =
                {
                    { x,      sy,      0,1,sc },
                    { x + cw, sy,      0,1,sc },
                    { x,      sy + sh, 0,1,sc },
                    { x + cw, sy + sh, 0,1,sc },
                };
                g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, sq, sizeof(V2D));
            }
        }
    }

    // ---- Big readable "blocks" (survive cubemap blur) ----
    DWORD lt = LocalSceneMs(tMs);
    int phB = (int)((tMs / 9u) & 1023u);

    // 3 large blocks that drift slightly
    for (int k = 0; k < 3; ++k)
    {
        int ph = (phB + k * 341) & 1023;
        float bx = (0.18f + 0.30f * (float)k) * W + s_sin[ph] * W * 0.06f;
        float by = (0.25f + 0.18f * (float)k) * H + s_cos[(ph + 256) & 1023] * H * 0.06f;

        float bw = W * (0.22f - 0.03f * (float)k);
        float bh = H * (0.10f + 0.02f * (float)k);

        int a0 = 25 + Ftoi(80.0f * inten);
        DWORD bc = MakeARGB(ClampByte(a0), 40, 255, 120);
        V2D bq[4] = { {bx,by,0,1,bc},{bx + bw,by,0,1,bc},{bx,by + bh,0,1,bc},{bx + bw,by + bh,0,1,bc} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bq, sizeof(V2D));
    }

    // ---- Glitch flash around peak (~19-20s) + global pulse ----
    float flash = 0.0f;
    if (lt > 18500u && lt < 20500u)
    {
        int d = (int)lt - 19500;
        if (d < 0) d = -d;
        flash = Clamp01f(1.0f - (float)d * (1.0f / 1000.0f));
    }
    flash = Clamp01f(flash + 0.6f * pulse);

    if (flash > 0.02f)
    {
        int aF = 20 + Ftoi(180.0f * flash);
        DWORD fc = MakeARGB(ClampByte(aF), 120, 255, 180);
        V2D fq[4] = { {0,0,0,1,fc},{W,0,0,1,fc},{0,H,0,1,fc},{W,H,0,1,fc} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fq, sizeof(V2D));
    }
}


// ------------------------------------------------------------
// Face 3: Lava / fire
// Rising heat columns, deep reds and oranges
// ------------------------------------------------------------

static void DrawFace3_Lava(DWORD tMs)
{
    const float W = (float)CUBE_FACE_SZ;
    const float H = (float)CUBE_FACE_SZ;

    float inten = SceneIntensity(tMs);
    float pulse = ScenePulse(tMs);

    // Deep red-black background (darker for contrast)
    Begin2D(false);
    int bph = (int)((tMs / 20u) & 1023u);
    BYTE br = ClampByte(12 + Ftoi(s_sin[bph] * 6.0f));
    DWORD bgc = MakeARGB(255, br, 1, 0);
    V2D bg[4] = { {0,0,0,1,bgc},{W,0,0,1,bgc},{0,H,0,1,bgc},{W,H,0,1,bgc} };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(V2D));

    Begin2D(true);

    // Rising heat columns (slightly thicker/brighter over time)
    const int HEAT_COLS = 20;
    int base = (int)((tMs / 4u) & 1023u);

    for (int col = 0; col < HEAT_COLS; ++col)
    {
        int ph = (base + col * (1024 / HEAT_COLS)) & 1023;
        int ph2 = (base + col * (1024 / HEAT_COLS) + 300) & 1023;

        float cx = ((float)col + 0.5f) * (W / (float)HEAT_COLS);
        float colW = (W / (float)HEAT_COLS) * (0.70f + 0.10f * inten);

        float htFrac = 0.30f + s_sin[ph] * 0.25f + s_cos[ph2] * 0.15f;
        if (htFrac < 0.05f) htFrac = 0.05f;
        float top = H * (1.0f - htFrac);

        BYTE cr = 255;
        BYTE cg = ClampByte(110 + Ftoi(s_sin[ph2] * 95.0f));
        BYTE cb = 0;

        int aBot = 190 + Ftoi(40.0f * inten);
        int aTop = 70 + Ftoi(25.0f * inten);

        DWORD cBot = MakeARGB(ClampByte(aBot), cr, cg, cb);
        DWORD cTop = MakeARGB(ClampByte(aTop), 170, 18, 0);

        float x0 = cx - colW * 0.5f;
        float x1 = cx + colW * 0.5f;

        V2D q[4] =
        {
            { x0, top, 0,1,cTop },
            { x1, top, 0,1,cTop },
            { x0,   H,  0,1,cBot },
            { x1,   H,  0,1,cBot },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }

    // Big magma pools (hero shapes that survive blur)
    DWORD lt = LocalSceneMs(tMs);
    float erupt = 0.0f;
    if (lt > 17500u && lt < 21500u)
    {
        int d = (int)lt - 19500;
        if (d < 0) d = -d;
        erupt = Clamp01f(1.0f - (float)d * (1.0f / 2000.0f));
    }
    erupt = Clamp01f(erupt + 0.6f * pulse);

    float poolR0 = W * (0.14f + 0.05f * inten);
    float poolR1 = W * (0.20f + 0.08f * erupt);

    int phP = (int)((tMs / 13u) & 1023u);
    float px0 = W * 0.32f + s_sin[phP] * W * 0.06f;
    float px1 = W * 0.68f + s_cos[(phP + 256) & 1023] * W * 0.06f;
    float py = H * 0.83f;

    int aC = 22 + Ftoi(120.0f * (0.7f * inten + 0.9f * erupt));
    DWORD cHot = MakeARGB(ClampByte(aC), 255, 200, 40);
    DWORD cZero = MakeARGB(0, 0, 0, 0);

    DrawRadialGlow(W, H, px0, py, poolR1, cHot, cZero);
    DrawRadialGlow(W, H, px1, py, poolR0, cHot, cZero);

    // eruption flash (brief white-hot pop)
    if (erupt > 0.10f)
    {
        int aF = 10 + Ftoi(160.0f * erupt);
        DWORD cF = MakeARGB(ClampByte(aF), 255, 255, 255);
        DrawRadialGlow(W, H, W * 0.50f, H * 0.78f, W * (0.22f + 0.12f * erupt), cF, cZero);
    }
}


// ------------------------------------------------------------
// Face 4: Electric energy grid
// Blue-white grid lines with lightning crackle nodes
// ------------------------------------------------------------

static void DrawFace4_EnergyGrid(DWORD tMs)
{
    const float W = (float)CUBE_FACE_SZ;
    const float H = (float)CUBE_FACE_SZ;

    float inten = SceneIntensity(tMs);
    float pulse = ScenePulse(tMs);

    // Black background (slightly deeper)
    Begin2D(false);
    DWORD black = MakeARGB(255, 0, 0, 6);
    V2D bg[4] = { {0,0,0,1,black},{W,0,0,1,black},{0,H,0,1,black},{W,H,0,1,black} };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(V2D));

    Begin2D(true);

    const int GRID = 8;
    float stepX = W / (float)GRID;
    float stepY = H / (float)GRID;

    int base = (int)((tMs / 6u) & 1023u);

    // Grid lines — horizontal
    for (int row = 0; row <= GRID; ++row)
    {
        int ph = (base + row * (1024 / GRID)) & 1023;
        float y = (float)row * stepY;
        float dy = 1.5f + 0.6f * inten;
        BYTE a = ClampByte(34 + Ftoi(s_sin[ph] * 40.0f) + Ftoi(18.0f * inten));
        BYTE b = ClampByte(150 + Ftoi(s_sin[ph] * 80.0f) + Ftoi(35.0f * inten));
        DWORD c = MakeARGB(a, ClampByte(a / 3), ClampByte(a / 2), b);
        V2D q[4] = { {0,y,0,1,c},{W,y,0,1,c},{0,y + dy,0,1,c},{W,y + dy,0,1,c} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }

    // Grid lines — vertical
    for (int col = 0; col <= GRID; ++col)
    {
        int ph = (base + col * (1024 / GRID) + 512) & 1023;
        float x = (float)col * stepX;
        float dx = 1.5f + 0.6f * inten;
        BYTE a = ClampByte(34 + Ftoi(s_sin[ph] * 40.0f) + Ftoi(18.0f * inten));
        BYTE b = ClampByte(130 + Ftoi(s_sin[ph] * 95.0f) + Ftoi(40.0f * inten));
        DWORD c = MakeARGB(a, ClampByte(a / 4), ClampByte(a / 3), b);
        V2D q[4] = { {x,0,0,1,c},{x + dx,0,0,1,c},{x,H,0,1,c},{x + dx,H,0,1,c} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }

    // Lightning nodes (bigger, brighter)
    const int SEG = 10;
    for (int row = 0; row <= GRID; ++row)
    {
        for (int col = 0; col <= GRID; ++col)
        {
            int ph = (base + row * 97 + col * 131) & 1023;
            int a = Ftoi(s_sin[ph] * 127.0f);
            if (a < 15) continue;

            float cx = (float)col * stepX;
            float cy = (float)row * stepY;
            float r = (2.8f + s_sin[(ph + 256) & 1023] * 2.4f) * (0.9f + 0.6f * inten);

            BYTE na = ClampByte(a * 2 + Ftoi(40.0f * inten));
            DWORD nc = MakeARGB(na, 80, 200, 255);
            DWORD nedge = MakeARGB(0, 10, 60, 220);

            static V2D fan[SEG + 2];
            fan[0] = { cx, cy, 0,1,nc };
            for (int s = 0; s <= SEG; ++s)
            {
                int ai = (s * 1024 / SEG) & 1023;
                fan[s + 1] = { cx + s_cos[ai] * r, cy + s_sin[ai] * r, 0,1,nedge };
            }
            g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, SEG, fan, sizeof(V2D));
        }
    }

    // Bright energy pulse sweeping across (kept)
    int pph = (int)((tMs / 3u) & 1023u);
    float px = s_cos[pph] * W * 0.5f + W * 0.5f;
    float pr = W * (0.07f + 0.03f * inten);
    BYTE pa = ClampByte(70 + Ftoi(s_sin[pph] * 70.0f) + Ftoi(30.0f * inten));
    DWORD pc = MakeARGB(pa, 110, 220, 255);
    DWORD pedge = MakeARGB(0, 10, 50, 200);

    const int PSEG = 16;
    static V2D pfan[PSEG + 2];
    pfan[0] = { px, H * 0.5f, 0,1,pc };
    for (int s = 0; s <= PSEG; ++s)
    {
        int ai = (s * 1024 / PSEG) & 1023;
        pfan[s + 1] = { px + s_cos[ai] * pr, H * 0.5f + s_sin[ai] * pr, 0,1,pedge };
    }
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, PSEG, pfan, sizeof(V2D));

    // ---- Convergence flash (global pulse ties all faces) ----
    if (pulse > 0.02f)
    {
        int aF = 10 + Ftoi(170.0f * pulse);
        DWORD fc = MakeARGB(ClampByte(aF), 200, 255, 255);
        V2D fq[4] = { {0,0,0,1,fc},{W,0,0,1,fc},{0,H,0,1,fc},{W,H,0,1,fc} };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, fq, sizeof(V2D));
    }
}


// ------------------------------------------------------------
// Face 5: Void particle drift
// Dark void with slow drifting particles, deep purples
// ------------------------------------------------------------
static const int VOID_PART_N = 80;
struct VoidParticle { float nx, ny; int phX, phY; int bright; int colR, colG, colB; };
static VoidParticle s_voidPart[VOID_PART_N];
static bool         s_voidBuilt = false;

static void BuildVoidParticles()
{
    if (s_voidBuilt) return;
    for (int i = 0; i < VOID_PART_N; ++i)
    {
        unsigned sd = (unsigned)(i * 999u + 0xC0FFEE);
        sd = sd * 1664525u + 1013904223u;
        s_voidPart[i].nx = (float)(sd & 0xFFFFu) * (1.0f / 65535.0f);
        sd = sd * 1664525u + 1013904223u;
        s_voidPart[i].ny = (float)(sd & 0xFFFFu) * (1.0f / 65535.0f);
        sd = sd * 1664525u + 1013904223u;
        s_voidPart[i].phX = (int)(sd & 1023u);
        sd = sd * 1664525u + 1013904223u;
        s_voidPart[i].phY = (int)(sd & 1023u);
        sd = sd * 1664525u + 1013904223u;
        s_voidPart[i].bright = 30 + (int)(sd % 120u);
        sd = sd * 1664525u + 1013904223u;
        int hue = (int)(sd % 3u);
        if (hue == 0) { s_voidPart[i].colR = 180; s_voidPart[i].colG = 20; s_voidPart[i].colB = 200; }
        else if (hue == 1) { s_voidPart[i].colR = 20; s_voidPart[i].colG = 40; s_voidPart[i].colB = 220; }
        else { s_voidPart[i].colR = 100; s_voidPart[i].colG = 10; s_voidPart[i].colB = 180; }
    }
    s_voidBuilt = true;
}


static void DrawFace5_Void(DWORD tMs)
{
    const float W = (float)CUBE_FACE_SZ;
    const float H = (float)CUBE_FACE_SZ;

    float inten = SceneIntensity(tMs);
    float pulse = ScenePulse(tMs);

    // Pure black void background
    Begin2D(false);
    DWORD black = MakeARGB(255, 0, 0, 0);
    V2D bg[4] = { {0,0,0,1,black},{W,0,0,1,black},{0,H,0,1,black},{W,H,0,1,black} };
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, bg, sizeof(V2D));

    // Faint purple nebula wisps
    Begin2D(true);
    int wph = (int)((tMs / 25u) & 1023u);
    for (int w = 0; w < 3; ++w)
    {
        int wph2 = (wph + w * 341) & 1023;
        float cx = (0.2f + (float)w * 0.3f) * W + s_sin[wph2] * W * 0.1f;
        float cy = W * 0.5f + s_cos[(wph2 + 256) & 1023] * H * 0.2f;
        float r = W * (0.18f + 0.03f * inten) + s_sin[(wph2 + 512) & 1023] * W * 0.05f;
        int a = 10 + Ftoi((10.0f + 10.0f * inten) * (0.6f + 0.4f * s_sin[wph2]));
        DWORD wc = MakeARGB(ClampByte(a), 60, 10, 120);
        DWORD wedge = MakeARGB(0, 30, 5, 60);
        const int WSEG = 12;
        static V2D wfan[WSEG + 2];
        wfan[0] = { cx, cy, 0,1,wc };
        for (int s = 0; s <= WSEG; ++s)
        {
            int ai = (s * 1024 / WSEG) & 1023;
            wfan[s + 1] = { cx + s_cos[ai] * r, cy + s_sin[ai] * r, 0,1,wedge };
        }
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLEFAN, WSEG, wfan, sizeof(V2D));
    }

    // Slow drifting particles (kept)
    int base = (int)((tMs / 12u) & 1023u);
    for (int i = 0; i < VOID_PART_N; ++i)
    {
        const VoidParticle& vp = s_voidPart[i];
        int phX = (base + vp.phX) & 1023;
        int phY = (base + vp.phY) & 1023;

        float px = vp.nx * W + s_sin[phX] * W * 0.08f;
        float py = vp.ny * H + s_cos[phY] * H * 0.06f;

        int tw = (s_sin[phX] > 0.0f) ? Ftoi(s_sin[phX] * 40.0f) : 0;
        int br = vp.bright + tw + Ftoi(25.0f * (inten - 0.7f));
        BYTE a = ClampByte(br);

        DWORD c = MakeARGB(a, ClampByte(vp.colR * br / 200),
            ClampByte(vp.colG * br / 200),
            ClampByte(vp.colB * br / 200));

        float sz = 2.0f + s_sin[(phX + 256) & 1023] * 1.0f;

        V2D q[4] =
        {
            { px,      py,      0,1,c },
            { px + sz, py,      0,1,c },
            { px,      py + sz, 0,1,c },
            { px + sz, py + sz, 0,1,c },
        };
        g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(V2D));
    }

    // ---- Hero rim + inward pulse (reads great in reflection) ----
    DWORD lt = LocalSceneMs(tMs);
    float suck = 0.0f;
    if (lt > 17500u && lt < 21500u)
    {
        int d = (int)lt - 19750;
        if (d < 0) d = -d;
        suck = Clamp01f(1.0f - (float)d * (1.0f / 2000.0f));
    }
    suck = Clamp01f(suck + 0.8f * pulse);

    float cx = W * 0.50f;
    float cy = H * 0.50f;

    float rRim = W * (0.36f + 0.04f * s_sin[(int)((tMs / 17u) & 1023u)]);
    float rCore = W * (0.12f + 0.06f * suck);

    int aR = 10 + Ftoi(90.0f * (inten + 0.7f * suck));
    DWORD cR = MakeARGB(ClampByte(aR), 120, 80, 255);
    DWORD cZ = MakeARGB(0, 0, 0, 0);

    DrawRadialGlow(W, H, cx, cy, rRim, cR, cZ);

    if (suck > 0.05f)
    {
        int aC = 8 + Ftoi(150.0f * suck);
        DWORD cC = MakeARGB(ClampByte(aC), 255, 255, 255);
        DrawRadialGlow(W, H, cx, cy, rCore, cC, cZ);
    }
}


// ------------------------------------------------------------
// Render environment into one cube face — dispatches to per-face renderer
// ------------------------------------------------------------
static void RenderCubeFace(int face, DWORD tMs)
{
    if (!s_cubeMap || !s_cubeDepth) return;

    LPDIRECT3DSURFACE8 faceSurf = NULL;
    if (FAILED(s_cubeMap->GetCubeMapSurface((D3DCUBEMAP_FACES)face, 0, &faceSurf)))
        return;

    LPDIRECT3DSURFACE8 oldRT = NULL;
    LPDIRECT3DSURFACE8 oldDepth = NULL;
    g_pDevice->GetRenderTarget(&oldRT);
    g_pDevice->GetDepthStencilSurface(&oldDepth);

    g_pDevice->SetRenderTarget(faceSurf, s_cubeDepth);
    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER,
        D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    D3DVIEWPORT8 vp = { 0, 0, (DWORD)CUBE_FACE_SZ, (DWORD)CUBE_FACE_SZ, 0.0f, 1.0f };
    g_pDevice->SetViewport(&vp);

    g_pDevice->BeginScene();
    switch (face)
    {
    case 0: DrawFace0_Nebula(tMs);      break;
    case 1: DrawFace1_Plasma(tMs);      break;
    case 2: DrawFace2_MatrixRain(tMs);  break;
    case 3: DrawFace3_Lava(tMs);        break;
    case 4: DrawFace4_EnergyGrid(tMs);  break;
    case 5: DrawFace5_Void(tMs);        break;
    }
    g_pDevice->EndScene();

    g_pDevice->SetRenderTarget(oldRT, oldDepth);
    D3DVIEWPORT8 mainVP = { 0, 0, (DWORD)SCREEN_W, (DWORD)SCREEN_H, 0.0f, 1.0f };
    g_pDevice->SetViewport(&mainVP);

    if (oldRT)    oldRT->Release();
    if (oldDepth) oldDepth->Release();
    faceSurf->Release();
}

// ------------------------------------------------------------
// Camera — slow orbit
// ------------------------------------------------------------
static void SetupCamera(DWORD tMs, float* outCamX, float* outCamY, float* outCamZ)
{
    int phCam = (int)((tMs / 9u) & 1023u);
    int phCamY = (int)((tMs / 23u) & 1023u);

    float camR = 6.0f;
    float ex = s_cos[phCam] * camR;
    float ey = 1.4f + s_sin[phCamY] * 0.7f;
    float ez = s_sin[phCam] * camR;

    if (outCamX) *outCamX = ex;
    if (outCamY) *outCamY = ey;
    if (outCamZ) *outCamZ = ez;

    D3DXVECTOR3 eye(ex, ey, ez);
    D3DXVECTOR3 at(0.0f, 0.0f, 0.0f);
    D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);

    D3DXMATRIX view, proj;
    D3DXMatrixLookAtLH(&view, &eye, &at, &up);
    D3DXMatrixPerspectiveFovLH(&proj, D3DX_PI / 3.0f,
        SCREEN_W / SCREEN_H, 0.1f, 100.0f);
    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);
}

// ------------------------------------------------------------
// Render the reflective cube — 4 passes
// ------------------------------------------------------------
static void RenderCube(const D3DXMATRIX& world, DWORD tMs)
{
    if (!s_cubeVB || !s_cubeIB) return;

    g_pDevice->SetTransform(D3DTS_WORLD, &world);
    g_pDevice->SetStreamSource(0, s_cubeVB, sizeof(CubeVtx));
    g_pDevice->SetIndices(s_cubeIB, 0);
    g_pDevice->SetVertexShader(FVF_CUBE);
    SetLinear(0); SetLinear(1); SetLinear(2);

    // Animated UV ripple matrix — liquid chrome surface
    float uOff = 0.018f * s_sin[(int)((tMs / 32u) & 1023u)]
        + 0.008f * s_sin[(int)((tMs / 14u) & 1023u)];
    float vOff = 0.018f * s_cos[(int)((tMs / 37u) & 1023u)]
        + 0.008f * s_cos[(int)((tMs / 16u) & 1023u)];

    // Rotation angle from LUT — rot in radians via small index
    int rotIdx = (int)((tMs / 53u) & 1023u);
    float cr2 = s_cos[rotIdx];
    float sr2 = s_sin[rotIdx] * 0.04f;  // small rotation only

    D3DXMATRIX texMat, identTex;
    D3DXMatrixIdentity(&texMat);
    texMat._11 = cr2;  texMat._12 = sr2;
    texMat._21 = -sr2;  texMat._22 = cr2;
    texMat._31 = uOff;  texMat._32 = vOff;
    D3DXMatrixIdentity(&identTex);

    // ── PASS 1: DOT3 bump x diffuse (opaque, writes Z) ───────────────────────
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    g_pDevice->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0), &texMat);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT2);
    g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);

    g_pDevice->SetTexture(0, s_texNormal);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->SetTexture(1, s_texDiff);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
    g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
    g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
    g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, CUBE_VERTS, 0, CUBE_TRIS);

    g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    g_pDevice->SetTransform((D3DTRANSFORMSTATETYPE)(D3DTS_TEXTURE0), &identTex);

    // ── PASS 2: environment cube map reflection (additive, no Z write) ────────
    if (s_cubeMap && s_cubeReady)
    {
        int phRefl = (int)((tMs / 6u) & 1023u);
        float rp = 0.38f + s_sin[phRefl] * 0.12f;
        BYTE refI = ClampByte(Ftoi(rp * 255.0f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(refI, refI, refI));

        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

        g_pDevice->SetTexture(0, s_cubeMap);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX,
            D3DTSS_TCI_CAMERASPACEREFLECTIONVECTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_COUNT3);

        g_pDevice->SetTexture(1, s_texMask);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_TEXCOORDINDEX, 1);

        g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, CUBE_VERTS, 0, CUBE_TRIS);

        g_pDevice->SetTextureStageState(0, D3DTSS_TEXCOORDINDEX, 0);
        g_pDevice->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    }

    // ── PASS 3: specular highlight — green-white (additive, no Z write) ───────
    if (s_texMask)
    {
        int phSpec = (int)((tMs / 7u) & 1023u);
        float sc = 0.5f + s_sin[phSpec] * 0.5f;
        BYTE spR = ClampByte(Ftoi((0.28f + 0.10f * sc) * 255.0f));
        BYTE spG = ClampByte(Ftoi((0.40f + 0.15f * sc) * 255.0f));
        BYTE spB = ClampByte(Ftoi((0.32f + 0.10f * (1.0f - sc)) * 255.0f));
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(spR, spG, spB));

        g_pDevice->SetTexture(0, s_texMask);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pDevice->SetTexture(1, NULL);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, CUBE_VERTS, 0, CUBE_TRIS);
    }

    // ── PASS 4: rim light — teal-blue edge catch (additive, no Z write) ───────
    {
        int phA = (int)((tMs / 8u) & 1023u);
        float plx = s_cos[phA] * 2.2f;
        float ply = 0.8f;
        float plz = s_sin[phA] * 2.2f;
        float plen = sqrtf(plx * plx + ply * ply + plz * plz);
        if (plen < 0.0001f) plen = 1.0f;

        float rimX = -(plx / plen) * 0.7f + 0.3f;
        float rimY = -(ply / plen) * 0.7f - 0.2f;
        float rimZ = -(plz / plen) * 0.7f;
        float rlen = sqrtf(rimX * rimX + rimY * rimY + rimZ * rimZ);
        if (rlen < 0.0001f) rlen = 1.0f;
        rimX /= rlen; rimY /= rlen; rimZ /= rlen;

        BYTE rimR = ClampByte(Ftoi((rimX * 0.5f + 0.5f) * 255.0f));
        BYTE rimG = ClampByte(Ftoi((rimY * 0.5f + 0.5f) * 255.0f));
        BYTE rimB = ClampByte(Ftoi((rimZ * 0.5f + 0.5f) * 255.0f));

        int   phRim = (int)((tMs / 9u) & 1023u);
        float rimSc = 0.28f + s_sin[phRim] * 0.08f;

        BYTE rimTR = ClampByte(Ftoi((float)rimR * rimSc * 0.4f));
        BYTE rimTG = ClampByte(Ftoi((float)rimG * rimSc * 1.0f));
        BYTE rimTB = ClampByte(Ftoi((float)rimB * rimSc * 0.9f));

        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rimR, rimG, rimB));

        g_pDevice->SetTexture(0, s_texNormal);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_DOTPRODUCT3);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, D3DCOLOR_XRGB(rimTR, rimTG, rimTB));
        g_pDevice->SetTexture(1, NULL);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_MODULATE);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG1, D3DTA_CURRENT);
        g_pDevice->SetTextureStageState(1, D3DTSS_COLORARG2, D3DTA_TFACTOR);
        g_pDevice->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pDevice->SetTextureStageState(2, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(2, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

        g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, CUBE_VERTS, 0, CUBE_TRIS);
    }

    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------
void CubeEnvScene_Init()
{
    s_active = false;
    s_startTicks = GetTickCount();
    s_loadStep = 0;
    s_nextFace = 0;

    BuildLUT();
    BuildEnvStars();
    BuildClouds();
    BuildRain();
    BuildVoidParticles();
    UndoState();
    CreateCubeRT();
    BuildCubeGeometry();
}

void CubeEnvScene_Shutdown()
{
    s_active = false;
    if (g_pDevice) UndoState();
    ReleaseCubeGeometry();
    ReleaseCubeRT();
    ReleaseTextures();
}

bool CubeEnvScene_IsFinished()
{
    return (GetTickCount() - s_startTicks) >= SCENE_DURATION_MS;
}

void CubeEnvScene_Render(float)
{
    if (!g_pDevice) return;

    // Step loader — one texture per frame, background always draws
    if (s_loadStep <= 4)
        StepLoad();

    DWORD tMs = GetTickCount() - s_startTicks;

    // ── Cube world matrix ─────────────────────────────────────────────────────
    int phY = (int)((tMs / 7u) & 1023u);
    int phX = (int)((tMs / 11u) & 1023u);
    int phZ = (int)((tMs / 16u) & 1023u);

    float ry = (float)tMs * 0.001f * 0.55f + s_sin[phY] * 0.12f;
    float rx = (float)tMs * 0.001f * 0.33f + s_sin[phX] * 0.08f;
    float rz = (float)tMs * 0.001f * 0.18f + s_sin[phZ] * 0.06f;

    D3DXMATRIX mx, my, mzm, world;
    D3DXMatrixRotationX(&mx, rx);
    D3DXMatrixRotationY(&my, ry);
    D3DXMatrixRotationZ(&mzm, rz);
    world = mx * my * mzm;

    // ── Staggered cube face update + cull ─────────────────────────────────────

    // ── Main scene background (full 640x480) ──────────────────────────────────
    DrawNebula(tMs, SCREEN_W, SCREEN_H);
    DrawNebulaClouds(tMs, SCREEN_W, SCREEN_H);
    DrawStars(tMs, SCREEN_W, SCREEN_H);
    DrawLensGlow(tMs, SCREEN_W, SCREEN_H);

    if (!s_active) return;

    // ── Camera + cube render ──────────────────────────────────────────────────
    float camX, camY, camZ;
    SetupCamera(tMs, &camX, &camY, &camZ);

    UpdateDOT3(world, tMs);

    // Clean 3D state before cube draw
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ZERO);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_CCW);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    for (int si = 0; si < 4; ++si)
    {
        g_pDevice->SetTexture(si, NULL);
        g_pDevice->SetTextureStageState(si, D3DTSS_COLOROP, D3DTOP_DISABLE);
        g_pDevice->SetTextureStageState(si, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    }

    RenderCube(world, tMs);
    UndoState();
}