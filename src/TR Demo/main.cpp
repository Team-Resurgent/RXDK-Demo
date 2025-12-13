// main.cpp - RXDK/XDK-style DX8 demo with scene management + fades + controller
//
// Controller:
//   A     = skip to next scene (fade out/in)
//   B     = exit to dashboard (best effort)
//   START = toggle music play/pause

#include <xtl.h>
#include <math.h>
#include <string.h>

#include "input.h"
#include "music.h"

#include "IntroScene.h"
#include "PlasmaScene.h"
#include "RingScene.h"
#include "GalaxyScene.h"
#include "UVRXDKScene.h"
#include "XScene.h"

#include "CubeScene.h"
#include "CityScene.h"
#include "Credits.h"

// -----------------------------------------------------------------------------
// D3D globals
// -----------------------------------------------------------------------------

LPDIRECT3D8        g_pD3D = nullptr;
LPDIRECT3DDEVICE8  g_pDevice = nullptr;

static const float SCREEN_W = 640.0f;
static const float SCREEN_H = 480.0f;

// -----------------------------------------------------------------------------
// Scene / demo state
// -----------------------------------------------------------------------------

enum DemoSceneId
{
    SCENE_INTRO = 0,
    SCENE_PLASMA,
    SCENE_RING,
    SCENE_GALAXY,
    SCENE_UVRXDK,
    SCENE_X,
    SCENE_CUBE,
    SCENE_CREDITS,
    SCENE_CITY,
    SCENE_COUNT
};

struct DemoState
{
    DemoSceneId current;
    DemoSceneId next;

    bool   inTransition;
    int    transitionPhase;     // 0 = fade-out, 1 = fade-in
    DWORD  sceneStartTicks;
    DWORD  transitionStartTicks;

    int    overlayAlpha;        // 0..255
};

static DemoState g_demo = {};

// durations in milliseconds
static const DWORD INTRO_SCENE_MS = 30000;
static const DWORD PLASMA_SCENE_MS = 20000;
static const DWORD RING_SCENE_MS = 20000;
static const DWORD GALAXY_SCENE_MS = 25000;
static const DWORD UVRXDK_SCENE_MS = 22000;
static const DWORD X_SCENE_MS = 25000;
static const DWORD CUBE_SCENE_MS = 22000;
static const DWORD CITY_SCENE_MS = 24000;
static const DWORD CREDITS_SCENE_MS = 35000;

static const DWORD FADE_DURATION_MS = 1000;

// -----------------------------------------------------------------------------
// D3D init / shutdown
// -----------------------------------------------------------------------------

static long InitD3D()
{
    g_pD3D = Direct3DCreate8(D3D_SDK_VERSION);
    if (!g_pD3D)
        return -1;

    D3DPRESENT_PARAMETERS p;
    ZeroMemory(&p, sizeof(p));

    p.BackBufferWidth = (UINT)SCREEN_W;
    p.BackBufferHeight = (UINT)SCREEN_H;
    p.BackBufferFormat = D3DFMT_X8R8G8B8;
    p.BackBufferCount = 1;
    p.SwapEffect = D3DSWAPEFFECT_DISCARD;
    p.Windowed = FALSE;
    p.EnableAutoDepthStencil = FALSE;
    p.FullScreen_RefreshRateInHz = 60;
    p.FullScreen_PresentationInterval = D3DPRESENT_INTERVAL_ONE;

    if (FAILED(g_pD3D->CreateDevice(
        0,
        D3DDEVTYPE_HAL,
        NULL,
        D3DCREATE_HARDWARE_VERTEXPROCESSING,
        &p,
        &g_pDevice)))
    {
        g_pD3D->Release();
        g_pD3D = nullptr;
        return -1;
    }

    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);

    return 0;
}

static void ShutdownD3D()
{
    if (g_pDevice)
    {
        g_pDevice->Release();
        g_pDevice = nullptr;
    }
    if (g_pD3D)
    {
        g_pD3D->Release();
        g_pD3D = nullptr;
    }
}

// -----------------------------------------------------------------------------
// Scene helpers
// -----------------------------------------------------------------------------

static DemoSceneId NextScene(DemoSceneId id)
{
    int n = (int)id + 1;
    if (n >= (int)SCENE_COUNT) n = 0;
    return (DemoSceneId)n;
}

static void InitScene(DemoSceneId id)
{
    switch (id)
    {
    case SCENE_INTRO:   IntroScene_Init();   break;
    case SCENE_PLASMA:  PlasmaScene_Init();  break;
    case SCENE_RING:    RingScene_Init();    break;
    case SCENE_GALAXY:  GalaxyScene_Init();  break;
    case SCENE_UVRXDK:  UVRXDKScene_Init();  break;
    case SCENE_X:       XScene_Init();       break;
    case SCENE_CUBE:    CubeScene_Init();    break;
    case SCENE_CREDITS: Credits_Init();      break;
    case SCENE_CITY:    CityScene_Init();    break;
    default: break;
    }
}

static void ShutdownScene(DemoSceneId id)
{
    switch (id)
    {
    case SCENE_INTRO:   IntroScene_Shutdown();   break;
    case SCENE_PLASMA:  PlasmaScene_Shutdown();  break;
    case SCENE_RING:    RingScene_Shutdown();    break;
    case SCENE_GALAXY:  GalaxyScene_Shutdown();  break;
    case SCENE_UVRXDK:  UVRXDKScene_Shutdown();  break;
    case SCENE_X:       XScene_Shutdown();       break;
    case SCENE_CUBE:    CubeScene_Shutdown();    break;
    case SCENE_CREDITS: Credits_Shutdown();      break;
    case SCENE_CITY:    CityScene_Shutdown();    break;
    default: break;
    }
}

static void RenderScene(DemoSceneId id, float demoTime)
{
    switch (id)
    {
    case SCENE_INTRO:   IntroScene_Render(demoTime);   break;
    case SCENE_PLASMA:  PlasmaScene_Render(demoTime);  break;
    case SCENE_RING:    RingScene_Render(demoTime);    break;
    case SCENE_GALAXY:  GalaxyScene_Render(demoTime);  break;
    case SCENE_UVRXDK:  UVRXDKScene_Render(demoTime);  break;
    case SCENE_X:       XScene_Render(demoTime);       break;
    case SCENE_CUBE:    CubeScene_Render(demoTime);    break;
    case SCENE_CREDITS: Credits_Render(demoTime);      break;
    case SCENE_CITY:    CityScene_Render(demoTime);    break;
    default: break;
    }
}

// -----------------------------------------------------------------------------
// Fade overlay
// -----------------------------------------------------------------------------

struct FadeVertex
{
    float x, y, z, rhw;
    DWORD color;
};

#define FADE_FVF (D3DFVF_XYZRHW | D3DFVF_DIFFUSE)

static void DrawFadeOverlay(int alpha)
{
    if (!g_pDevice || alpha <= 0)
        return;

    if (alpha > 255) alpha = 255;

    FadeVertex v[4];
    const float z = 0.0f;
    const float rhw = 1.0f;
    DWORD col = D3DCOLOR_ARGB((BYTE)alpha, 0, 0, 0);

    v[0] = { 0.0f,     0.0f,     z, rhw, col };
    v[1] = { SCREEN_W, 0.0f,     z, rhw, col };
    v[2] = { 0.0f,     SCREEN_H, z, rhw, col };
    v[3] = { SCREEN_W, SCREEN_H, z, rhw, col };

    g_pDevice->SetVertexShader(FADE_FVF);
    g_pDevice->SetTexture(0, NULL);

    g_pDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    g_pDevice->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_pDevice->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    g_pDevice->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    g_pDevice->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    g_pDevice->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_pDevice->SetRenderState(D3DRS_LIGHTING, FALSE);

    g_pDevice->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(FadeVertex));
}

// -----------------------------------------------------------------------------
// Exit helper
// -----------------------------------------------------------------------------

static void ExitToDashboard()
{
    Music_Shutdown();
    XLaunchNewImage(NULL, NULL);

    while (1)
        Sleep(1000);
}

// -----------------------------------------------------------------------------
// Demo state update
// -----------------------------------------------------------------------------

static void BeginTransitionTo(DemoSceneId nextScene, DWORD nowTicks)
{
    if (g_demo.inTransition)
        return;

    g_demo.inTransition = true;
    g_demo.transitionPhase = 0;
    g_demo.next = nextScene;
    g_demo.transitionStartTicks = nowTicks;
    g_demo.overlayAlpha = 0;
}

static DWORD SceneDurationMs(DemoSceneId id)
{
    switch (id)
    {
    case SCENE_INTRO:   return INTRO_SCENE_MS;
    case SCENE_PLASMA:  return PLASMA_SCENE_MS;
    case SCENE_RING:    return RING_SCENE_MS;
    case SCENE_GALAXY:  return GALAXY_SCENE_MS;
    case SCENE_UVRXDK:  return UVRXDK_SCENE_MS;
    case SCENE_X:       return X_SCENE_MS;
    case SCENE_CUBE:    return CUBE_SCENE_MS;
    case SCENE_CREDITS: return CREDITS_SCENE_MS;
    case SCENE_CITY:    return CITY_SCENE_MS;
    default:            return 20000;
    }
}

static void UpdateDemoState(DWORD nowTicks, bool requestSkip)
{
    if (!g_demo.inTransition)
    {
        if (requestSkip)
        {
            BeginTransitionTo(NextScene(g_demo.current), nowTicks);
            return;
        }

        DWORD sceneElapsed = nowTicks - g_demo.sceneStartTicks;
        DWORD dur = SceneDurationMs(g_demo.current);

        if (sceneElapsed >= dur)
            BeginTransitionTo(NextScene(g_demo.current), nowTicks);

        return;
    }

    DWORD elapsed = nowTicks - g_demo.transitionStartTicks;

    if (g_demo.transitionPhase == 0)
    {
        if (elapsed >= FADE_DURATION_MS)
        {
            g_demo.overlayAlpha = 255;

            ShutdownScene(g_demo.current);
            InitScene(g_demo.next);

            g_demo.current = g_demo.next;
            g_demo.sceneStartTicks = nowTicks;

            g_demo.transitionPhase = 1;
            g_demo.transitionStartTicks = nowTicks;
        }
        else
        {
            g_demo.overlayAlpha = (int)((elapsed * 255U) / FADE_DURATION_MS);
        }
    }
    else
    {
        if (elapsed >= FADE_DURATION_MS)
        {
            g_demo.overlayAlpha = 0;
            g_demo.inTransition = false;
            g_demo.transitionPhase = 0;
        }
        else
        {
            int a = (int)((elapsed * 255U) / FADE_DURATION_MS);
            g_demo.overlayAlpha = 255 - a;
            if (g_demo.overlayAlpha < 0) g_demo.overlayAlpha = 0;
        }
    }
}

// -----------------------------------------------------------------------------
// Frame rendering
// -----------------------------------------------------------------------------

static void RenderFrame(float demoTime)
{
    if (!g_pDevice)
        return;

    g_pDevice->Clear(0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(0, 0, 0), 1.0f, 0);

    g_pDevice->BeginScene();

    RenderScene(g_demo.current, demoTime);
    DrawFadeOverlay(g_demo.overlayAlpha);

    g_pDevice->EndScene();
    g_pDevice->Present(NULL, NULL, NULL, NULL);
}

// -----------------------------------------------------------------------------
// Entry point
// -----------------------------------------------------------------------------

extern "C"
void __cdecl main()
{
    if (InitD3D() < 0)
    {
        while (1) Sleep(1000);
    }

    InitInput();

    Music_Init("D:\\idk.trm");
    Music_Play();
    bool musicPaused = false;

    DWORD startTicks = GetTickCount();

    g_demo.current = SCENE_INTRO;
    g_demo.next = SCENE_PLASMA;
    g_demo.inTransition = false;
    g_demo.transitionPhase = 0;
    g_demo.sceneStartTicks = startTicks;
    g_demo.transitionStartTicks = startTicks;
    g_demo.overlayAlpha = 0;

    InitScene(g_demo.current);

    WORD lastButtons = 0;

    for (;;)
    {
        DWORD now = GetTickCount();
        float demoTime = (now - startTicks) / 1000.0f;

        PumpInput();
        WORD buttons = GetButtons();

        WORD pressed = (WORD)(buttons & (WORD)~lastButtons);
        lastButtons = buttons;

        if (pressed & BTN_B)
            ExitToDashboard();

        if (pressed & BTN_START)
        {
            if (musicPaused) { Music_Play();  musicPaused = false; }
            else { Music_Pause(); musicPaused = true; }
        }

        bool requestSkip = (pressed & BTN_A) != 0;

        Music_Update();

        UpdateDemoState(now, requestSkip);
        RenderFrame(demoTime);

        Sleep(1);
    }
}
