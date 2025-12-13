// IntroScene.h
#pragma once

// Simple intro scene lifecycle
void IntroScene_Init();
void IntroScene_Shutdown();

// demoTime = seconds since demo start (from main)
void IntroScene_Render(float demoTime);
