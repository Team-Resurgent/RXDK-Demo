#pragma once
#include <xtl.h>

// -----------------------------------------------------------------------------
// Menu overlay system for RXDK demoscene
//
// Usage in main.cpp:
//   #include "Menu.h"
//
//   Menu_Init(SCENE_COUNT);                  // pass total scene count once
//
//   // Inside main loop (after PumpInput / GetButtons):
//   if (pressed & BTN_BACK)
//       Menu_Toggle();
//
//   Menu_Update(now, pressed);               // drives state machine + input
//
//   // After RenderScene, before Present:
//   if (Menu_IsOpen())
//       Menu_Render();
//
//   // Handle scene jump request:
//   int jump = Menu_GetRequestedScene();
//   if (jump >= 0)
//       BeginTransitionTo((DemoSceneId)jump, now);
//
//   // In NextScene() / skip logic:
//   if (!Menu_IsSceneEnabled(candidateId))
//       skip it;
// -----------------------------------------------------------------------------

// Call once at startup. sceneCount = SCENE_COUNT from main.cpp.
void Menu_Init(int sceneCount);

// Toggle menu open/closed (call on BTN_BACK edge).
void Menu_Toggle();

// Drive the menu state machine. Pass current tick and the freshly-computed
// "pressed this frame" button mask. Menu consumes input when open.
// Returns true if the menu consumed the input (caller may skip its own handling).
bool Menu_Update(DWORD nowTicks, WORD pressed);

// Draw the menu overlay. Call AFTER RenderScene, BEFORE DrawFadeOverlay/Present.
// No-op when menu is fully closed.
void Menu_Render();

// True while the menu is open or animating (fade in/out).
bool Menu_IsOpen();

// Returns a DemoSceneId to jump to, or -1 if no jump is pending.
// Resets to -1 after being read once.
int Menu_GetRequestedScene();

// Query per-scene enable flag. Used by NextScene() to skip disabled scenes.
bool Menu_IsSceneEnabled(int sceneId);

// Query music mute state so main.cpp can sync its musicPaused flag if needed.
bool Menu_IsMusicMuted();