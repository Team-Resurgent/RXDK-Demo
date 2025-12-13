// RingScene.cpp - 3D Torus Demo Scene
// Three real 3D rings: wireframe, transparent, textured glow
// Background: spherical neon lattice for depth
// Textured ring uses D:\metal.dds

#include "RingScene.h"
#include <xtl.h>
#include <xgraphics.h>
#include <math.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// -----------------------------------------------------------------------------
// Scene constants
// -----------------------------------------------------------------------------

static const float SCENE_DURATION = 15.0f;   // 15 seconds

// -----------------------------------------------------------------------------
// Internal scene state
// -----------------------------------------------------------------------------

static bool  s_active = false;
static float s_startTime = 0.0f;

static LPDIRECT3DVERTEXBUFFER8 s_vb = nullptr;
static LPDIRECT3DINDEXBUFFER8  s_ib = nullptr;
static int                     s_numVerts = 0;
static int                     s_numIndices = 0;

// Textured torus texture (metal.dds)
static LPDIRECT3DTEXTURE8 s_tex = nullptr;

// Simple integer tick for animation (no float->int helpers)
static int s_tick = 0;

// -----------------------------------------------------------------------------
// Small helpers
// -----------------------------------------------------------------------------

// Fade 0..1 over scene lifetime
static float FadeAlpha(float t)
{
    const float FADE = 1.0f;   // 1 second in/out

    if (t < FADE)
        return t / FADE;

    if (t > (SCENE_DURATION - FADE))
        return (SCENE_DURATION - t) / FADE;

    return 1.0f;
}

// Pure integer RGB cycle based on a tick (no float usage inside)
static DWORD MakeRgbCycle(int tick)
{
    // Cycle across 3 segments of 256 steps each (total 768)
    int h = tick % 768;
    if (h < 0) h += 768;

    int r, g, b;

    if (h < 256)
    {
        // Red -> Yellow (R=255, G:0->255, B=0)
        r = 255;
        g = h;
        b = 0;
    }
    else if (h < 512)
    {
        // Yellow -> Green (R:255->0, G=255, B=0)
        int k = h - 256;
        r = 255 - k;
        g = 255;
        b = 0;
    }
    else
    {
        // Green -> Cyan (R=0, G=255, B:0->255)
        int k = h - 512;
        r = 0;
        g = 255;
        b = k;
    }

    if (r < 0)   r = 0;   if (r > 255) r = 255;
    if (g < 0)   g = 0;   if (g > 255) g = 255;
    if (b < 0)   b = 0;   if (b > 255) b = 255;

    BYTE a = 180; // some transparency for the additive ring

    return ((DWORD)a << 24) |
        ((DWORD)r << 16) |
        ((DWORD)g << 8) |
        (DWORD)b;
}

// -----------------------------------------------------------------------------
// Torus mesh generator
// -----------------------------------------------------------------------------

struct TorusVertex
{
    float x, y, z;
    float nx, ny, nz;
    float u, v;
};

#define FVF_TORUS (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_TEX1)

static void CreateTorusMesh(float radius, float tube, int segR, int segT)
{
    s_numVerts = segR * segT;
    s_numIndices = segR * segT * 6;

    g_pDevice->CreateVertexBuffer(
        s_numVerts * sizeof(TorusVertex),
        0, FVF_TORUS,
        D3DPOOL_MANAGED,
        &s_vb);

    g_pDevice->CreateIndexBuffer(
        s_numIndices * sizeof(WORD),
        0, D3DFMT_INDEX16,
        D3DPOOL_MANAGED,
        &s_ib);

    TorusVertex* v;
    s_vb->Lock(0, 0, (BYTE**)&v, 0);

    for (int r = 0; r < segR; r++)
    {
        float ur = (float)r / (float)segR * D3DX_PI * 2.0f;

        float cosr = cosf(ur);
        float sinr = sinf(ur);

        for (int t = 0; t < segT; t++)
        {
            float ut = (float)t / (float)segT * D3DX_PI * 2.0f;

            float cost = cosf(ut);
            float sint = sinf(ut);

            int idx = r * segT + t;

            float x = (radius + tube * cost) * cosr;
            float y = (radius + tube * cost) * sinr;
            float z = tube * sint;

            float nx = cost * cosr;
            float ny = cost * sinr;
            float nz = sint;

            v[idx].x = x;  v[idx].y = y;  v[idx].z = z;
            v[idx].nx = nx; v[idx].ny = ny; v[idx].nz = nz;

            // Slight UV tiling to help the metal texture
            float u = (float)r / (float)segR * 4.0f;
            float vtex = (float)t / (float)segT * 4.0f;

            v[idx].u = u;
            v[idx].v = vtex;
        }
    }

    s_vb->Unlock();

    WORD* idx;
    s_ib->Lock(0, 0, (BYTE**)&idx, 0);

    int i = 0;
    for (int r = 0; r < segR; r++)
    {
        int r2 = (r + 1) % segR;
        for (int t = 0; t < segT; t++)
        {
            int t2 = (t + 1) % segT;

            WORD v00 = (WORD)(r * segT + t);
            WORD v01 = (WORD)(r * segT + t2);
            WORD v10 = (WORD)(r2 * segT + t);
            WORD v11 = (WORD)(r2 * segT + t2);

            idx[i++] = v00; idx[i++] = v10; idx[i++] = v11;
            idx[i++] = v00; idx[i++] = v11; idx[i++] = v01;
        }
    }

    s_ib->Unlock();
}

// -----------------------------------------------------------------------------
// Background spherical lattice (neon Xbox green)
// -----------------------------------------------------------------------------

struct LatticeVertex
{
    float x, y, z;
    DWORD color;
};

#define FVF_LATTICE (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static void DrawSphericalLattice(float t)
{
    if (!g_pDevice) return;

    // Denser lattice, larger radius so camera is inside the sphere
    const int   LAT_LINES = 16;   // more latitudinal rings
    const int   LON_LINES = 32;   // more longitudinal rings
    const float RADIUS = 7.0f; // bigger than camera radius (~5.5)
    const DWORD COL = D3DCOLOR_ARGB(70, 0, 255, 0); // faint Xbox green

    // Ensure this is treated as pure background (no depth)
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(FVF_LATTICE);

    // World rotation for the whole lattice
    D3DXMATRIX mRotY;
    D3DXMatrixRotationY(&mRotY, t * 0.25f);
    g_pDevice->SetTransform(D3DTS_WORLD, &mRotY);

    // --- Latitudinal circles (horizontal bands) ---
    for (int lat = 1; lat < LAT_LINES; ++lat)
    {
        // Map lat 1..LAT_LINES-1 to polar angle (avoid exact poles)
        float v = (float)lat / (float)LAT_LINES;
        float phi = (v - 0.5f) * D3DX_PI * 0.95f;

        LatticeVertex verts[LON_LINES + 1];

        for (int lon = 0; lon <= LON_LINES; ++lon)
        {
            float u = (float)lon / (float)LON_LINES;
            float theta = u * (D3DX_PI * 2.0f);

            float cosPhi = cosf(phi);
            float sinPhi = sinf(phi);
            float cosTheta = cosf(theta);
            float sinTheta = sinf(theta);

            float x = RADIUS * cosPhi * cosTheta;
            float y = RADIUS * sinPhi;
            float z = RADIUS * cosPhi * sinTheta;

            verts[lon].x = x;
            verts[lon].y = y;
            verts[lon].z = z;
            verts[lon].color = COL;
        }

        g_pDevice->DrawPrimitiveUP(
            D3DPT_LINESTRIP,
            LON_LINES,
            verts,
            sizeof(LatticeVertex));
    }

    // --- Longitudinal circles (vertical bands) ---
    for (int lon = 0; lon < LON_LINES; ++lon)
    {
        float u = (float)lon / (float)LON_LINES;
        float theta = u * (D3DX_PI * 2.0f);

        LatticeVertex verts[LAT_LINES + 1];

        for (int lat = 0; lat <= LAT_LINES; ++lat)
        {
            float v = (float)lat / (float)LAT_LINES;
            float phi = (v - 0.5f) * D3DX_PI * 0.95f;

            float cosPhi = cosf(phi);
            float sinPhi = sinf(phi);
            float cosTheta = cosf(theta);
            float sinTheta = sinf(theta);

            float x = RADIUS * cosPhi * cosTheta;
            float y = RADIUS * sinPhi;
            float z = RADIUS * cosPhi * sinTheta;

            verts[lat].x = x;
            verts[lat].y = y;
            verts[lat].z = z;
            verts[lat].color = COL;
        }

        g_pDevice->DrawPrimitiveUP(
            D3DPT_LINESTRIP,
            LAT_LINES,
            verts,
            sizeof(LatticeVertex));
    }
}

// -----------------------------------------------------------------------------
// Init / Shutdown
// -----------------------------------------------------------------------------

void RingScene_Init()
{
    s_active = true;
    s_startTime = 0.0f;
    s_tick = 0;

    CreateTorusMesh(1.2f, 0.4f, 48, 24);

    // Load metal texture from disc
    if (s_tex)
    {
        s_tex->Release();
        s_tex = nullptr;
    }

    if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tex\\metal.dds", &s_tex)))
    {
        s_tex = nullptr; // still runs even if texture is missing
    }

    // Texture filtering for the metal ring
    g_pDevice->SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    g_pDevice->SetTextureStageState(0, D3DTSS_MIPFILTER, D3DTEXF_LINEAR);
}

void RingScene_Shutdown()
{
    s_active = false;

    if (s_vb) { s_vb->Release();  s_vb = nullptr; }
    if (s_ib) { s_ib->Release();  s_ib = nullptr; }
    if (s_tex) { s_tex->Release(); s_tex = nullptr; }
}

// -----------------------------------------------------------------------------
// Render
// -----------------------------------------------------------------------------

bool RingScene_IsFinished()
{
    return (s_startTime >= SCENE_DURATION);
}

void RingScene_Render(float demoTime)
{
    if (!s_active) return;

    if (s_startTime == 0.0f)
        s_startTime = demoTime;

    float t = demoTime - s_startTime;
    if (t > SCENE_DURATION) t = SCENE_DURATION;

    float fade = FadeAlpha(t);

    // Advance integer tick for RGB / movement
    s_tick++;

    // Camera
    float camR = 5.5f - (t * 0.05f);
    float camA = t * 0.7f;
    float camY = sinf(t * 0.3f) * 0.4f;

    float cx = cosf(camA) * camR;
    float cz = sinf(camA) * camR;

    D3DXVECTOR3 eye(cx, camY, cz);
    D3DXVECTOR3 at(0, 0, 0);
    D3DXVECTOR3 up(0, 1, 0);

    D3DXMATRIX mView;
    D3DXMatrixLookAtLH(&mView, &eye, &at, &up);
    g_pDevice->SetTransform(D3DTS_VIEW, &mView);

    D3DXMATRIX mProj;
    D3DXMatrixPerspectiveFovLH(&mProj, D3DX_PI / 3, 640.0f / 480.0f, 0.1f, 50.0f);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &mProj);

    // -------------------------------------------------------------------------
    // Background lattice (neon green sphere behind everything)
    // -------------------------------------------------------------------------
    DrawSphericalLattice(t);

    // -------------------------------------------------------------------------
    // Draw Rings
    // -------------------------------------------------------------------------

    const float RING_OFFSET = 1.8f; // left/right spacing

    g_pDevice->SetStreamSource(0, s_vb, sizeof(TorusVertex));
    g_pDevice->SetIndices(s_ib, 0);
    g_pDevice->SetVertexShader(FVF_TORUS);

    // Reset basic texture stage
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);

    // *** Ring 1 - WIREFRAME (left) ***
    {
        g_pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_WIREFRAME);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pDevice->SetTexture(0, NULL);

        D3DXMATRIX mRot, mTrans, mWorld;
        D3DXMatrixRotationY(&mRot, t * 1.5f);
        D3DXMatrixTranslation(&mTrans, -RING_OFFSET, 0.0f, 0.0f);
        D3DXMatrixMultiply(&mWorld, &mRot, &mTrans);

        g_pDevice->SetTransform(D3DTS_WORLD, &mWorld);

        g_pDevice->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST,
            0, s_numVerts,
            0, s_numIndices / 3);
    }

    // *** Ring 2 - TRANSPARENT + faster RGB cycling (center) ***
    {
        DWORD rgb = MakeRgbCycle(s_tick * 2);

        g_pDevice->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        g_pDevice->SetTexture(0, NULL);

        // Use texture factor as the color source
        g_pDevice->SetRenderState(D3DRS_TEXTUREFACTOR, rgb);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
        g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TFACTOR);

        float scale = 1.1f;
        D3DXMATRIX mScale, mRot, mWorld;
        D3DXMatrixScaling(&mScale, scale, scale, scale);
        D3DXMatrixRotationX(&mRot, t * 0.8f);
        D3DXMatrixMultiply(&mWorld, &mScale, &mRot);

        g_pDevice->SetTransform(D3DTS_WORLD, &mWorld);

        g_pDevice->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST,
            0, s_numVerts,
            0, s_numIndices / 3);
    }

    // Restore stage for textured ring
    g_pDevice->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
    g_pDevice->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);

    // *** Ring 3 - TEXTURED GLOW (right, metal.dds) ***
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
        g_pDevice->SetTexture(0, s_tex);

        D3DXMATRIX mRotY, mRotZ, mTrans, mWorldTmp, mWorld;
        D3DXMatrixRotationY(&mRotY, t * 0.5f);
        D3DXMatrixRotationZ(&mRotZ, t * 1.1f);
        D3DXMatrixTranslation(&mTrans, RING_OFFSET, 0.0f, 0.0f);

        D3DXMatrixMultiply(&mWorldTmp, &mRotY, &mRotZ);
        D3DXMatrixMultiply(&mWorld, &mWorldTmp, &mTrans);

        g_pDevice->SetTransform(D3DTS_WORLD, &mWorld);

        g_pDevice->DrawIndexedPrimitive(
            D3DPT_TRIANGLELIST,
            0, s_numVerts,
            0, s_numIndices / 3);
    }

    // -------------------------------------------------------------------------
    // Fade overlay
    // -------------------------------------------------------------------------

    if (fade < 1.0f)
    {
        DWORD a = (DWORD)(255 * (1.0f - fade));
        DWORD col = (a << 24);

        struct QuadV { float x, y, z, rhw; DWORD c; };
        QuadV q[4] =
        {
            { 0,   0,   0,1, col },
            {640,  0,   0,1, col },
            { 0, 480,   0,1, col },
            {640,480,   0,1, col }
        };

        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_pDevice->SetTexture(0, NULL);
        g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);

        g_pDevice->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            2, q, sizeof(QuadV));
    }
}
