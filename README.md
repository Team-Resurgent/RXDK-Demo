# RXDK Demo – Visual Test App

A lightweight RXDK-based demo application for the original Xbox, focused on
rendering visuals and testing input-driven UI logic without relying on the
official Microsoft XDK.

This project is intended for experimentation, prototyping, and learning within
the RXDK environment.



## Features

- Custom rendering pipeline using `xgraphics`
- Button-mapped actions (no mouse / no virtual cursors)
- Designed for deterministic rendering (no dynamic allocations per frame)

 ## RXDK Demos

- IntroScene - Opening title/logo sequence (new)
- ChromeScene - A DXT1/DXT5 bump mapping sample with diffuse lighting and UV ripple
- PlasmaScene - Fullscreen animated plasma effect with swirling color patterns and camera drift
- BallScene - Physics demo with bouncing balls featuring squash/stretch deformation and multiple materials (rubber, chrome, glass, plasma) [New Look]
- RingScene - Animated ring/torus geometry effects
- GlassScene - Stained glass-inspired scene showing off CPU-based vertex shading and DOT3 PS shading.
- GalaxyScene - Procedural space scene with stars, nebulae, and dust particles with stats overlay
- CrystalScene -  GPU-intensive cave grotto filled with luminous hex crystal formations, rendered with multi-pass DOT3 bump mapping and cubemap reflections.
- VURXDKScene - Large wireframe "RXDK" letters with music-reactive VU meter fills
- XScene - Xbox logo visualization
- CubeScene - Spinning cube with Matrix-style "RXDK" text rain on all 6 faces
- CuveEnvScene - A spinning cube with environment  mapping and  with an Xbox-themed space background
- DripScene - Interactive water ripple simulation with multi-layer shading, rain mode, and realistic physics
- MazeScene - A cel-shaded Windows 95-like maze
- PostFXScene - Various PostFX samples such as lenswarping, vignette, heat warping, and a few other techniques to appear as a black hole 
- Credits - Star Wars-style scrolling credits with perspective and color-coded shoutouts
- CityScene - Animated cityscape/skyline scene

## Constraints / Design Notes

- No per-frame heap allocation
- No RNG usage in render paths
- Z-buffer disabled for predictable visuals
- Built to run cleanly on real hardware and emulators

## Build Requirements

- RXDK toolchain
- Compatible C++ compiler for RXDK
- Standard RXDK libraries (`xtl`, `xgraphics`, `xboxkrnl`, `libcmtd`, `xapilibd`, `d3d8d`, `d3dx8d`, `xapilib`, `d3d8`, `d3dx8`, `xgraphicsd`, `dsound`)

## Usage

- Build the project using RXDK
- Deploy the resulting XBE to an original Xbox or compatible emulator
- Navigate using a standard Xbox controller

## Controls

### Global
- A: Skip Scene
- B: Exit to Dashboard
- Start: Play / Pause Music

### Menu
- Back: Enters / Exits menu
- D-Pad: navigation
- A: Selects item
- B: Back

### Per Scene:
#### BallScene
- X: Spawn
- Y: Material Change

#### DripScene
- Y: Enable / Disable rain effect

## Purpose

This demo serves as a foundation for:
- Visual experiments
- Input handling
- Future expansion into full applications

It is **not** intended to be a full engine or production-ready framework.

---

By: Darkone83 x Team Resurgent
