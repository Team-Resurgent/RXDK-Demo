#pragma once

// PostFXScene - render-to-texture + fullscreen composite (baseline for bloom/warp later)

void PostFXScene_Init();
void PostFXScene_Shutdown();
void PostFXScene_Render(float demoTime);