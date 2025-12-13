# RXDK Demo – Visual Test App

A lightweight RXDK-based demo application for the original Xbox, focused on
rendering visuals and testing input-driven UI logic without relying on the
official Microsoft XDK.

This project is intended for experimentation, prototyping, and learning within
the RXDK environment.

## Features

- RXDK-compatible Xbox executable
- Custom rendering pipeline using `xgraphics`
- Button-mapped actions (no mouse / no virtual cursors)
- Designed for deterministic rendering (no dynamic allocations per frame)

 ## RDXK Demos

- IntroScene - Opening title/logo sequence
- PlasmaScene - Fullscreen animated plasma effect with swirling color patterns and camera drift
- BallScene - Physics demo with bouncing balls featuring squash/stretch deformation and multiple materials (rubber, chrome, glass, plasma)
- RingScene - Animated ring/torus geometry effects
- GalaxyScene - Procedural space scene with stars, nebulae, and dust particles with stats overlay
- UVRXDKScene - Large wireframe "RXDK" letters with music-reactive VU meter fills
- XScene - Xbox logo visualization
- CubeScene - Spinning cube with Matrix-style "RXDK" text rain on all 6 faces
- CityScene - Animated cityscape/skyline scene
- DripScene - Interactive water ripple simulation with multi-layer shading, rain mode, and realistic physics
- Credits - Star Wars-style scrolling credits with perspective and color-coded shoutouts

## Constraints / Design Notes

- No reliance on Microsoft XDK headers or libraries
- Avoids unsupported symbols (e.g. `__ftol2_sse`)
- No per-frame heap allocation
- No RNG usage in render paths
- Z-buffer disabled for predictable visuals
- Built to run cleanly on real hardware and emulators

## Build Requirements

- RXDK toolchain
- Compatible C++ compiler for RXDK
- Standard RXDK libraries (`xtl`, `xgraphics`, etc.)

## Usage

- Build the project using RXDK
- Deploy the resulting XBE to an original Xbox or compatible emulator
- Navigate menus using a standard Xbox controller

## Purpose

This demo serves as a foundation for:
- Visual experiments
- Input handling
- Future expansion into full applications

It is **not** intended to be a full engine or production-ready framework.

---

By: Darkone83 x Team Resurgent
