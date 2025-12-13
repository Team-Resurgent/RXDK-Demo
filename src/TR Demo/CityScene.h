#pragma once

// CityScene - Vector city flythrough (wireframe buildings + neon highlights + billboard text)
// RXDK-safe: no per-frame allocs, no RNG in Render, no float->int casts in Render.

void CityScene_Init();
void CityScene_Shutdown();
bool CityScene_IsFinished();
void CityScene_Render(float demoTime);
