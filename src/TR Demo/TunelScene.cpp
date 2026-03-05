// TunelScene.cpp - Classic checkerboard "snake" tunnel
// - Tunnel = rings of black/white tiles along Z
// - Each ring alternates pattern (ring0: W/B, ring1: B/W, etc.)
// - Camera stays centered, tunnel moves toward camera and snakes in X/Y
// - tr.dds is a disc at the far end, pulled toward us along the same path
// - TR is only visible near the end of the scene, then zooms in
// - No float->int casts (avoids __ftol2_sse issues)

#include "TunelScene.h"
#include <xtl.h>
#include <xgraphics.h>
#include <math.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

static const float TUNEL_DURATION = 18.0f;  // for IsFinished()
static const float CAMERA_SPEED = 3.0f;   // units/s forward

static const int   TUNEL_SEG_THETA = 32;     // quads around
static const int   TUNEL_SEG_Z = 64;     // number of rings
static const float TUNEL_RADIUS = 3.5f;
static const float TUNEL_STEP_Z = 1.2f;   // spacing between rings
static const float TUNEL_FRONT_OFFSET = 4.0f;   // keep nearest ring in front

// Snake motion
static const float SNAKE_FREQ_Z = 0.25f;  // path vs Z
static const float SNAKE_FREQ_T = 0.8f;   // wiggle vs time
static const float SNAKE_AMP_X = 1.4f;
static const float SNAKE_AMP_Y = 0.9f;

// Logo & reveal timing
static const float LOGO_BASE_Z =
(float)(TUNEL_SEG_Z - 1) * TUNEL_STEP_Z + 10.0f;

// When TR starts to appear and zoom (seconds since scene start)
static const float LOGO_REVEAL_START_T = 12.0f;  // start showing TR
static const float LOGO_FULL_ZOOM_T = 17.5f;  // by here TR is almost filling view

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static bool  s_active = false;
static float s_startTime = 0.0f;

struct TunnelVertex
{
    float x, y, z;
    DWORD color;
};

#define FVF_TUNEL (D3DFVF_XYZ | D3DFVF_DIFFUSE)

static LPDIRECT3DVERTEXBUFFER8 s_vb = nullptr;
static LPDIRECT3DINDEXBUFFER8  s_ib = nullptr;
static int                     s_numVerts = 0;
static int                     s_numIndices = 0;

static LPDIRECT3DTEXTURE8      s_trTex = nullptr;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

// Compute center offset of tunnel at a given base Z (for snake motion)
static void GetSnakeOffset(float baseZ, float t, float& outX, float& outY)
{
    float phase = baseZ * SNAKE_FREQ_Z + t * SNAKE_FREQ_T;

    float sx = sinf(phase);
    float cy = cosf(phase * 0.85f);

    outX = sx * SNAKE_AMP_X;
    outY = cy * SNAKE_AMP_Y;
}

// Fills vertex buffer for current time t
static void FillTunnelVertices(float t)
{
    if (!s_vb || !g_pDevice)
        return;

    TunnelVertex* v = nullptr;
    s_vb->Lock(0, 0, (BYTE**)&v, 0);

    // Global forward motion distance
    float d = t * CAMERA_SPEED;

    for (int iz = 0; iz < TUNEL_SEG_Z; ++iz)
    {
        // Base Z of this ring before motion
        float baseZ = (float)iz * TUNEL_STEP_Z + TUNEL_FRONT_OFFSET;

        // Ring's Z after global forward motion
        float zPos = baseZ - d;

        // Get snake center offset for this ring
        float cx, cy;
        GetSnakeOffset(baseZ, t, cx, cy);

        // Pattern alternates per ring: ring0 W/B, ring1 B/W, etc.
        int ringParity = (iz & 1);

        for (int it = 0; it < TUNEL_SEG_THETA; ++it)
        {
            float theta = (float)it / (float)TUNEL_SEG_THETA *
                (2.0f * D3DX_PI);
            float ca = cosf(theta);
            float sa = sinf(theta);

            int idx = iz * TUNEL_SEG_THETA + it;

            v[idx].x = cx + TUNEL_RADIUS * ca;
            v[idx].y = cy + TUNEL_RADIUS * sa;
            v[idx].z = zPos;

            int colParity = (it & 1) ^ ringParity;

            if (colParity == 0)
            {
                // white tile
                v[idx].color = D3DCOLOR_XRGB(240, 240, 240);
            }
            else
            {
                // black tile
                v[idx].color = D3DCOLOR_XRGB(20, 20, 20);
            }
        }
    }

    s_vb->Unlock();
}

// -----------------------------------------------------------------------------
// Mesh creation (indices are static; vertices are updated per-frame)
// -----------------------------------------------------------------------------

static void CreateTunnelMesh()
{
    s_numVerts = TUNEL_SEG_THETA * TUNEL_SEG_Z;
    s_numIndices = (TUNEL_SEG_Z - 1) * TUNEL_SEG_THETA * 6;

    g_pDevice->CreateVertexBuffer(
        s_numVerts * sizeof(TunnelVertex),
        0,
        FVF_TUNEL,
        D3DPOOL_MANAGED,
        &s_vb);

    g_pDevice->CreateIndexBuffer(
        s_numIndices * sizeof(WORD),
        0,
        D3DFMT_INDEX16,
        D3DPOOL_MANAGED,
        &s_ib);

    // Indices: quads between ring iz and iz+1
    WORD* idx = nullptr;
    s_ib->Lock(0, 0, (BYTE**)&idx, 0);

    int i = 0;
    for (int iz = 0; iz < (TUNEL_SEG_Z - 1); ++iz)
    {
        for (int it = 0; it < TUNEL_SEG_THETA; ++it)
        {
            int itNext = it + 1;
            if (itNext == TUNEL_SEG_THETA)
                itNext = 0;

            WORD v00 = (WORD)(iz * TUNEL_SEG_THETA + it);
            WORD v01 = (WORD)(iz * TUNEL_SEG_THETA + itNext);
            WORD v10 = (WORD)((iz + 1) * TUNEL_SEG_THETA + it);
            WORD v11 = (WORD)((iz + 1) * TUNEL_SEG_THETA + itNext);

            // First tri
            idx[i++] = v00;
            idx[i++] = v10;
            idx[i++] = v11;

            // Second tri
            idx[i++] = v00;
            idx[i++] = v11;
            idx[i++] = v01;
        }
    }

    s_ib->Unlock();

    // Initial fill (t=0)
    FillTunnelVertices(0.0f);
}

// -----------------------------------------------------------------------------
// Init / Shutdown
// -----------------------------------------------------------------------------

void TunelScene_Init()
{
    s_active = true;
    s_startTime = 0.0f;

    if (!g_pDevice)
        return;

    CreateTunnelMesh();

    // Load TR logo texture
    if (FAILED(D3DXCreateTextureFromFileA(g_pDevice, "D:\\tr.dds", &s_trTex)))
    {
        s_trTex = nullptr;
    }
}

void TunelScene_Shutdown()
{
    s_active = false;

    if (s_vb) { s_vb->Release();    s_vb = nullptr; }
    if (s_ib) { s_ib->Release();    s_ib = nullptr; }
    if (s_trTex) { s_trTex->Release(); s_trTex = nullptr; }
}

// -----------------------------------------------------------------------------
// IsFinished (optional, main can ignore this)
// -----------------------------------------------------------------------------

bool TunelScene_IsFinished()
{
    if (s_startTime == 0.0f)
        return false;

    float t = GetTickCount() * 0.001f - s_startTime;
    if (t < 0.0f) t = 0.0f;

    return (t > TUNEL_DURATION);
}

// -----------------------------------------------------------------------------
// Render
// -----------------------------------------------------------------------------

void TunelScene_Render(float demoTime)
{
    if (!s_active || !g_pDevice)
        return;

    if (s_startTime == 0.0f)
        s_startTime = demoTime;

    float t = demoTime - s_startTime;
    if (t < 0.0f) t = 0.0f;

    // Update tunnel vertices for current time (forward motion + snake)
    FillTunnelVertices(t);

    // Camera: fixed at origin, looking straight down +Z
    D3DXMATRIX mView, mProj, mWorld;
    {
        D3DXVECTOR3 eye(0.0f, 0.0f, 0.0f);
        D3DXVECTOR3 at(0.0f, 0.0f, 5.0f);
        D3DXVECTOR3 up(0.0f, 1.0f, 0.0f);

        D3DXMatrixLookAtLH(&mView, &eye, &at, &up);
        D3DXMatrixPerspectiveFovLH(
            &mProj,
            D3DX_PI / 3.0f,
            640.0f / 480.0f,
            0.1f,
            200.0f);
        D3DXMatrixIdentity(&mWorld);

        g_pDevice->SetTransform(D3DTS_VIEW, &mView);
        g_pDevice->SetTransform(D3DTS_PROJECTION, &mProj);
        g_pDevice->SetTransform(D3DTS_WORLD, &mWorld);
    }

    // Base render state
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    g_pDevice->SetTexture(0, NULL);

    // Draw tunnel
    g_pDevice->SetVertexShader(FVF_TUNEL);
    g_pDevice->SetStreamSource(0, s_vb, sizeof(TunnelVertex));
    g_pDevice->SetIndices(s_ib, 0);

    g_pDevice->DrawIndexedPrimitive(
        D3DPT_TRIANGLELIST,
        0, s_numVerts,
        0, s_numIndices / 3);

    // -----------------------------------------------------------------
    // TR logo disc: "light at end of tunnel" sequence
    // -----------------------------------------------------------------
    if (s_trTex && t >= LOGO_REVEAL_START_T)
    {
        // Logo travels along the same base path:
        float logoBaseZ = LOGO_BASE_Z;
        float logoZ = logoBaseZ - t * CAMERA_SPEED;

        // Clamp so it never comes *too* close to the camera
        if (logoZ < 3.0f)
            logoZ = 3.0f;

        // Snake center for the same base Z (so TR sits in the tunnel center)
        float cx, cy;
        GetSnakeOffset(logoBaseZ, t, cx, cy);

        // Progress of the TR sequence, 0..1 over [LOGO_REVEAL_START_T .. LOGO_FULL_ZOOM_T]
        float seqLen = LOGO_FULL_ZOOM_T - LOGO_REVEAL_START_T;
        if (seqLen < 0.1f) seqLen = 0.1f;

        float seqT = (t - LOGO_REVEAL_START_T) / seqLen;
        if (seqT < 0.0f) seqT = 0.0f;
        if (seqT > 1.0f) seqT = 1.0f;

        // Radius grows strongly toward the end of the sequence
        float baseRadius = TUNEL_RADIUS * 1.4f;
        float zoomFactor = 1.0f + 3.0f * seqT * seqT; // ease-in-ish
        float logoRadius = baseRadius * zoomFactor;

        struct LogoV
        {
            float x, y, z;
            DWORD color;
            float u, v;
        };

        LogoV q[4];
        DWORD col = D3DCOLOR_ARGB(255, 255, 255, 255);

        q[0].x = cx - logoRadius; q[0].y = cy + logoRadius; q[0].z = logoZ;
        q[0].color = col;         q[0].u = 0.0f;            q[0].v = 0.0f;

        q[1].x = cx + logoRadius; q[1].y = cy + logoRadius; q[1].z = logoZ;
        q[1].color = col;         q[1].u = 1.0f;            q[1].v = 0.0f;

        q[2].x = cx - logoRadius; q[2].y = cy - logoRadius; q[2].z = logoZ;
        q[2].color = col;         q[2].u = 0.0f;            q[2].v = 1.0f;

        q[3].x = cx + logoRadius; q[3].y = cy - logoRadius; q[3].z = logoZ;
        q[3].color = col;         q[3].u = 1.0f;            q[3].v = 1.0f;

        g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_TRUE);
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

        g_pDevice->SetTexture(0, s_trTex);
        g_pDevice->SetVertexShader(D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1);

        g_pDevice->DrawPrimitiveUP(
            D3DPT_TRIANGLESTRIP,
            2,
            q,
            sizeof(LogoV));
    }
}
