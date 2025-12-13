#pragma once
#include <xtl.h>

// Simple streamed WAV (PCM) player using DirectSound.
// idk.trm is treated as a WAV file.
//
// Usage:
//   if (Music_Init("D:\\idk.trm")) Music_Play();
//   each frame: Music_Update();
//   START toggle: Music_Pause() / Music_Play();
//   on exit: Music_Shutdown();

bool Music_Init(const char* path);
void Music_Shutdown();

void Music_Play();     // start/resume
void Music_Pause();    // pause/stop

void Music_Update();   // stream refill (call once per frame)

bool Music_IsReady();
bool Music_IsPlaying();

// -----------------------------------------------------------------------------
// UV Meter levels (0..255 each)
// out[0]=low, out[1]=mid, out[2]=high, out[3]=overall
// Safe: no float->int casts, computed from streamed PCM.
// -----------------------------------------------------------------------------
void Music_GetUVLevels(int out4[4]);
