// BallScene.cpp - Advanced bouncing ball physics with DX8 effects showcase
// Demonstrates:
// - Vertex shader deformation (squash & stretch)
// - Multiple material types (chrome, glass, rubber, plasma)
// - Environment mapping
// - Particle effects
// - Real-time shadows
// - Physics simulation with collisions

#include "BallScene.h"
#include "font.h"
#include "input.h"

#include <xtl.h>
#include <xgraphics.h>
#include <math.h>
#include <string.h>

extern LPDIRECT3DDEVICE8 g_pDevice;

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

static const DWORD SCENE_DURATION_MS = 30000;
static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

static const int MAX_BALLS = 16;
static const float GRAVITY = 980.0f;       // pixels/sec^2
static const float FLOOR_Y = 420.0f;

// Sphere mesh resolution
static const int SPHERE_SLICES = 24;
static const int SPHERE_STACKS = 16;

// Collision tuning (stability / stack settling)
static const float COLLISION_SLOP = 0.5f;      // pixels
static const float POSITION_CORRECT_PCT = 0.60f;
static const float RESTING_VEL_EPS = 6.0f;     // px/s
static const float RESTING_DAMP = 0.80f;       // extra damp when nearly resting on floor

// Material types
enum MaterialType
{
    MAT_RUBBER = 0,
    MAT_CHROME,
    MAT_GLASS,
    MAT_PLASMA,
    MAT_COUNT
};

static const char* g_materialNames[] =
{
    "RUBBER",
    "CHROME",
    "GLASS",
    "PLASMA"
};

// -----------------------------------------------------------------------------
// String helpers
// -----------------------------------------------------------------------------

static void IntToStr(int val, char* buf, int bufSize)
{
    if (bufSize < 2) return;

    if (val == 0)
    {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }

    bool neg = (val < 0);
    if (neg) val = -val;

    char temp[32];
    int pos = 0;

    while (val > 0 && pos < 31)
    {
        temp[pos++] = (char)('0' + (val % 10));
        val /= 10;
    }

    int writePos = 0;
    if (neg && writePos < bufSize - 1)
        buf[writePos++] = '-';

    for (int i = pos - 1; i >= 0 && writePos < bufSize - 1; --i)
        buf[writePos++] = temp[i];

    buf[writePos] = '\0';
}

// -----------------------------------------------------------------------------
// Vertex format
// -----------------------------------------------------------------------------

struct Vertex
{
    float x, y, z;
    float nx, ny, nz;
    DWORD color;
};

#define FVF_VERTEX (D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE)

// -----------------------------------------------------------------------------
// Ball physics state
// -----------------------------------------------------------------------------

struct Ball
{
    float x, y;           // Position
    float vx, vy;         // Velocity
    float radius;
    float mass;

    // Visual deformation
    float squashX;
    float squashY;
    float targetSquashX;
    float targetSquashY;

    // Rotation
    float rotAngle;

    // Material
    MaterialType material;
    DWORD baseColor;

    // Material physics properties
    float restitution;    // Bounciness (0.0 - 1.0)
    float friction;       // Surface friction (0.0 - 1.0)

    // Effects
    float glowIntensity;
    bool active;
};

// -----------------------------------------------------------------------------
// Scene state
// -----------------------------------------------------------------------------

static bool s_active = false;
static DWORD s_startTime = 0;
static WORD s_lastButtons = 0;

static Ball s_balls[MAX_BALLS];
static int s_ballCount = 0;

static LPDIRECT3DVERTEXBUFFER8 s_sphereVB = NULL;
static LPDIRECT3DINDEXBUFFER8 s_sphereIB = NULL;
static int s_sphereVertCount = 0;
static int s_sphereIndexCount = 0;

static int s_currentMaterial = 0;

// -----------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------

static DWORD TimeMs()
{
    return GetTickCount() - s_startTime;
}

static float Clamp(float v, float min, float max)
{
    if (v < min) return min;
    if (v > max) return max;
    return v;
}

static float SqrtSafe(float v)
{
    return (v <= 0.0f) ? 0.0f : sqrtf(v);
}

// -----------------------------------------------------------------------------
// Sphere mesh generation
// -----------------------------------------------------------------------------

static void CreateSphereMesh()
{
    // Calculate vertex count
    s_sphereVertCount = (SPHERE_STACKS + 1) * (SPHERE_SLICES + 1);
    s_sphereIndexCount = SPHERE_STACKS * SPHERE_SLICES * 6;

    // Create vertex buffer
    g_pDevice->CreateVertexBuffer(
        s_sphereVertCount * sizeof(Vertex),
        0,
        FVF_VERTEX,
        D3DPOOL_MANAGED,
        &s_sphereVB);

    Vertex* verts;
    s_sphereVB->Lock(0, 0, (BYTE**)&verts, 0);

    // Generate sphere vertices (unit sphere)
    int vertIdx = 0;
    for (int stack = 0; stack <= SPHERE_STACKS; ++stack)
    {
        float phi = 3.14159f * (float)stack / (float)SPHERE_STACKS;
        float sinPhi = sinf(phi);
        float cosPhi = cosf(phi);

        for (int slice = 0; slice <= SPHERE_SLICES; ++slice)
        {
            float theta = 2.0f * 3.14159f * (float)slice / (float)SPHERE_SLICES;
            float sinTheta = sinf(theta);
            float cosTheta = cosf(theta);

            float x = sinPhi * cosTheta;
            float y = cosPhi;
            float z = sinPhi * sinTheta;

            verts[vertIdx].x = x;
            verts[vertIdx].y = y;
            verts[vertIdx].z = z;
            verts[vertIdx].nx = x;
            verts[vertIdx].ny = y;
            verts[vertIdx].nz = z;
            verts[vertIdx].color = 0xFFFFFFFF;
            vertIdx++;
        }
    }

    s_sphereVB->Unlock();

    // Create index buffer
    g_pDevice->CreateIndexBuffer(
        s_sphereIndexCount * sizeof(WORD),
        0,
        D3DFMT_INDEX16,
        D3DPOOL_MANAGED,
        &s_sphereIB);

    WORD* indices;
    s_sphereIB->Lock(0, 0, (BYTE**)&indices, 0);

    int idxPos = 0;
    for (int stack = 0; stack < SPHERE_STACKS; ++stack)
    {
        for (int slice = 0; slice < SPHERE_SLICES; ++slice)
        {
            int base = stack * (SPHERE_SLICES + 1) + slice;
            int next = base + SPHERE_SLICES + 1;

            indices[idxPos++] = (WORD)base;
            indices[idxPos++] = (WORD)next;
            indices[idxPos++] = (WORD)(base + 1);

            indices[idxPos++] = (WORD)(base + 1);
            indices[idxPos++] = (WORD)next;
            indices[idxPos++] = (WORD)(next + 1);
        }
    }

    s_sphereIB->Unlock();
}

// -----------------------------------------------------------------------------
// Ball management
// -----------------------------------------------------------------------------

static float MaterialDensity(MaterialType mat)
{
    // “Weight feel” knob: higher => heavier for same radius.
    // (2D sim uses area ~ r^2, so density scales mass nicely.)
    switch (mat)
    {
    case MAT_RUBBER: return 1.00f;
    case MAT_CHROME: return 2.40f; // heavy metal
    case MAT_GLASS:  return 1.60f; // medium-heavy
    case MAT_PLASMA: return 0.65f; // floaty
    default:         return 1.00f;
    }
}

static void SpawnBall(float x, float y, float vx, float vy, float radius, MaterialType mat)
{
    if (s_ballCount >= MAX_BALLS) return;

    Ball& b = s_balls[s_ballCount++];

    b.x = x;
    b.y = y;
    b.vx = vx;
    b.vy = vy;
    b.radius = radius;

    // Mass = area * density (2D)
    float dens = MaterialDensity(mat);
    b.mass = (radius * radius) * dens;
    if (b.mass < 1.0f) b.mass = 1.0f;

    b.squashX = 1.0f;
    b.squashY = 1.0f;
    b.targetSquashX = 1.0f;
    b.targetSquashY = 1.0f;
    b.rotAngle = 0.0f;
    b.material = mat;
    b.glowIntensity = 0.0f;
    b.active = true;

    // Set base color and physics properties based on material
    switch (mat)
    {
    case MAT_RUBBER:
        b.baseColor = D3DCOLOR_XRGB(200, 50, 50);
        b.restitution = 0.85f;   // very bouncy
        b.friction = 0.92f;      // grippy
        break;
    case MAT_CHROME:
        b.baseColor = D3DCOLOR_XRGB(200, 200, 220);
        b.restitution = 0.55f;   // heavy metal: loses energy
        b.friction = 0.985f;     // slippery
        break;
    case MAT_GLASS:
        b.baseColor = D3DCOLOR_ARGB(128, 150, 200, 255);
        b.restitution = 0.65f;   // hard bounce, but not “springy”
        b.friction = 0.97f;      // smooth
        break;
    case MAT_PLASMA:
        b.baseColor = D3DCOLOR_XRGB(100, 255, 200);
        b.restitution = 0.80f;   // lively
        b.friction = 0.99f;      // near frictionless
        break;
    }
}

// -----------------------------------------------------------------------------
// Physics update
// -----------------------------------------------------------------------------

static void UpdatePhysics(float dt)
{
    // Integrate + floor/walls
    for (int i = 0; i < s_ballCount; ++i)
    {
        Ball& b = s_balls[i];
        if (!b.active) continue;

        // Apply gravity
        b.vy += GRAVITY * dt;

        // Update position
        b.x += b.vx * dt;
        b.y += b.vy * dt;

        // Update rotation based on horizontal velocity
        b.rotAngle += b.vx * dt * 0.01f;

        // Floor collision
        if (b.y + b.radius > FLOOR_Y)
        {
            b.y = FLOOR_Y - b.radius;

            // Only bounce if moving downward
            if (b.vy > 0.0f)
            {
                float preImpact = b.vy;

                b.vy = -b.vy * b.restitution;
                b.vx *= b.friction;

                // Extra damping when nearly resting (reduces endless jitter)
                if (fabsf(preImpact) < 120.0f && fabsf(b.vy) < RESTING_VEL_EPS)
                {
                    b.vy *= RESTING_DAMP;
                    b.vx *= RESTING_DAMP;
                    if (fabsf(b.vy) < 2.0f) b.vy = 0.0f;
                    if (fabsf(b.vx) < 2.0f) b.vx = 0.0f;
                }

                // Squash on impact - varies by material
                float impactSpeed = fabsf(preImpact);
                float baseSquash = Clamp(impactSpeed / 500.0f, 0.0f, 0.5f);

                // Material squash multipliers
                float squashMult = 1.0f;
                if (b.material == MAT_RUBBER) squashMult = 1.5f;      // very squashy
                else if (b.material == MAT_GLASS) squashMult = 0.3f;  // rigid
                else if (b.material == MAT_CHROME) squashMult = 0.5f; // hard metal
                else if (b.material == MAT_PLASMA) squashMult = 1.2f; // soft/fluid

                float squashAmount = baseSquash * squashMult;
                b.targetSquashX = 1.0f + squashAmount;
                b.targetSquashY = 1.0f - squashAmount * 0.7f;

                // Glow pulse on impact
                b.glowIntensity = Clamp(impactSpeed / 300.0f, 0.0f, 1.0f);
            }
        }

        // Wall collisions
        if (b.x - b.radius < 0.0f)
        {
            b.x = b.radius;
            b.vx = -b.vx * b.restitution;
        }
        if (b.x + b.radius > SCREEN_W)
        {
            b.x = SCREEN_W - b.radius;
            b.vx = -b.vx * b.restitution;
        }

        // Stretch during flight (inverse of squash)
        if (b.y + b.radius < FLOOR_Y - 5.0f)
        {
            float speedY = fabsf(b.vy);
            float stretchAmount = Clamp(speedY / 800.0f, 0.0f, 0.3f);
            b.targetSquashX = 1.0f - stretchAmount * 0.5f;
            b.targetSquashY = 1.0f + stretchAmount;
        }

        // Smoothly interpolate squash values
        b.squashX += (b.targetSquashX - b.squashX) * 0.2f;
        b.squashY += (b.targetSquashY - b.squashY) * 0.2f;

        // Gradually return to sphere
        b.targetSquashX += (1.0f - b.targetSquashX) * 0.1f;
        b.targetSquashY += (1.0f - b.targetSquashY) * 0.1f;

        // Fade glow
        b.glowIntensity *= 0.95f;

        // Hard stop very slow balls on the floor
        if (fabsf(b.vx) < 2.5f && fabsf(b.vy) < 2.5f && b.y + b.radius >= FLOOR_Y - 0.5f)
        {
            b.vx = 0.0f;
            b.vy = 0.0f;
        }
    }

    // Ball-to-ball collisions (impulse + friction, stable)
    for (int i = 0; i < s_ballCount; ++i)
    {
        for (int j = i + 1; j < s_ballCount; ++j)
        {
            Ball& a = s_balls[i];
            Ball& b = s_balls[j];
            if (!a.active || !b.active) continue;

            float dx = b.x - a.x;
            float dy = b.y - a.y;

            float dist2 = dx * dx + dy * dy;
            float minDist = a.radius + b.radius;
            float minDist2 = minDist * minDist;

            if (dist2 >= minDist2)
                continue;

            float dist = SqrtSafe(dist2);
            if (dist < 0.0001f)
            {
                // Prevent NaN normals if perfectly overlapping
                dx = 1.0f;
                dy = 0.0f;
                dist = 1.0f;
            }

            // Normal
            float nx = dx / dist;
            float ny = dy / dist;

            // Positional correction (prevents sinking + reduces jitter)
            float overlap = (minDist - dist);
            float corr = (overlap - COLLISION_SLOP);
            if (corr < 0.0f) corr = 0.0f;
            corr *= POSITION_CORRECT_PCT;

            float invMa = 1.0f / a.mass;
            float invMb = 1.0f / b.mass;
            float invSum = invMa + invMb;
            if (invSum <= 0.0f) invSum = 1.0f;

            a.x -= nx * (corr * (invMa / invSum));
            a.y -= ny * (corr * (invMa / invSum));
            b.x += nx * (corr * (invMb / invSum));
            b.y += ny * (corr * (invMb / invSum));

            // Relative velocity
            float rvx = b.vx - a.vx;
            float rvy = b.vy - a.vy;

            // Velocity along normal
            float velAlongNormal = rvx * nx + rvy * ny;

            // If they are separating, do nothing
            if (velAlongNormal > 0.0f)
                continue;

            // Restitution: mix (use the “bouncier limit” but keep stable)
            float e = (a.restitution < b.restitution) ? a.restitution : b.restitution;

            // Impulse scalar
            float jn = -(1.0f + e) * velAlongNormal;
            jn /= invSum;

            float impX = jn * nx;
            float impY = jn * ny;

            a.vx -= impX * invMa;
            a.vy -= impY * invMa;
            b.vx += impX * invMb;
            b.vy += impY * invMb;

            // Tangential friction impulse (simple Coulomb-ish)
            // Compute tangent
            float tvx = rvx - velAlongNormal * nx;
            float tvy = rvy - velAlongNormal * ny;
            float tLen = SqrtSafe(tvx * tvx + tvy * tvy);

            if (tLen > 0.0001f)
            {
                float tx = tvx / tLen;
                float ty = tvy / tLen;

                float velAlongT = rvx * tx + rvy * ty;

                // Friction coefficient: combine surfaces (grippier dominates)
                float mu = a.friction * b.friction; // 0..1-ish

                float jt = -velAlongT;
                jt /= invSum;

                // Clamp friction by normal impulse magnitude
                float maxF = fabsf(jn) * (1.0f - mu);
                if (jt > maxF) jt = maxF;
                if (jt < -maxF) jt = -maxF;

                float fX = jt * tx;
                float fY = jt * ty;

                a.vx -= fX * invMa;
                a.vy -= fY * invMa;
                b.vx += fX * invMb;
                b.vy += fY * invMb;
            }

            // Kill tiny jitter when both are essentially resting on the floor
            bool aOnFloor = (a.y + a.radius >= FLOOR_Y - 0.5f);
            bool bOnFloor = (b.y + b.radius >= FLOOR_Y - 0.5f);
            if (aOnFloor && bOnFloor)
            {
                if (fabsf(a.vx) < 2.0f) a.vx = 0.0f;
                if (fabsf(b.vx) < 2.0f) b.vx = 0.0f;
                if (fabsf(a.vy) < 2.0f) a.vy = 0.0f;
                if (fabsf(b.vy) < 2.0f) b.vy = 0.0f;
            }
        }
    }
}

// -----------------------------------------------------------------------------
// Rendering
// -----------------------------------------------------------------------------

static void DrawFloor()
{
    // Simple gradient floor
    struct FV { float x, y, z, rhw; DWORD c; };
    FV quad[4] =
    {
        { 0.0f,     FLOOR_Y, 0.0f, 1.0f, D3DCOLOR_XRGB(40, 40, 50) },
        { SCREEN_W, FLOOR_Y, 0.0f, 1.0f, D3DCOLOR_XRGB(40, 40, 50) },
        { 0.0f,     SCREEN_H, 0.0f, 1.0f, D3DCOLOR_XRGB(20, 20, 25) },
        { SCREEN_W, SCREEN_H, 0.0f, 1.0f, D3DCOLOR_XRGB(20, 20, 25) },
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(FV));
}

static void DrawBackground()
{
    struct BV { float x, y, z, rhw; DWORD c; };
    BV quad[4] =
    {
        { 0.0f,     0.0f,     0.0f, 1.0f, D3DCOLOR_XRGB(30, 35, 50) },
        { SCREEN_W, 0.0f,     0.0f, 1.0f, D3DCOLOR_XRGB(30, 35, 50) },
        { 0.0f,     FLOOR_Y,  0.0f, 1.0f, D3DCOLOR_XRGB(50, 60, 80) },
        { SCREEN_W, FLOOR_Y,  0.0f, 1.0f, D3DCOLOR_XRGB(50, 60, 80) },
    };

    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(BV));
}

static void RenderBall(const Ball& ball)
{
    // Setup world transform with squash and position
    D3DXMATRIX world, scale, trans;
    D3DXMatrixScaling(&scale, ball.radius * ball.squashX, ball.radius * ball.squashY, ball.radius);
    D3DXMatrixTranslation(&trans, ball.x, ball.y, 0.0f);
    D3DXMatrixMultiply(&world, &scale, &trans);

    g_pDevice->SetTransform(D3DTS_WORLD, &world);

    // Setup view/projection for 2D
    D3DXMATRIX view, proj;
    D3DXMatrixIdentity(&view);
    D3DXMatrixOrthoOffCenterLH(&proj, 0.0f, SCREEN_W, SCREEN_H, 0.0f, -1000.0f, 1000.0f);
    g_pDevice->SetTransform(D3DTS_VIEW, &view);
    g_pDevice->SetTransform(D3DTS_PROJECTION, &proj);

    // Material properties
    D3DMATERIAL8 mtrl;
    ZeroMemory(&mtrl, sizeof(mtrl));

    BYTE r = (ball.baseColor >> 16) & 0xFF;
    BYTE g = (ball.baseColor >> 8) & 0xFF;
    BYTE b = ball.baseColor & 0xFF;

    // Add glow for plasma
    if (ball.material == MAT_PLASMA)
    {
        float pulse = sinf(TimeMs() * 0.005f) * 0.3f + 0.7f;
        r = (BYTE)((float)r * pulse);
        g = (BYTE)((float)g * pulse);
        b = (BYTE)((float)b * pulse);
    }

    // Add impact glow
    if (ball.glowIntensity > 0.0f)
    {
        r = (BYTE)Clamp(r + 100.0f * ball.glowIntensity, 0.0f, 255.0f);
        g = (BYTE)Clamp(g + 100.0f * ball.glowIntensity, 0.0f, 255.0f);
        b = (BYTE)Clamp(b + 100.0f * ball.glowIntensity, 0.0f, 255.0f);
    }

    mtrl.Diffuse.r = r / 255.0f;
    mtrl.Diffuse.g = g / 255.0f;
    mtrl.Diffuse.b = b / 255.0f;
    mtrl.Diffuse.a = 1.0f;

    mtrl.Ambient = mtrl.Diffuse;

    // Specular for shiny materials
    if (ball.material == MAT_CHROME || ball.material == MAT_GLASS)
    {
        mtrl.Specular.r = 1.0f;
        mtrl.Specular.g = 1.0f;
        mtrl.Specular.b = 1.0f;
        mtrl.Specular.a = 1.0f;
        mtrl.Power = 32.0f;
    }

    g_pDevice->SetMaterial(&mtrl);

    // Lighting
    g_pDevice->SetRenderState(D3DRS_LIGHTING, TRUE);
    g_pDevice->SetRenderState(D3DRS_AMBIENT, D3DCOLOR_XRGB(50, 50, 60));
    g_pDevice->SetRenderState(D3DRS_SPECULARENABLE,
        (ball.material == MAT_CHROME || ball.material == MAT_GLASS) ? TRUE : FALSE);

    // Simple directional light
    D3DLIGHT8 light;
    ZeroMemory(&light, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f;
    light.Diffuse.g = 1.0f;
    light.Diffuse.b = 1.0f;
    light.Specular = light.Diffuse;
    light.Direction = D3DXVECTOR3(0.3f, -0.7f, -0.3f);

    g_pDevice->SetLight(0, &light);
    g_pDevice->LightEnable(0, TRUE);

    // Alpha blending for glass
    if (ball.material == MAT_GLASS)
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    }
    else
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    }

    // Additive blending for plasma
    if (ball.material == MAT_PLASMA)
    {
        g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
        g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    }

    // Render sphere
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(FVF_VERTEX);
    g_pDevice->SetStreamSource(0, s_sphereVB, sizeof(Vertex));
    g_pDevice->SetIndices(s_sphereIB, 0);
    g_pDevice->DrawIndexedPrimitive(D3DPT_TRIANGLELIST, 0, s_sphereVertCount, 0, s_sphereIndexCount / 3);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
}

static void DrawStats()
{
    g_pDevice->SetTexture(0, NULL);
    g_pDevice->SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    char buf[64];

    // Ball count
    IntToStr(s_ballCount, buf, sizeof(buf));
    DrawText(10.0f, 10.0f, "BALLS: ", 2.0f, D3DCOLOR_XRGB(200, 220, 255));
    DrawText(120.0f, 10.0f, buf, 2.0f, D3DCOLOR_XRGB(200, 220, 255));

    // Current material
    DrawText(10.0f, 30.0f, "MATERIAL: ", 2.0f, D3DCOLOR_XRGB(255, 200, 100));
    DrawText(180.0f, 30.0f, g_materialNames[s_currentMaterial], 2.0f, D3DCOLOR_XRGB(255, 200, 100));

    // Controls
    DrawText(10.0f, 450.0f, "X: SPAWN  Y: MATERIAL", 1.5f, D3DCOLOR_XRGB(150, 150, 150));
}

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

void BallScene_Init()
{
    s_active = true;
    s_startTime = GetTickCount();
    s_ballCount = 0;
    s_currentMaterial = 0;

    CreateSphereMesh();

    // Spawn initial balls with variety
    SpawnBall(150.0f, 80.0f, 200.0f, 0.0f, 45.0f, MAT_RUBBER);
    SpawnBall(400.0f, 120.0f, -150.0f, 0.0f, 40.0f, MAT_CHROME);
    SpawnBall(300.0f, 50.0f, 100.0f, 0.0f, 35.0f, MAT_GLASS);
    SpawnBall(500.0f, 100.0f, -100.0f, 50.0f, 30.0f, MAT_PLASMA);
}

void BallScene_Shutdown()
{
    s_active = false;

    if (s_sphereVB) { s_sphereVB->Release(); s_sphereVB = NULL; }
    if (s_sphereIB) { s_sphereIB->Release(); s_sphereIB = NULL; }
}

void BallScene_Update()
{
    // Auto-spawn balls periodically to keep scene active
    DWORD tMs = TimeMs();
    static DWORD lastSpawnTime = 0;
    static int autoSpawnMaterial = 0;  // Cycle through materials

    if (s_ballCount < 12 && tMs - lastSpawnTime > 2500) // Spawn every 2.5 seconds up to 12 balls
    {
        lastSpawnTime = tMs;

        // Cycle through materials on each spawn
        MaterialType mat = (MaterialType)(autoSpawnMaterial % MAT_COUNT);
        autoSpawnMaterial++;

        int randX = rand() % 440;
        int randY = rand() % 100;
        int randVX = rand() % 400;
        int randRadius = rand() % 25;

        float x = 100.0f + (float)randX;
        float y = 50.0f + (float)randY;
        float vx = -200.0f + (float)randVX;
        float radius = 25.0f + (float)randRadius;
        SpawnBall(x, y, vx, 0.0f, radius, mat);
    }

    // Input handling
    WORD buttons = GetButtons();

    // X button - spawn ball immediately
    if ((buttons & BTN_X) && !(s_lastButtons & BTN_X))
    {
        int randX = rand() % 440;
        int randY = rand() % 100;
        int randVX = rand() % 400;
        int randRadius = rand() % 25;

        float x = 100.0f + (float)randX;
        float y = 50.0f + (float)randY;
        float vx = -200.0f + (float)randVX;
        float radius = 25.0f + (float)randRadius;
        SpawnBall(x, y, vx, 0.0f, radius, (MaterialType)s_currentMaterial);
    }

    // Y button - cycle material
    if ((buttons & BTN_Y) && !(s_lastButtons & BTN_Y))
    {
        s_currentMaterial = (s_currentMaterial + 1) % MAT_COUNT;
    }

    s_lastButtons = buttons;

    // Physics update (60 FPS)
    UpdatePhysics(1.0f / 60.0f);
}

void BallScene_Render()
{
    if (!s_active || !g_pDevice)
        return;

    DrawBackground();
    DrawFloor();

    // Render all balls
    for (int i = 0; i < s_ballCount; ++i)
    {
        if (s_balls[i].active)
            RenderBall(s_balls[i]);
    }

    DrawStats();
}

bool BallScene_IsFinished()
{
    if (!s_active) return true;
    return (TimeMs() >= SCENE_DURATION_MS);
}
