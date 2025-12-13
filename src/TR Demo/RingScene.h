// RingScene.h - Scene 3: 3D Torus Rings (wireframe, transparent, textured)

#pragma once
#include <xtl.h>

void RingScene_Init();
void RingScene_Shutdown();
void RingScene_Render(float demoTime);
bool RingScene_IsFinished();
