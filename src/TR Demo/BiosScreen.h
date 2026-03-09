#pragma once

// BiosScreen.h - Mock late-2000s BIOS splash screen
//
// Call BiosScreen_Run() from main() after the D3D settle frames,
// before InitInput(). Blocks for ~6 seconds then returns cleanly.
// Completely self-contained - no shared state with the demo.

void BiosScreen_Run();