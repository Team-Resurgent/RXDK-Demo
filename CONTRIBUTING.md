# Contributing

Thanks for your interest in contributing to this RXDK demo project.

This repository is intended to remain **simple, readable, and RXDK-safe**.
Contributions should align with that goal.

## What This Project Is

- A lightweight RXDK demo / reference application
- Focused on rendering, input handling, and experimentation
- Designed to run on real Xbox hardware and emulators
- Not a full engine or framework

## Contribution Guidelines

### General Rules

- Keep changes **small and focused**
- Avoid unnecessary abstraction
- Prefer clarity over cleverness
- No per-frame heap allocations
- No reliance on unsupported symbols or Microsoft XDK components

### Code Style

- C / C++ compatible with RXDK
- No STL usage unless already present
- Avoid dynamic memory allocation in render paths
- Avoid RNG usage in render loops
- Keep rendering deterministic where possible
- Preserve existing comments and annotations

### Rendering & Performance

- Do not introduce performance-heavy effects
- Avoid excessive draw calls
- Avoid grid-heavy or fill-rate-heavy visuals unless justified
- Test with real hardware constraints in mind

### API / Behavior Changes

If a change:
- Alters input behavior
- Changes rendering assumptions
- Modifies menu flow
- Affects config or data formats (e.g. JSON)

Please explain **why** the change is needed in the commit message or PR description.

## What Not to Submit

- Large refactors without discussion
- Style-only changes
- Feature creep
- Dependencies that break RXDK compatibility
- Code that assumes newer Xbox SDKs

## Submitting Changes

- Fork the repository
- Create a clearly named branch
- Submit a pull request with a short, clear description
- Include screenshots or video when visual output changes

## Licensing

By contributing, you agree that your contributions will be licensed under
the same license as this project (MIT), unless explicitly stated otherwise.

---

(c) 2025 Team Resurgent
